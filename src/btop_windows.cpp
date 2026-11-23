/* btop-unified: Windows data provider (btop-agent.exe over pipes, see PROTOCOL.md)

indent = tab
tab-size = 4
*/

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <condition_variable>
#include <csignal>
#include <deque>
#include <fcntl.h>
#include <future>
#include <memory>
#include <mutex>
#include <poll.h>
#include <ranges>
#include <spawn.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

#include "btop_windows.hpp"
#include "btop_config.hpp"
#include "btop_tools.hpp"

extern char** environ;

namespace rng = std::ranges;
using namespace Tools;
using std::clamp;
using std::max;
using std::min;
using std::cmp_greater;
using std::pair;

namespace Win {
	std::atomic<int> cores{0};
	string last_error;

	namespace {
		//? Rejects nan/inf so they never reach clamp() or the process sorter
		double stod_fin(const string& v) {
			const double d = std::stod(v);
			if (not std::isfinite(d)) throw std::invalid_argument("non-finite number");
			return d;
		}

		struct Disk { string mount, fs; uint64_t total{}, free{}, rbps{}, wbps{}; };
		struct Iface { string name, ipv4; bool up{}; uint64_t rx_bps{}, tx_bps{}, rx_total{}, tx_total{}; };
		struct WProc { size_t pid{}, ppid{}; string name, user, cmd; size_t threads{}; uint64_t mem{}, cpu_ms{}, start{}; double cpu{}; int prio{}; };
		struct Snapshot {
			double total{};
			vector<long long> core_pct;
			uint64_t mem_total{}, mem_used{}, mem_avail{}, mem_standby{}, mem_free{};
			vector<Disk> disks;
			vector<Iface> nets;
			vector<WProc> procs;
		};
		using SnapPtr = std::shared_ptr<const Snapshot>;
		struct KillReq { size_t pid; uint64_t start; std::promise<string> reply; };

		//? Shared between threads, guarded by mtx
		std::mutex mtx;
		std::condition_variable cv;
		SnapPtr latest;
		std::deque<KillReq> kill_queue;
		bool want_get{}, quit{}, first_try_done{};
		std::atomic<bool> enabled{false}, stopping{false}, worker_done{false};
		std::thread worker;
		string agent_path;

		//? Runner thread only
		SnapPtr cur;

		//? Worker thread only
		pid_t child = -1;
		int fd_in = -1, fd_out = -1;
		string rbuf;
		size_t rpos = 0;

		vector<string> split_tab(const string& line) {
			vector<string> out;
			for (size_t pos = 0;;) {
				const auto next = line.find('\t', pos);
				out.push_back(line.substr(pos, next - pos));
				if (next == string::npos) return out;
				pos = next + 1;
			}
		}

		//? Names end up on the terminal, never pass control characters through
		string sanitize(string s) {
			for (auto& c : s) if ((unsigned char)c < 0x20 or c == 0x7f) c = '?';
			return s;
		}

		bool write_all(const string& s) {
			for (size_t done = 0; done < s.size();) {
				const auto n = write(fd_in, s.data() + done, s.size() - done);
				if (n < 0 and errno == EINTR) continue;
				if (n <= 0) return false;
				done += n;
			}
			return true;
		}

		bool read_line(string& line, uint64_t timeout_ms) {
			const auto deadline = time_ms() + timeout_ms;
			for (;;) {
				if (auto p = rbuf.find('\n', rpos); p != string::npos) {
					line.assign(rbuf, rpos, p - rpos);
					rpos = p + 1;
					if (rpos > (1 << 16)) { rbuf.erase(0, rpos); rpos = 0; }
					return true;
				}
				if (rbuf.size() - rpos > (1 << 20) or stopping) return false;
				const auto now = time_ms();
				if (now >= deadline) return false;
				pollfd pfd{fd_out, POLLIN, 0};
				const int r = poll(&pfd, 1, (int)min<uint64_t>(100, deadline - now));
				if (r < 0 and errno != EINTR) return false;
				if (r <= 0) continue;
				char buf[65536];
				const auto n = read(fd_out, buf, sizeof(buf));
				if (n < 0 and errno == EINTR) continue;
				if (n <= 0) return false;
				rbuf.append(buf, n);
			}
		}

