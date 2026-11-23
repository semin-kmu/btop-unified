/* btop-unified: Windows data provider (btop-agent.exe over pipes, see PROTOCOL.md)

indent = tab
tab-size = 4
*/

#pragma once

#include <atomic>
#include <string>
#include <vector>

#include "btop_shared.hpp"

namespace Win {
	//? Windows processes are keyed as pid_base + Windows pid, Linux pids are always < pid_base (pid_max <= 2^22)
	constexpr size_t pid_base = size_t{1} << 30;
	constexpr bool is_win_pid(long long pid) { return pid >= (long long)pid_base; }

	//? Codes returned by send_signal() in addition to errno values
	constexpr int err_refused = 100001; // signal other than TERM/KILL/INT for a Windows process
	constexpr int err_agent = 100002;   // agent refused or failed, see last_error
	extern std::string last_error;

	//? Number of Windows cores in the current layout, 0 = Windows parts hidden (stock btop layout)
	extern std::atomic<int> cores;
	inline bool shown() { return cores > 0; }

	void start();
	void stop();

	//* Runner: take the latest agent snapshot for this cycle, returns true if the box layout must be recalculated
	bool refresh();

	//* Merge the current snapshot into the Linux collector results
	void cpu(Cpu::cpu_info& cpu);
	void mem(Mem::mem_info& mem, bool show_disks);
	void net_ifaces(std::unordered_map<string, Net::net_info>& net, vector<string>& interfaces);
	bool is_win_iface(const string& iface);
	void net_stat(const string& iface, bool download, uint64_t& total, uint64_t& speed);
	bool procs(vector<Proc::proc_info>& procs, vector<size_t>& found, bool per_core, size_t detailed_pid);
	uint64_t mem_total();

	string pid_str(size_t pid);

	//* Route a signal: Windows keys only to the agent (TERM/KILL/INT), everything else only to kill(). Returns 0 or error code
	int send_signal(long long pid, int sig);
}
