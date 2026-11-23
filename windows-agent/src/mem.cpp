#include "collect.hpp"
#include <psapi.h>

namespace {
PDH_HCOUNTER standby[3], freezero;
}

void mem_init() {
	standby[0] = pdh_add(L"\\Memory\\Standby Cache Core Bytes");
	standby[1] = pdh_add(L"\\Memory\\Standby Cache Normal Priority Bytes");
	standby[2] = pdh_add(L"\\Memory\\Standby Cache Reserve Bytes");
	freezero = pdh_add(L"\\Memory\\Free & Zero Page List Bytes");
}

void mem_get(std::string& o) {
	MEMORYSTATUSEX ms{sizeof ms};
	PERFORMANCE_INFORMATION pi{};
	if (!GlobalMemoryStatusEx(&ms) || !GetPerformanceInfo(&pi, sizeof pi)) return;
	uint64_t total = ms.ullTotalPhys, avail = ms.ullAvailPhys, page = pi.PageSize;

	double sb = 0, v;
	bool ok = true;
	for (auto c : standby) {
		if (pdh_value(c, v)) sb += v;
		else ok = false;
	}
	uint64_t cached = ok ? static_cast<uint64_t>(sb) : pi.SystemCache * page;
	uint64_t free = pdh_value(freezero, v) ? static_cast<uint64_t>(v) : (avail > cached ? avail - cached : 0);

	o += "mem\t" + std::to_string(total) + "\t" + std::to_string(total - avail) + "\t" + std::to_string(avail) + "\t" + std::to_string(cached)
	     + "\t" + std::to_string(free) + "\t" + std::to_string(pi.CommitTotal * page) + "\t" + std::to_string(pi.CommitLimit * page) + "\n";
}
