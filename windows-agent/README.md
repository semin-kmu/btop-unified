# btop-agent

btop-agent.exe is a headless Windows collector that speaks `PROTOCOL.md` v1 over stdin/stdout. It needs no admin rights and does not enable any privilege.

## Build
```
./build.sh        # WSL -> cmd.exe -> build.bat -> vcvars64 -> cl
```
- Toolchain: MSVC 14.43, SDK 10.0.22621. Flags: `/O2 /EHsc /std:c++20 /MT /utf-8 /W4` (0 warnings). Libraries: pdh, iphlpapi, ntdll, powrprof, advapi32.
- Output goes to `bin/btop-agent.exe`. Objects go to `build/`, and compiler temp files go to `build/tmp` (TMP is redirected there).

## Run
```
bin/btop-agent.exe                 # protocol mode (stdin requests)
bin/btop-agent.exe --watch 1000    # print a get frame every N ms
bin/btop-agent.exe --no-nvml       # force the PDH GPU fallback (test)
```

## Sources
| Item | API |
|---|---|
| CPU %, per core | `NtQuerySystemInformation(SystemProcessorPerformanceInformation)`: busy = (kernel+user) - idle, so it matches `% Processor Time` |
| CPU freq | PDH `\Processor Information(_Total)\% Processor Performance` × `MaxMhz` (`CallNtPowerInformation`, 3701 MHz on this machine), which is the "Speed" value Task Manager shows. The first frame, or a PDH failure, uses `CurrentMhz` |
| Memory | `GlobalMemoryStatusEx` + `GetPerformanceInfo`, used = total - available. standby = the sum of PDH `Standby Cache Core/Normal Priority/Reserve Bytes`. free = `Free & Zero Page List Bytes` |
| Disk | Fixed drives: `GetDiskFreeSpaceExW`, `GetVolumeInformationW`. IO: PDH `LogicalDisk(*)\Disk Read/Write Bytes/sec` |
| Net | `GetIfTable2` rows that are also listed by `GetAdaptersAddresses`. Skips loopback, filter interfaces, and non-hardware interfaces that are down (Wi-Fi Direct, Bluetooth PAN). IPv4 comes from GAA |
| GPU | NVML loaded dynamically from System32 (`nvmlDeviceGetMemoryInfo_v2`, so used excludes reserved memory, same as nvidia-smi). Fallback: PDH `GPU Engine` (busiest engine type) + `GPU Adapter Memory\Dedicated Usage`. Missing values are -1 |
| Processes | `SystemProcessInformation`. user comes from the token + `LookupAccountSidW` (cached per SID). cmd comes from `ProcessCommandLineInformation` (60). Both are cached per (pid, create time) |
| kill | Checks pid ≤ 4, self and parent, then the start time (`GetProcessTimes`, same conversion as the `proc` line), then `IsProcessCritical`, then `TerminateProcess(h, 1)` |

## Verification (2026-11-23, Ryzen 5 7500F / 31.6 GiB / RTX 4070 SUPER)
| Item | Agent | Reference | Result |
|---|---|---|---|
| CPU total / per core | 19.9 %, 25.0 % … per core e.g. `16.9,75.4,21.5,…` | Get-Counter `% Processor Time` 18.2 %, 23.2 % … `16.7,79.9,13.6,…` | Agree (1-second windows differ by about 0.3 s) |
| CPU freq | 4450 MHz | `% Processor Performance` 122 % × 3701 = 4515 | Agree. The `Processor Frequency` counter always shows the 3701 nominal value |
| Mem total | 33946566656 | TotalVisibleMemorySize 33150944 KiB = same bytes | Exact |
| Available / free / standby / commit | 17.46 G / 3.29 G / 14.17 G / 25.14 G of 36.09 G | 17.39 G (FreePhysicalMemory is the same) / 3.24 G / 14.16 G / 25.27 G of 36.09 G | Agree (taken a few seconds apart) |
| Processes | 312 entries | Get-Process 313 (includes Idle, which the protocol excludes) | Sets equal apart from Idle and short-lived processes |
| WS / threads | svchost 924 ws=6672384 thr=1, lsass ws=36954112 thr=11 | Get-Process 6672384/1, 36950016/11 | Agree |
| cmdline / user | explorer, wezterm-gui, wslhost: same strings as CIM. Service processes give cmd=name, user empty | CIM CommandLine is empty for the same services (no admin) | Agree |
| GPU | util 6-8 %, used 1324.5 MiB / 12282 MiB, 37 °C, 39.8 W / 245 W, 2640 / 10501 MHz | nvidia-smi 7 %, 1325 / 12282 MiB, 37, 39.48 W / 245 W, 2640 / 10501 | Agree |
| GPU (PDH fallback) | 1397 MB dedicated, util 10 % (3D engine) | PDH Dedicated Usage 1397 MB | Agree (the engine metric differs from NVML util) |
| Net | Ethernet rx/tx ≈ 7-14 kB/s / 118-125 kB/s, vEthernet(WSL) rx ≈ 33 kB/s | Get-Counter Network Interface/Adapter, same values at matching timestamps | Agree |
| Disk C: | total 999109423104, free 131406958592, NTFS | Win32_LogicalDisk, same values | Exact (free changes slightly between samples) |
| kill | `kill\tpid` → `err bad request`; wrong start → `err pid reused`; correct start → `ok`, and the self-started PING.EXE exits with code 1 | | OK |
| Framing | 21 frames: field counts and section order are correct, no CR, `quit`/EOF → exit 0 | | OK |

The sum of per-process `cpu%` is about 1 percentage point below the total cpu. This is because DPC and interrupt time is not charged to any process, and many small values round down.

## Performance (1 request/s, 60 s)
- `get` latency: median 9.0 ms, p95 10.6 ms, max 11.0 ms. The first `get` takes about 0.5 s because it includes process start-up and NVML init.
- Agent CPU: 375 ms in 60 s, which is 0.62 % of one core or 0.05 % of the whole machine.

## License
Apache-2.0, same as btop. Parts of the Windows collection logic are adapted from
[btop4win](https://github.com/aristocratos/btop4win) (Apache-2.0, Copyright 2021 Aristocratos).
