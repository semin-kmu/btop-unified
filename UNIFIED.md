# btop-unified

btop v1.4.5(`a1099d0`)에 Windows 데이터를 더한 판. WSL 터미널에서 btop 화면 하나에 WSL과 Windows 값을 함께 보여 줍니다.
Windows 값은 `btop-agent.exe`가 보냅니다(통신 규약: `windows-agent/PROTOCOL.md`). 에이전트가 없거나 죽으면 Windows 부분만 숨기고 원본 btop과 같게 동작합니다.

## 바뀐 것

| 위치 | 내용 |
|---|---|
| `src/btop_windows.{hpp,cpp}` | Windows 공급부. `posix_spawn`으로 에이전트 실행(stdin/stdout 파이프, stderr는 `/dev/null`, 별도 프로세스 그룹). 작업 스레드가 runner 주기마다 `get`을 보내고, 받은 스냅샷을 mutex 아래 보관. 수집 함수는 최신 스냅샷만 읽으므로 에이전트가 느려도 화면 갱신이 늦어지지 않음. 응답 없음·깨진 응답·종료 시 에이전트를 정리하고 1s→2s→…→30s 간격으로 재시작 |
| `src/btop.cpp` | `Win::start()`(첫 스냅샷을 최대 3초 기다려 첫 화면부터 Windows 포함), runner 주기 시작 시 `Win::refresh()`(Windows 표시 여부가 바뀌면 박스 크기 재계산), `clean_quit`에서 `Win::stop()`(mutex를 잡지 않음, 시그널 처리기에서도 안전), 첫 실행 시 설정 복사 |
| `src/btop_config.cpp` | 설정 디렉터리 `$XDG_CONFIG_HOME/btop-unified`(없으면 `~/.config/btop-unified`), 로그 `$XDG_STATE_HOME/btop-unified.log`. 새 설정 `windows_source`(기본 True), `windows_agent`(기본 `~/.local/share/btop-unified/btop-agent.exe`) |
| `src/linux/btop_collect.cpp` | 각 collect 끝에 Windows 값을 합치는 호출만 추가 |
| `src/btop_draw.cpp` | 아래 박스별 표시 |
| `src/btop_menu.cpp` | 옵션 메뉴 general에 `Windows source`, 시그널 전송을 `Win::send_signal`로 교체 |

## 박스별 표시

- **CPU**: 위 그래프 = WSL 전체, 아래 그래프 = Windows 전체(`win-total`). 가운데 선 라벨 `wsl ▲▼ win`. 오른쪽 상자 첫 두 줄이 `WSL`/`WIN` 전체 미터(같은 미터), 이어서 WSL 코어 `C0..`, Windows 코어 `W0..`.
- **MEM**: 첫 줄 라벨 `Total:` → `WSL:`, WSL 항목 아래에 `Windows:` 머리줄(총량)과 Used/Available/Cached(standby)/Free. 디스크 목록 뒤에 Windows 드라이브(`C:` 등, 읽기/쓰기 속도 포함).
- **NET**: `b`/`n` 순환 목록에 `win:<이름>` 추가. 속도는 에이전트 값, 합계·그래프·자동 배율은 원본 그대로.
- **PROC**: Windows 프로세스를 한 목록에 합침. PID 열에 `w` 접두어(`w1234`). 정렬·필터(`w`로 시작하는 PID 문자열도 필터 대상)·트리·선택·상세 보기 동작.
- **GPU**: 변경 없음.

## 설계 선택

