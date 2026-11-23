#include "collect.hpp"
#include <algorithm>
#include <cwctype>
#include <map>

namespace {
// minimal NVML ABI (nvml.h not required)
using Dev = void*;
struct Util {
	unsigned gpu, memory;
};
struct MemInfo {
	unsigned long long total, free, used;
};
struct MemInfoV2 {  // used excludes driver-reserved memory (same as nvidia-smi)
	unsigned version;
	unsigned long long total, reserved, free, used;
};
int(__cdecl* nvmlInit)();
int(__cdecl* nvmlDeviceGetCount)(unsigned*);
int(__cdecl* nvmlDeviceGetHandleByIndex)(unsigned, Dev*);
int(__cdecl* nvmlDeviceGetName)(Dev, char*, unsigned);
int(__cdecl* nvmlDeviceGetUtilizationRates)(Dev, Util*);
int(__cdecl* nvmlDeviceGetMemoryInfo)(Dev, MemInfo*);
int(__cdecl* nvmlDeviceGetMemoryInfo_v2)(Dev, MemInfoV2*);
int(__cdecl* nvmlDeviceGetTemperature)(Dev, int, unsigned*);
int(__cdecl* nvmlDeviceGetPowerUsage)(Dev, unsigned*);
int(__cdecl* nvmlDeviceGetEnforcedPowerLimit)(Dev, unsigned*);
int(__cdecl* nvmlDeviceGetClockInfo)(Dev, int, unsigned*);

struct Gpu {
	Dev dev;
	std::string name;
};
std::vector<Gpu> gpus;
PDH_HCOUNTER pdh_eng = nullptr, pdh_mem = nullptr;

template <class F>
bool load(HMODULE m, F& f, const char* name) {
	f = reinterpret_cast<F>(reinterpret_cast<void*>(GetProcAddress(m, name)));
	return f != nullptr;
}

bool nvml_init() {
	HMODULE m = LoadLibraryExW(L"nvml.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
	if (!m) return false;
	if (!(load(m, nvmlInit, "nvmlInit_v2") && load(m, nvmlDeviceGetCount, "nvmlDeviceGetCount_v2")
	      && load(m, nvmlDeviceGetHandleByIndex, "nvmlDeviceGetHandleByIndex_v2") && load(m, nvmlDeviceGetName, "nvmlDeviceGetName")
	      && load(m, nvmlDeviceGetUtilizationRates, "nvmlDeviceGetUtilizationRates") && load(m, nvmlDeviceGetMemoryInfo, "nvmlDeviceGetMemoryInfo")
	      && load(m, nvmlDeviceGetTemperature, "nvmlDeviceGetTemperature") && load(m, nvmlDeviceGetPowerUsage, "nvmlDeviceGetPowerUsage")
	      && load(m, nvmlDeviceGetEnforcedPowerLimit, "nvmlDeviceGetEnforcedPowerLimit")
	      && load(m, nvmlDeviceGetClockInfo, "nvmlDeviceGetClockInfo"))
	    || nvmlInit() != 0)
		return false;
	load(m, nvmlDeviceGetMemoryInfo_v2, "nvmlDeviceGetMemoryInfo_v2");
	unsigned n = 0;
	nvmlDeviceGetCount(&n);
	for (unsigned i = 0; i < n; i++) {
		Gpu g{};
		char name[96] = "";
		if (nvmlDeviceGetHandleByIndex(i, &g.dev) != 0) continue;
		nvmlDeviceGetName(g.dev, name, sizeof name);
		g.name = clean(name);
		gpus.push_back(g);
	}
	return !gpus.empty();
}

std::string num(long long v) { return std::to_string(v); }
}  // namespace

void gpu_init(bool allow_nvml) {
	if (allow_nvml && nvml_init()) return;
	pdh_eng = pdh_add(L"\\GPU Engine(*)\\Utilization Percentage");
	pdh_mem = pdh_add(L"\\GPU Adapter Memory(*)\\Dedicated Usage");
}

void gpu_get(std::string& o) {
	for (size_t i = 0; i < gpus.size(); i++) {
		auto d = gpus[i].dev;
		Util u{};
		MemInfo m{};
		unsigned v = 0;
		auto val = [&](int rc) { return rc == 0 ? "\t" + std::to_string(v) : std::string("\t-1"); };
		MemInfoV2 m2{static_cast<unsigned>(sizeof(MemInfoV2) | 2u << 24)};
		bool mok = false;
		if (nvmlDeviceGetMemoryInfo_v2 && nvmlDeviceGetMemoryInfo_v2(d, &m2) == 0) m = {m2.total, m2.free, m2.used}, mok = true;
		else mok = nvmlDeviceGetMemoryInfo(d, &m) == 0;
		o += "gpu\t" + std::to_string(i) + "\t" + gpus[i].name + "\t" + (nvmlDeviceGetUtilizationRates(d, &u) == 0 ? pct(u.gpu) : "-1") + "\t"
		     + num(mok ? static_cast<long long>(m.used) : -1) + "\t" + num(mok ? static_cast<long long>(m.total) : -1);
		o += val(nvmlDeviceGetTemperature(d, 0, &v));
		o += val(nvmlDeviceGetPowerUsage(d, &v));
		o += val(nvmlDeviceGetEnforcedPowerLimit(d, &v));
		o += val(nvmlDeviceGetClockInfo(d, 0, &v));  // graphics
		o += val(nvmlDeviceGetClockInfo(d, 2, &v));  // memory
		o += "\n";
	}
	if (!pdh_eng && !pdh_mem) return;

	// PDH fallback, per adapter LUID: util = busiest engine type (sum over processes), mem = dedicated usage
	auto luid = [](const std::wstring& n) {
		auto b = n.find(L"luid_");
		std::wstring l = b == std::wstring::npos ? std::wstring{} : n.substr(b, 26);  // luid_0xXXXXXXXX_0xXXXXXXXX
		for (auto& c : l) c = towlower(c);
		return l;
	};
	std::map<std::wstring, std::map<std::wstring, double>> eng;  // luid -> engtype -> %
	std::map<std::wstring, double> mem;
	for (auto& [n, v] : pdh_array(pdh_eng)) {
		auto e = n.find(L"engtype_");
		eng[luid(n)][e == std::wstring::npos ? L"" : n.substr(e)] += v;
	}
	for (auto& [n, v] : pdh_array(pdh_mem)) mem[luid(n)] += v;
	for (auto& [l, v] : mem) eng[l];
	int idx = 0;
	for (auto& [l, types] : eng) {
		if (l.empty()) continue;
		double util = 0;
		for (auto& [t, v] : types) util = std::max(util, v);
		auto mi = mem.find(l);
		o += "gpu\t" + std::to_string(idx++) + "\tGPU " + utf8(l.c_str()) + "\t" + pct(util) + "\t"
		     + num(mi == mem.end() ? -1 : static_cast<long long>(mi->second)) + "\t-1\t-1\t-1\t-1\t-1\t-1\n";
	}
}
