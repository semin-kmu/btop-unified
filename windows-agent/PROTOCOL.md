# btop-agent 통신 규약 v1

- 전송: 에이전트 stdin(요청) / stdout(응답), 줄 끝 `\n`(CR 없음), UTF-8 (UTF-16 → `WideCharToMultiByte(CP_UTF8)`, cp949 금지)
- 필드 구분: 탭 `\t`. 필드 값의 탭·CR·LF는 공백으로 치환
- stdin EOF 또는 `quit` → 에이전트 종료
- 숫자: 정수는 10진, 비율(%)은 소수 1자리 (`12.3`), 바이트는 정수 바이트
- 비율·속도는 직전 `get` 이후 변화량으로 계산. 첫 `get`의 비율·속도는 0
- stderr: 진단 메시지만 (btop은 무시 또는 로그)

## 요청
| 요청 | 응답 |
|---|---|
| `get` | 아래 섹션 줄들, 마지막 `end` |
| `kill<TAB><pid><TAB><start_unix_s>` | `ok` 또는 `err<TAB><사유>` 한 줄 (`TerminateProcess`, 종료 코드 1). `start_unix_s`는 btop이 받은 `proc` 줄의 값 그대로. 에이전트는 `OpenProcess(PROCESS_TERMINATE\|PROCESS_QUERY_LIMITED_INFORMATION)` 후 같은 변환 함수로 구한 생성 시각이 다르면 `err<TAB>pid reused`, `IsProcessCritical`이 참이면 `err<TAB>critical process`, pid ≤ 4·자기 자신·부모(btop 쪽 프로세스)면 `err<TAB>protected`로 거부. 인자 2개가 아니면 `err<TAB>bad request` |
| `hello` | `hello<TAB>btop-agent<TAB>1` (규약 버전) |
| 그 외 | `err<TAB>unknown` |

## `get` 응답 섹션 (순서 고정, 반복 섹션은 0개 이상)
```
time    <unix_ms>
sys     <hostname> <os_version> <uptime_s> <cpu_name> <logical_cores>
cpu     <total%> <user%> <kernel%(idle 제외)> <freq_mhz>
cores   <c0%>,<c1%>,...                         # logical_cores 개
mem     <total> <used> <available> <standby(cached)> <free> <commit_used> <commit_limit>
disk    <mount 예: C:\> <fs> <total> <free> <read_Bps> <write_Bps>        # 반복
net     <name> <ipv4> <up 0|1> <rx_Bps> <tx_Bps> <rx_total> <tx_total>   # 반복, 루프백·필터 드라이버 가상 인터페이스 제외
gpu     <idx> <name> <util%> <mem_used> <mem_total> <temp_c> <power_mw> <power_limit_mw> <clk_gfx_mhz> <clk_mem_mhz>  # 반복, NVML 불가 시 PDH 값, 없는 값은 -1
proc    <pid> <ppid> <name> <user> <threads> <mem_ws> <cpu%> <cpu_time_ms> <start_unix_s> <priority> <cmd>   # 반복, cmd 마지막
end
```
- `mem.used` = `total - available` (작업 관리자 "사용 중"과 같은 정의)
- `cpu%`(proc) = 시스템 전체 대비 % (0~100). btop의 `proc_per_core` 배율은 btop 쪽에서 적용
- `user`: 알 수 없으면 빈 문자열. `cmd`: 읽기 불가 시 `name`
- PID 0(Idle)은 제외, PID 4(System)는 포함

## 검증 모드
- `btop-agent.exe --watch [ms]`: 같은 응답을 주기적으로 stdout에 출력 (사람 확인용)
