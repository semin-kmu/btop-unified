#pragma once
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <pdh.h>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

// util (main.cpp)
std::string utf8(const wchar_t* w, int n = -1);
std::string clean(std::string s);  // tab/CR/LF -> space
std::string pct(double v);         // "12.3", clamped 0..100
uint64_t now_100ns();              // monotonic, 100 ns units

// one shared PDH query, collected once per `get` before collectors run
PDH_HCOUNTER pdh_add(const wchar_t* english_path);  // nullptr on failure
bool pdh_value(PDH_HCOUNTER c, double& v);           // false if unavailable (e.g. first sample of a rate)
std::vector<std::pair<std::wstring, double>> pdh_array(PDH_HCOUNTER c);

// init() registers PDH counters; get() appends protocol lines
void cpu_init();
void sys_get(std::string& o);
void cpu_get(std::string& o);
void mem_init();
void mem_get(std::string& o);
void disk_init();
void disk_get(std::string& o);
void net_get(std::string& o);
void gpu_init(bool allow_nvml);
void gpu_get(std::string& o);
void proc_get(std::string& o);
std::string proc_kill(unsigned long pid, int64_t start_unix_s);  // "ok" or "err\t<reason>"