		void close_agent(bool graceful) {
			if (child <= 0) return;
			if (graceful) write_all("quit\n");
			close(fd_in);
			close(fd_out);
			fd_in = fd_out = -1;
			for (int i = 0; i < 20; i++) {
				const auto r = waitpid(child, nullptr, WNOHANG);
				if (r == child or r < 0) { child = -1; return; }
				sleep_ms(50);
			}
			kill(child, SIGKILL);
			waitpid(child, nullptr, 0);
			child = -1;
		}

		bool start_agent() {
			int in[2], out[2];
			if (pipe2(in, O_CLOEXEC) != 0) return false;
			if (pipe2(out, O_CLOEXEC) != 0) { close(in[0]); close(in[1]); return false; }
			posix_spawn_file_actions_t fa;
			posix_spawn_file_actions_init(&fa);
			posix_spawn_file_actions_adddup2(&fa, in[0], STDIN_FILENO);
			posix_spawn_file_actions_adddup2(&fa, out[1], STDOUT_FILENO);
			posix_spawn_file_actions_addopen(&fa, STDERR_FILENO, "/dev/null", O_WRONLY, 0);
			posix_spawnattr_t attr;
			posix_spawnattr_init(&attr);
			sigset_t none, def;
			sigemptyset(&none);
			sigemptyset(&def);
			sigaddset(&def, SIGPIPE);
			posix_spawnattr_setsigmask(&attr, &none);
			posix_spawnattr_setsigdefault(&attr, &def);
			//? Own process group: terminal signals (ctrl+c/z) must not reach the agent
			posix_spawnattr_setpgroup(&attr, 0);
			posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF | POSIX_SPAWN_SETPGROUP);
			char* argv[] = {agent_path.data(), nullptr};
			const int r = posix_spawn(&child, agent_path.c_str(), &fa, &attr, argv, environ);
			posix_spawn_file_actions_destroy(&fa);
			posix_spawnattr_destroy(&attr);
			close(in[0]);
			close(out[1]);
			if (r != 0) {
				close(in[1]);
				close(out[0]);
				child = -1;
				Logger::info("Windows agent not started (" + agent_path + "): " + strerror(r));
				return false;
			}
			fd_in = in[1];
			fd_out = out[0];
			rbuf.clear();
			rpos = 0;
			string line;
			if (write_all("hello\n") and read_line(line, 10000) and line == "hello\tbtop-agent\t1") {
				Logger::info("Windows agent started: " + agent_path);
				return true;
			}
			Logger::warning("Windows agent handshake failed: " + agent_path);
			close_agent(false);
			return false;
		}

		SnapPtr fetch() {
			if (not write_all("get\n")) return nullptr;
			auto s = std::make_shared<Snapshot>();
			bool got_cpu = false, got_mem = false;
			string line;
			for (int lines = 0; ++lines < 100000;) {
				if (not read_line(line, 5000)) {
					Logger::debug("Windows agent: reply incomplete after " + to_string(lines) + " lines");
					return nullptr;
				}
				if (line == "end") {
					if (got_cpu and got_mem and not s->core_pct.empty()) return s;
					Logger::debug("Windows agent: reply without cpu/mem/cores");
					return nullptr;
				}
				const auto f = split_tab(line);
				const auto& t = f[0];
				if (t == "cpu" and f.size() >= 5) {
					s->total = stod_fin(f[1]);
					got_cpu = true;
				}
				else if (t == "cores" and f.size() >= 2) {
					for (const auto& v : ssplit(f[1], ',')) s->core_pct.push_back(clamp(std::llround(stod_fin(v)), 0ll, 100ll));
					if (s->core_pct.size() > 1024) return nullptr;
				}
				else if (t == "mem" and f.size() >= 8) {
					s->mem_total = stoull(f[1]); s->mem_used = stoull(f[2]); s->mem_avail = stoull(f[3]);
					s->mem_standby = stoull(f[4]); s->mem_free = stoull(f[5]);
					got_mem = s->mem_total > 0;
				}
				else if (t == "disk" and f.size() >= 7)
					s->disks.push_back({sanitize(f[1]), sanitize(f[2]), stoull(f[3]), stoull(f[4]), stoull(f[5]), stoull(f[6])});
				else if (t == "net" and f.size() >= 8)
					s->nets.push_back({sanitize(f[1]), sanitize(f[2]), f[3] == "1", stoull(f[4]), stoull(f[5]), stoull(f[6]), stoull(f[7])});
				else if (t == "proc" and f.size() >= 12) {
					WProc p{stoull(f[1]), stoull(f[2]), sanitize(f[3]), sanitize(f[4]), sanitize(f[11]), stoull(f[5]),
							stoull(f[6]), stoull(f[8]), stoull(f[9]), stod_fin(f[7]), stoi(f[10])};
					//? Like stock btop skipping processes without a readable name; fall back to the executable from cmd
					if (p.name.empty() and not p.cmd.empty()) {
						const auto exe = p.cmd.substr(0, p.cmd.find(p.cmd.front() == '"' ? '"' : ' ', 1));
						p.name = exe.substr(exe.find_last_of("\\/\"") + 1);
					}
					if (p.pid > 0 and p.pid < pid_base and p.ppid < pid_base and not p.name.empty()) s->procs.push_back(std::move(p));
				}
			}
			return nullptr;
		}

