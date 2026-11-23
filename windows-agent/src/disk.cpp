#include "collect.hpp"
#include <map>

namespace {
PDH_HCOUNTER rd, wr;
std::map<wchar_t, std::wstring> fs_cache;  // drive letter -> file system name
}

void disk_init() {
	rd = pdh_add(L"\\LogicalDisk(*)\\Disk Read Bytes/sec");
	wr = pdh_add(L"\\LogicalDisk(*)\\Disk Write Bytes/sec");
}

void disk_get(std::string& o) {
	std::map<std::wstring, std::pair<double, double>> io;  // "C:" -> read, write
	for (auto& [n, v] : pdh_array(rd)) io[n].first = v;
	for (auto& [n, v] : pdh_array(wr)) io[n].second = v;

	DWORD mask = GetLogicalDrives();
	for (wchar_t l = L'A'; l <= L'Z'; l++) {
		if (!(mask & (1u << (l - L'A')))) continue;
		wchar_t root[] = {l, L':', L'\\', 0};
		if (GetDriveTypeW(root) != DRIVE_FIXED) continue;
		ULARGE_INTEGER total, free;
		if (!GetDiskFreeSpaceExW(root, nullptr, &total, &free)) continue;
		auto& fs = fs_cache[l];
		if (fs.empty()) {
			wchar_t name[MAX_PATH + 1] = L"";
			GetVolumeInformationW(root, nullptr, 0, nullptr, nullptr, nullptr, name, MAX_PATH + 1);
			fs = name[0] ? name : L"unknown";
		}
		auto [r, w] = io[std::wstring{l, L':'}];
		o += "disk\t" + utf8(root) + "\t" + clean(utf8(fs.c_str())) + "\t" + std::to_string(total.QuadPart) + "\t" + std::to_string(free.QuadPart)
		     + "\t" + std::to_string(static_cast<uint64_t>(r)) + "\t" + std::to_string(static_cast<uint64_t>(w)) + "\n";
	}
}
