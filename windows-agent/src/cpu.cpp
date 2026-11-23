#include "collect.hpp"
#include <winternl.h>
#include <powerbase.h>

extern "C" NTSTATUS NTAPI RtlGetVersion(PRTL_OSVERSIONINFOW);

namespace {
struct PowerInfo {  // PROCESSOR_POWER_INFORMATION (not in user-mode headers)
	ULONG Number, MaxMhz, CurrentMhz, MhzLimit, MaxIdleState, CurrentIdleState;
};
struct Times {
	uint64_t idle = 0, kernel = 0, user = 0;  // kernel includes idle
};

int ncores = 0;
std::vector<Times> prev;
PDH_HCOUNTER perf = nullptr;  // % Processor Performance (relative to base clock)
ULONG max_mhz = 0;

std::vector<PowerInfo> power_info() {
	std::vector<PowerInfo> pi(static_cast<size_t>(ncores));
	if (CallNtPowerInformation(ProcessorInformation, nullptr, 0, pi.data(), static_cast<ULONG>(pi.size() * sizeof(PowerInfo))) != 0) pi.clear();
	return pi;
}

std::string cpu_name() {
	wchar_t buf[256];
	DWORD sz = sizeof buf;
	if (RegGetValueW(HKEY_LOCAL_MACHINE, L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0", L"ProcessorNameString",
	                 RRF_RT_REG_SZ, nullptr, buf, &sz) != ERROR_SUCCESS)
		return "unknown";
	std::string s = utf8(buf);
	auto b = s.find_first_not_of(' '), e = s.find_last_not_of(' ');
	return b == std::string::npos ? "unknown" : s.substr(b, e - b + 1);
}
}  // namespace

void cpu_init() {
	SYSTEM_INFO si;
	GetSystemInfo(&si);
	ncores = static_cast<int>(si.dwNumberOfProcessors);
	perf = pdh_add(L"\\Processor Information(_Total)\\% Processor Performance");
	if (auto pi = power_info(); !pi.empty()) max_mhz = pi[0].MaxMhz;
}

void sys_get(std::string& o) {
	static const std::string fixed = [] {
		wchar_t host[256];
		DWORD n = 256;
		if (!GetComputerNameExW(ComputerNamePhysicalDnsHostname, host, &n)) host[0] = 0;
		RTL_OSVERSIONINFOW v{sizeof v};
		RtlGetVersion(&v);
		return clean(utf8(host)) + "\tWindows " + std::to_string(v.dwMajorVersion) + "." + std::to_string(v.dwMinorVersion) + "."
		       + std::to_string(v.dwBuildNumber);
	}();
	static const std::string name = clean(cpu_name());
	o += "sys\t" + fixed + "\t" + std::to_string(GetTickCount64() / 1000) + "\t" + name + "\t" + std::to_string(ncores) + "\n";
}

void cpu_get(std::string& o) {
	std::vector<SYSTEM_PROCESSOR_PERFORMANCE_INFORMATION> sp(static_cast<size_t>(ncores));
	std::vector<Times> cur(sp.size());
	if (NT_SUCCESS(NtQuerySystemInformation(SystemProcessorPerformanceInformation, sp.data(),
	                                        static_cast<ULONG>(sp.size() * sizeof sp[0]), nullptr)))
		for (size_t i = 0; i < sp.size(); i++)
			cur[i] = {static_cast<uint64_t>(sp[i].IdleTime.QuadPart), static_cast<uint64_t>(sp[i].KernelTime.QuadPart),
			          static_cast<uint64_t>(sp[i].UserTime.QuadPart)};

	double tot = 0, idle = 0, user = 0, kern = 0;
	std::string cores = "cores\t";
	for (size_t i = 0; i < cur.size(); i++) {
		double ci = 0, ct = 0;
		if (!prev.empty()) {
			ci = static_cast<double>(cur[i].idle - prev[i].idle);
			ct = static_cast<double>(cur[i].kernel + cur[i].user - prev[i].kernel - prev[i].user);
			user += static_cast<double>(cur[i].user - prev[i].user);
			kern += static_cast<double>(cur[i].kernel - prev[i].kernel) - ci;
		}
		tot += ct;
		idle += ci;
		cores += (i ? "," : "") + pct(ct > 0 ? 100.0 * (ct - ci) / ct : 0);
	}
	prev = std::move(cur);

	double mhz = 0, p;
	if (pdh_value(perf, p) && max_mhz) mhz = p * max_mhz / 100.0;
	else if (auto pi = power_info(); !pi.empty()) mhz = pi[0].CurrentMhz;

	auto r = [&](double v) { return pct(tot > 0 ? 100.0 * v / tot : 0); };
	o += "cpu\t" + r(tot - idle) + "\t" + r(user) + "\t" + r(kern) + "\t" + std::to_string(static_cast<long long>(mhz + 0.5)) + "\n";
	o += cores + "\n";
}