		void run() {
			uint64_t backoff = 1000, next_try = 0;
			std::unique_lock lk(mtx);
			while (not quit and not stopping) {
				cv.wait_for(lk, std::chrono::milliseconds(200), [&] {
					return quit or stopping or not kill_queue.empty() or want_get or (enabled and child <= 0 and time_ms() >= next_try);
				});
				if (quit or stopping) break;
				auto kills = std::move(kill_queue);
				kill_queue.clear();
				const bool en = enabled;
				bool do_get = std::exchange(want_get, false);
				lk.unlock();

				bool failed = false;
				if (not en) close_agent(true);
				else if (child <= 0 and time_ms() >= next_try) {
					if (start_agent()) do_get = true;
					else failed = true;
				}

				for (auto& k : kills) {
					string line;
					if (child <= 0) k.reply.set_value(en ? "agent not running" : "Windows source disabled");
					else if (not write_all(fmt::format("kill\t{}\t{}\n", k.pid, k.start)) or not read_line(line, 3000)) {
						k.reply.set_value("agent not responding");
						failed = true;
						break;
					}
					else k.reply.set_value(line == "ok" ? "" : line.starts_with("err\t") ? line.substr(4) : "bad reply: " + line);
				}

				SnapPtr snap;
				if (do_get and child > 0 and not failed) {
					try { snap = fetch(); }
					catch (const std::exception& e) { Logger::debug("Windows agent: bad reply: "s + e.what()); }
					if (not snap) {
						if (not stopping) Logger::warning("Windows agent: no valid reply, restarting");
						failed = true;
					}
				}
				if (failed) close_agent(false);

				lk.lock();
				for (auto& k : kills) try { k.reply.set_value("agent not responding"); } catch (const std::future_error&) {}
				if (snap) {
					latest = std::move(snap);
					backoff = 1000;
				}
				else if (failed or not en) {
					latest = nullptr;
					if (failed) {
						next_try = time_ms() + backoff;
						backoff = min<uint64_t>(backoff * 2, 30000);
					}
				}
				if (do_get or failed) first_try_done = true;
				cv.notify_all();
			}
			lk.unlock();
			close_agent(true);
		}