- **PID 충돌 방지**: Windows 프로세스의 내부 키 = `2^30 + Windows PID`. 리눅스 `pid_max`는 최대 2^22라 겹치지 않음. 2^30 이상인 Windows PID는 받지 않음(기존 `int` 형 선택 PID와 호환).
- **시그널 경로(안전)**: 모든 시그널은 `Win::send_signal(pid, sig)` 하나를 지남. 키 ≥ 2^30이면 에이전트 `kill<TAB>pid<TAB>start_unix_s`만 사용하고 `kill()`은 부르지 않음. 그 외(1 ≤ pid < 2^30)만 리눅스 `kill()`, pid < 1은 거부(원본은 `kill(0/-1)` 가능성이 있었음). Windows에는 TERM/KILL/INT만 보내고 나머지는 "Windows: only TERM, KILL or INT!" 실패 창. 에이전트 거부 사유(`pid reused`, `critical process`, `protected` 등)는 원본 실패 창에 "Windows: <사유>"로 표시.
- **Windows 부모 PID**: Windows는 부모가 끝나고 PID가 재사용될 수 있어, 부모가 자식보다 늦게 시작했으면 부모 없음(트리의 뿌리)으로 처리. Windows 프로세스는 리눅스 트리와 따로 자기 트리를 이룸.
- **이름 없는 프로세스**: 이름이 비면 cmd의 실행 파일 이름 사용, 둘 다 비면 목록에서 뺌(원본이 이름을 못 읽은 프로세스를 빼는 것과 같음). 제어 문자는 `?`로 바꿈.
- **넓은 문자**: Program 열과 트리 이름, NET 제목의 인터페이스 이름은 표시 폭 기준으로 자르고 채움(한글 이름으로 열이 밀리지 않음). 리눅스 이름에도 같이 적용.
- **메모리 %**: Windows 프로세스의 Mem%와 상세 보기 메모리는 Windows 총 메모리 기준.
- **상세 보기**: Windows 프로세스는 상태 "Unknown", 경과 시간은 시작 시각 기준, IO 값 없음, Nice는 Windows 우선순위 근사값(idle 19, below normal 10, normal 0, above normal -5, high -10, realtime -20).

## 옵션 동작

| 옵션 | Windows 있을 때 |
|---|---|
| `windows_source` False | 에이전트 종료, 원본 btop 화면 |
| `cpu_graph_lower` Auto | `win-total` (GPU 정보보다 우선). 다른 값을 지정하면 그 값 |
| `cpu_graph_upper`/`lower`에 `win-total` 지정 | Windows가 없으면 원본 기본값으로 |
| `cpu_single_graph` True | 위 그래프(WSL)만, Windows는 `WIN` 미터·코어 목록에만 |
| `cpu_invert_lower` | Windows 그래프에 적용 |
| `check_temp`/`show_coretemp` | Windows 코어는 온도 없음(빈칸으로 열 맞춤) |
| `proc_per_core` | Windows 프로세스는 Windows 논리 코어 수를 곱함 |
| `show_disks`, `io_mode`, `show_io_stat` | Windows 드라이브에도 적용, 단 IO% 줄은 없음(에이전트가 주지 않음) |
| `disks_filter` | 리눅스 마운트에만 적용 |

- **높이가 부족할 때(MEM)**: 원본처럼 항목을 뒤에서부터 줄이되, WSL과 Windows에 같은 우선순위(Used, Available, Cached, Free 순으로 남김)를 번갈아 적용해 두 쪽이 균형 있게 줄어듦(Windows 쪽이 먼저 한 칸 줄어듦). 한 항목씩도 안 들어가면 원본처럼 들어가는 데까지만 표시.
- **높이가 부족할 때(CPU)**: 코어 칸 수는 원본 계산을 따르되 WSL+Windows 코어와 `WIN` 줄 하나를 포함. 매우 작은 창(예 80x24)에서는 원본처럼 들어가는 코어까지만 보임.

## 설정·경로

- 설정: `~/.config/btop-unified/btop.conf`. 없으면 첫 실행 때 `~/.config/btop/btop.conf`를 읽어서 복사(원본 파일은 쓰지 않음).
- 테마: 원본 검색 경로 그대로(`<바이너리>/../share/btop/themes` 등, 읽기만), 사용자 테마는 `~/.config/btop-unified/themes`.
- 에이전트 경로: `windows_agent` 설정, 환경 변수 `BTOP_AGENT`가 우선.

## 빌드·설치

```
make -j6 GPU_SUPPORT=true                      # bin/btop
install -m 755 bin/btop ~/.local/bin/btop-unified
```
Windows 에이전트는 `windows-agent/README.md` 참고. 빌드한 `btop-agent.exe`를 `~/.local/share/btop-unified/`에 두면 됩니다.

`make install`은 쓰지 마세요. 바이너리 이름이 `btop`이라 기존 btop을 덮어씁니다.

## 알려진 한계

- Windows CPU 온도·전력 없음, Windows 디스크 IO%(활동률) 없음, Windows 프로세스 IO·상태 없음.
- 작은 창에서 CPU 코어·MEM 항목이 원본처럼 잘림.
- 기본 네트워크 인터페이스는 원본 규칙(연결된 것 중 누적 트래픽이 가장 큰 것)이라 Windows 인터페이스가 먼저 선택될 수 있음. `net_iface`로 고정 가능.
- 에이전트가 처음 응답할 때까지 시작이 최대 3초 늦어질 수 있음.
