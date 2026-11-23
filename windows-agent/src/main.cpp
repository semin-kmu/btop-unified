#include "collect.hpp"
#include <fcntl.h>
#include <io.h>
#include <pdhmsg.h>
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>

std::string utf8(const wchar_t* w, int n) {
	if (!w) return {};
	if (n < 0) n = static_cast<int>(wcslen(w));
	if (n == 0) return {};
	int len = WideCharToMultiByte(CP_UTF8, 0, w, n, nullptr, 0, nullptr, nullptr);
	std::string s(static_cast<size_t>(len), '\0');
	WideCharToMultiByte(CP_UTF8, 0, w, n, s.data(), len, nullptr, nullptr);
	return s;
}

std::string clean(std::string s) {
	for (auto& c : s)
		if (c == '\t' || c == '\r' || c == '\n') c = ' ';
	return s;
}

std::string pct(double v) {
	char b[16];
	snprintf(b, sizeof b, "%.1f", std::clamp(v, 0.0, 100.0));
	return b;
}

uint64_t now_100ns() {
	static LARGE_INTEGER f = [] { LARGE_INTEGER x; QueryPerformanceFrequency(&x); return x; }();
	LARGE_INTEGER c;
	QueryPerformanceCounter(&c);
	return static_cast<uint64_t>(c.QuadPart / f.QuadPart * 10'000'000 + c.QuadPart % f.QuadPart * 10'000'000 / f.QuadPart);
}

static PDH_HQUERY g_query = nullptr;

PDH_HCOUNTER pdh_add(const wchar_t* path) {
	if (!g_query && PdhOpenQueryW(nullptr, 0, &g_query) != ERROR_SUCCESS) return nullptr;
	PDH_HCOUNTER c = nullptr;
	return PdhAddEnglishCounterW(g_query, path, 0, &c) == ERROR_SUCCESS ? c : nullptr;
}

bool pdh_value(PDH_HCOUNTER c, double& v) {
	PDH_FMT_COUNTERVALUE fv;
	if (!c || PdhGetFormattedCounterValue(c, PDH_FMT_DOUBLE | PDH_FMT_NOCAP100, nullptr, &fv) != ERROR_SUCCESS
	    || fv.CStatus != ERROR_SUCCESS)
		return false;
	v = fv.doubleValue;
	return true;
}

std::vector<std::pair<std::wstring, double>> pdh_array(PDH_HCOUNTER c) {
	std::vector<std::pair<std::wstring, double>> r;
	if (!c) return r;
	DWORD bytes = 0, count = 0;
	if (PdhGetFormattedCounterArrayW(c, PDH_FMT_DOUBLE | PDH_FMT_NOCAP100, &bytes, &count, nullptr) != static_cast<PDH_STATUS>(PDH_MORE_DATA)) return r;
	std::vector<std::byte> buf(bytes);
	auto items = reinterpret_cast<PDH_FMT_COUNTERVALUE_ITEM_W*>(buf.data());
	if (PdhGetFormattedCounterArrayW(c, PDH_FMT_DOUBLE | PDH_FMT_NOCAP100, &bytes, &count, items) != ERROR_SUCCESS) return r;
	for (DWORD i = 0; i < count; i++)
		if (items[i].FmtValue.CStatus == ERROR_SUCCESS) r.emplace_back(items[i].szName, items[i].FmtValue.doubleValue);
	return r;
}

static std::string get() {
	std::string o;
	o.reserve(64 * 1024);
	FILETIME ft;
	GetSystemTimePreciseAsFileTime(&ft);
	uint64_t t = (static_cast<uint64_t>(ft.dwHighDateTime) << 32 | ft.dwLowDateTime) / 10'000 - 11'644'473'600'000ULL;
	o += "time\t" + std::to_string(t) + "\n";
	if (g_query) PdhCollectQueryData(g_query);
	for (auto f : {sys_get, cpu_get, mem_get, disk_get, net_get, gpu_get, proc_get}) {
		try {
			f(o);
		} catch (const std::exception& e) {
			fprintf(stderr, "btop-agent: %s\n", e.what());
		}
	}
	o += "end\n";
	return o;
}

// "kill\t<pid>\t<start_unix_s>"
static std::string kill(const std::string& line) {
	char* e1 = nullptr;
	char* e2 = nullptr;
	const char* p = line.c_str() + 4;
	if (*p != '\t' || !isdigit(static_cast<unsigned char>(p[1]))) return "err\tbad request";
	unsigned long pid = strtoul(p + 1, &e1, 10);
	if (*e1 != '\t' || !(isdigit(static_cast<unsigned char>(e1[1])) || e1[1] == '-')) return "err\tbad request";
	long long start = strtoll(e1 + 1, &e2, 10);
	if (*e2 != '\0') return "err\tbad request";
	return proc_kill(pid, start);
}

static void emit(const std::string& s) {
	fwrite(s.data(), 1, s.size(), stdout);
	fflush(stdout);
}

int main(int argc, char** argv) {
	_setmode(_fileno(stdin), _O_BINARY);
	_setmode(_fileno(stdout), _O_BINARY);
	bool watch = false, nvml = true;
	int interval = 1000;
	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--watch")) {
			watch = true;
			if (i + 1 < argc && atoi(argv[i + 1]) > 0) interval = atoi(argv[++i]);
		} else if (!strcmp(argv[i], "--no-nvml")) nvml = false;  // test PDH GPU fallback
	}
	cpu_init();
	mem_init();
	disk_init();
	gpu_init(nvml);

	if (watch) {
		for (;;) {
			emit(get());
			Sleep(static_cast<DWORD>(interval));
		}
	}
	std::string line;
	while (std::getline(std::cin, line)) {
		while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
		if (line == "get") emit(get());
		else if (line == "hello") emit("hello\tbtop-agent\t1\n");
		else if (line == "quit") break;
		else if (line == "kill" || line.starts_with("kill\t")) emit(kill(line) + "\n");
		else emit("err\tunknown\n");
	}
	return 0;
}
