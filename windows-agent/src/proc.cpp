#include "collect.hpp"
#include <winternl.h>
#include <map>
#include <unordered_map>

namespace {
// SYSTEM_PROCESS_INFORMATION with the fields winternl.h hides
struct ProcInfo {
	ULONG NextEntryOffset, NumberOfThreads;
	LARGE_INTEGER WorkingSetPrivateSize;
	ULONG HardFaultCount, NumberOfThreadsHighWatermark;
	ULONGLONG CycleTime;
	LARGE_INTEGER CreateTime, UserTime, KernelTime;
	UNICODE_STRING ImageName;
	LONG BasePriority;
	HANDLE UniqueProcessId, InheritedFromUniqueProcessId;
	ULONG HandleCount, SessionId;
	ULONG_PTR UniqueProcessKey;
	SIZE_T PeakVirtualSize, VirtualSize;
	ULONG PageFaultCount;
	SIZE_T PeakWorkingSetSize, WorkingSetSize;
};
constexpr ULONG ProcessCommandLineInformation = 60;
constexpr NTSTATUS StatusInfoLengthMismatch = static_cast<NTSTATUS>(0xC0000004L);

struct Entry {
	int64_t create;
	uint64_t cpu;  // user + kernel, 100 ns
	std::string user, cmd;
	unsigned gen;
};
std::unordered_map<ULONG_PTR, Entry> cache;
std::map<std::string, std::string> sid_names;  // SID bytes -> account name
std::vector<std::byte> buf(1 << 20);
uint64_t prev_t = 0;
unsigned gen = 0;
int ncores = 1;

int64_t unix_s(int64_t filetime) { return filetime / 10'000'000 - 11'644'473'600LL; }

std::string user_of(HANDLE h) {
	HANDLE tok;
	if (!OpenProcessToken(h, TOKEN_QUERY, &tok)) return {};
	std::byte tb[256];
	DWORD len;
	std::string r;
	if (GetTokenInformation(tok, TokenUser, tb, sizeof tb, &len)) {
		PSID sid = reinterpret_cast<TOKEN_USER*>(tb)->User.Sid;
		std::string key(reinterpret_cast<char*>(sid), GetLengthSid(sid));
		auto it = sid_names.find(key);
		if (it == sid_names.end()) {
			wchar_t name[256], dom[256];
			DWORD nn = 256, dn = 256;
			SID_NAME_USE use;
			it = sid_names.emplace(key, LookupAccountSidW(nullptr, sid, name, &nn, dom, &dn, &use) ? clean(utf8(name)) : "").first;
		}
		r = it->second;
	}
	CloseHandle(tok);
	return r;
}

std::string cmd_of(HANDLE h) {
	std::vector<std::byte> b(4096);
	ULONG len = 0;
	NTSTATUS st = NtQueryInformationProcess(h, static_cast<PROCESSINFOCLASS>(ProcessCommandLineInformation), b.data(),
	                                        static_cast<ULONG>(b.size()), &len);
	if (st == StatusInfoLengthMismatch && len > b.size()) {
		b.resize(len);
		st = NtQueryInformationProcess(h, static_cast<PROCESSINFOCLASS>(ProcessCommandLineInformation), b.data(), len, &len);
	}
	if (!NT_SUCCESS(st)) return {};
	auto us = reinterpret_cast<UNICODE_STRING*>(b.data());
	return us->Buffer ? clean(utf8(us->Buffer, us->Length / 2)) : std::string{};
}
}  // namespace

void proc_get(std::string& o) {
	ULONG len = 0;
	NTSTATUS st;
	while ((st = NtQuerySystemInformation(SystemProcessInformation, buf.data(), static_cast<ULONG>(buf.size()), &len)) == StatusInfoLengthMismatch)
		buf.resize(len + (64 << 10));
	if (!NT_SUCCESS(st)) return;
	if (prev_t == 0) {
		SYSTEM_INFO si;
		GetSystemInfo(&si);
		ncores = static_cast<int>(si.dwNumberOfProcessors);
	}
	uint64_t now = now_100ns();
	double span = prev_t ? static_cast<double>(now - prev_t) * ncores : 0;
	prev_t = now;
	gen++;
	FILETIME ft;
	GetSystemTimeAsFileTime(&ft);
	int64_t boot_unix = static_cast<int64_t>((static_cast<uint64_t>(ft.dwHighDateTime) << 32 | ft.dwLowDateTime) / 10'000'000 - 11'644'473'600ULL)
	                    - static_cast<int64_t>(GetTickCount64() / 1000);

	for (auto p = reinterpret_cast<ProcInfo*>(buf.data());; p = reinterpret_cast<ProcInfo*>(reinterpret_cast<std::byte*>(p) + p->NextEntryOffset)) {
		auto pid = reinterpret_cast<ULONG_PTR>(p->UniqueProcessId);
		if (pid != 0) {
			std::string name = p->ImageName.Buffer ? clean(utf8(p->ImageName.Buffer, p->ImageName.Length / 2)) : "System";
			uint64_t cpu = static_cast<uint64_t>(p->UserTime.QuadPart + p->KernelTime.QuadPart);
			auto it = cache.find(pid);
			double cpct = 0;
			if (it != cache.end() && it->second.create == p->CreateTime.QuadPart) {
				if (span > 0 && cpu >= it->second.cpu) cpct = 100.0 * static_cast<double>(cpu - it->second.cpu) / span;
				it->second.cpu = cpu;
			} else {
				Entry e{p->CreateTime.QuadPart, cpu, {}, {}, 0};
				if (HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, static_cast<DWORD>(pid))) {
					e.user = user_of(h);
					e.cmd = cmd_of(h);
					CloseHandle(h);
				}
				if (e.cmd.empty()) e.cmd = name;
				it = cache.insert_or_assign(pid, std::move(e)).first;
			}
			it->second.gen = gen;
			int64_t start = p->CreateTime.QuadPart ? unix_s(p->CreateTime.QuadPart) : boot_unix;
			o += "proc\t" + std::to_string(pid) + "\t" + std::to_string(reinterpret_cast<ULONG_PTR>(p->InheritedFromUniqueProcessId)) + "\t" + name
			     + "\t" + it->second.user + "\t" + std::to_string(p->NumberOfThreads) + "\t" + std::to_string(p->WorkingSetSize) + "\t" + pct(cpct)
			     + "\t" + std::to_string(cpu / 10'000) + "\t" + std::to_string(start) + "\t" + std::to_string(p->BasePriority) + "\t"
			     + it->second.cmd + "\n";
		}
		if (!p->NextEntryOffset) break;
	}
	std::erase_if(cache, [](auto& kv) { return kv.second.gen != gen; });
}

std::string proc_kill(unsigned long pid, int64_t start) {
	static const ULONG_PTR parent = [] {
		PROCESS_BASIC_INFORMATION pbi{};
		return NT_SUCCESS(NtQueryInformationProcess(GetCurrentProcess(), ProcessBasicInformation, &pbi, sizeof pbi, nullptr))
		           ? reinterpret_cast<ULONG_PTR>(pbi.Reserved3)  // InheritedFromUniqueProcessId
		           : 0;
	}();
	if (pid <= 4 || pid == GetCurrentProcessId() || pid == parent) return "err\tprotected";
	HANDLE h = OpenProcess(PROCESS_TERMINATE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
	if (!h) return GetLastError() == ERROR_INVALID_PARAMETER ? "err\tno such process" : "err\taccess denied";
	std::string r;
	FILETIME c, x, k, u;
	BOOL crit = FALSE;
	if (!GetProcessTimes(h, &c, &x, &k, &u)) r = "err\tcannot verify start time";
	else if (unix_s(static_cast<int64_t>(static_cast<uint64_t>(c.dwHighDateTime) << 32 | c.dwLowDateTime)) != start) r = "err\tpid reused";
	else if (!IsProcessCritical(h, &crit) || crit) r = "err\tcritical process";
	else if (!TerminateProcess(h, 1)) r = "err\tterminate failed (" + std::to_string(GetLastError()) + ")";
	else r = "ok";
	CloseHandle(h);
	return r;
}