		int64_t nice_of(int prio) {
			return prio >= 24 ? -20 : prio >= 13 ? -10 : prio >= 10 ? -5 : prio >= 8 ? 0 : prio >= 6 ? 10 : 19;
		}
	}

	void start() {
		if (not v_contains(Cpu::available_fields, "win-total"s)) Cpu::available_fields.push_back("win-total");
		const char* env = std::getenv("BTOP_AGENT");
		agent_path = (env != nullptr and *env != '\0') ? env : Config::getS("windows_agent");
		if (const char* home = std::getenv("HOME"); agent_path.starts_with("~/") and home != nullptr)
			agent_path = home + agent_path.substr(1);
		enabled = Config::getB("windows_source");

		//? Worker blocks all signals (handlers stay on btop's threads, SIGPIPE becomes EPIPE)
		sigset_t all, old;
		sigfillset(&all);
		pthread_sigmask(SIG_BLOCK, &all, &old);
		worker = std::thread([] {
			try { run(); }
			catch (const std::exception& e) { Logger::error("Windows provider stopped: "s + e.what()); }
			worker_done = true;
		});
		pthread_sigmask(SIG_SETMASK, &old, nullptr);
		if (not enabled) return;

		//? Wait briefly for the first snapshot so the first layout already includes Windows
		std::unique_lock lk(mtx);
		want_get = true;
		cv.notify_all();
		cv.wait_for(lk, std::chrono::seconds(3), [] { return latest != nullptr or first_try_done; });
		if (latest) cores = latest->core_pct.size();
	}

	//* May run from a signal handler (clean_quit): never blocks on mtx, gives the worker 2s to send quit and reap the agent.
	//* Otherwise the agent exits on stdin EOF when btop exits.
	void stop() {
		if (not worker.joinable() or stopping.exchange(true)) return;
		cv.notify_all();
		for (int i = 0; i < 200 and not worker_done; i++) sleep_ms(10);
		if (worker_done) worker.join();
		else worker.detach();
	}

	bool refresh() {
		enabled = Config::getB("windows_source");
		{
			std::lock_guard lk(mtx);
			cur = enabled ? latest : nullptr;
			want_get = enabled;
		}
		cv.notify_all();
		const int n = cur ? (int)cur->core_pct.size() : 0;
		if (n == cores) return false;
		Logger::info(n > 0 ? "Windows data available" : "Windows data unavailable");
		cores = n;
		return true;
	}

	void cpu(Cpu::cpu_info& cpu) {
		auto& total = cpu.cpu_percent.at("win-total");
		if (not cur) {
			total.clear();
			cpu.win_core_percent.clear();
			return;
		}
		total.push_back(clamp(std::llround(cur->total), 0ll, 100ll));
		while (cmp_greater(total.size(), Cpu::width * 2)) total.pop_front();
		cpu.win_core_percent.resize(cur->core_pct.size());
		for (size_t i = 0; i < cur->core_pct.size(); i++) {
			auto& core = cpu.win_core_percent[i];
			core.push_back(cur->core_pct[i]);
			while (core.size() > 40) core.pop_front();
		}
	}

	void mem(Mem::mem_info& mem, bool show_disks) {
		static const array<string, 4> names = {"win_used", "win_available", "win_cached", "win_free"};
		static std::unordered_map<string, Mem::disk_info> disks;
		static vector<string> last_keys;
		if (cur) {
			const auto total = cur->mem_total;
			const array<uint64_t, 4> vals = {cur->mem_used, cur->mem_avail, cur->mem_standby, cur->mem_free};
			mem.stats["win_total"] = total;
			for (size_t i = 0; i < names.size(); i++) {
				mem.stats[names[i]] = vals[i];
				auto& percent = mem.percent[names[i]];
				percent.push_back(std::llround((double)vals[i] * 100 / total));
				while (cmp_greater(percent.size(), Mem::width * 2)) percent.pop_front();
			}
		}
		else {
			mem.stats.erase("win_total");
			for (const auto& name : names) {
				mem.stats.erase(name);
				mem.percent.erase(name);
			}
		}

		if (not show_disks) return;
		vector<string> keys;
		const double interval = Config::getI("update_ms") / 1000.0;
		if (cur) {
			for (const auto& d : cur->disks) {
				const string key = "win:" + d.mount;
				auto& disk = disks[key];
				if (disk.name.empty()) {
					disk.name = (d.mount.size() > 1 and d.mount.ends_with('\\')) ? d.mount.substr(0, d.mount.size() - 1) : d.mount;
					disk.fstype = d.fs;
				}
				disk.total = d.total;
				disk.free = min(d.free, d.total);
				disk.used = disk.total - disk.free;
				disk.used_percent = disk.total > 0 ? std::lround((double)disk.used * 100 / disk.total) : 0;
				disk.free_percent = disk.total > 0 ? 100 - disk.used_percent : 0;
				//? Same unit as the Linux collector: bytes per update interval
				for (auto [io, bps] : {pair{&disk.io_read, d.rbps}, pair{&disk.io_write, d.wbps}}) {
					io->push_back(std::llround(bps * interval));
					while (cmp_greater(io->size(), Mem::width * 2)) io->pop_front();
				}
				keys.push_back(key);
			}
		}
		std::erase_if(disks, [&](const auto& d) { return not v_contains(keys, d.first); });
		std::erase_if(mem.disks, [&](const auto& d) { return d.first.starts_with("win:") and not v_contains(keys, d.first); });
		std::erase_if(mem.disks_order, [&](const auto& k) { return k.starts_with("win:"); });
		for (const auto& key : keys) {
			mem.disks[key] = disks.at(key);
			mem.disks_order.push_back(key);
		}
		Mem::disk_ios += keys.size();
		if (keys != last_keys) {
			last_keys = keys;
			Mem::redraw = true;
		}
	}

	bool is_win_iface(const string& iface) { return iface.starts_with("win:"); }

	void net_ifaces(std::unordered_map<string, Net::net_info>& net, vector<string>& interfaces) {
		if (not cur) return;
		for (const auto& n : cur->nets) {
			const string key = "win:" + n.name;
			if (v_contains(interfaces, key)) continue;
			interfaces.push_back(key);
			auto& netif = net[key];
			netif.connected = n.up;
			netif.ipv4 = n.ipv4;
			netif.ipv6.clear();
		}
	}

	void net_stat(const string& iface, bool download, uint64_t& total, uint64_t& speed) {
		total = speed = 0;
		if (not cur) return;
		for (const auto& n : cur->nets) {
			if (iface.compare(4, string::npos, n.name) != 0) continue;
			total = download ? n.rx_total : n.tx_total;
			speed = download ? n.rx_bps : n.tx_bps;
			return;
		}
	}

	bool procs(vector<Proc::proc_info>& procs, vector<size_t>& found, bool per_core, size_t detailed_pid) {
		if (not cur) return false;
		const double mult = per_core ? cur->core_pct.size() : 1;
		const double now = time_s();
		std::unordered_map<size_t, uint64_t> starts;
		for (const auto& wp : cur->procs) starts[wp.pid] = wp.start;
		bool got_detailed = false;
		for (const auto& wp : cur->procs) {
			const size_t key = pid_base + wp.pid;
			found.push_back(key);
			auto it = rng::find(procs, key, &Proc::proc_info::pid);
			if (it == procs.end()) {
				procs.push_back({key});
				it = procs.end() - 1;
			}
			auto& p = *it;
			if (p.cpu_s != wp.start) p = {key}; // pid reused by a new process
			p.name = wp.name;
			p.cmd = wp.cmd;
			p.user = wp.user;
			p.threads = wp.threads;
			p.mem = wp.mem;
			p.cpu_p = clamp(wp.cpu * mult, 0.0, 100.0 * mult);
			p.cpu_c = wp.cpu_ms / 1000.0 / max(1.0, now - wp.start);
			p.cpu_s = wp.start;
			p.cpu_t = wp.cpu_ms;
			p.p_nice = nice_of(wp.prio);
			//? Windows ppids can be stale (parent exited, pid reused): only link to parents older than the child
			const auto parent = starts.find(wp.ppid);
			p.ppid = (wp.ppid != wp.pid and parent != starts.end() and parent->second <= wp.start) ? pid_base + wp.ppid : 0;
			if (key == detailed_pid) got_detailed = true;
		}
		return got_detailed;
	}

	uint64_t mem_total() { return cur ? cur->mem_total : 0; }

	string pid_str(size_t pid) {
		return is_win_pid(pid) ? 'w' + to_string(pid - pid_base) : to_string(pid);
	}

	int send_signal(long long pid, int sig) {
		if (pid < 1) return ESRCH;
		if (not is_win_pid(pid)) return kill((pid_t)pid, sig) == 0 ? 0 : errno;

		if (sig != SIGTERM and sig != SIGKILL and sig != SIGINT) return err_refused;
		const size_t wpid = pid - pid_base;
		std::future<string> reply;
		{
			std::lock_guard lk(mtx);
			if (not latest) {
				last_error = "agent not running";
				return err_agent;
			}
			const auto p = rng::find(latest->procs, wpid, &WProc::pid);
			if (p == latest->procs.end()) return ESRCH;
			kill_queue.push_back({wpid, p->start, {}});
			reply = kill_queue.back().reply.get_future();
		}
		cv.notify_all();
		try {
			if (reply.wait_for(std::chrono::seconds(6)) != std::future_status::ready) last_error = "agent not responding";
			else if (last_error = reply.get(); last_error.empty()) return 0;
		}
		catch (const std::future_error&) { last_error = "agent stopped"; }
		return err_agent;
	}
}
