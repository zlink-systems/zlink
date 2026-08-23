# C / C++ perf runner inventory gate 검증 (2026-08-23)

> 대상 계획서: `doc/perf/perf/bindings-0.12.0/bindings-library-performance-improvement-plan-core-0.12.0.ko.md`
> (§3 측정 크기, §4 측정 전 inventory gate)
>
> 범위: 정적 코드/스크립트 분석(read-only). 실측 실행은 하지 않았다.
>
> 기준 commit: `9855b8f57d1b4f421ba54b2c860120bc07c85c68` (branch
> `codex/bindings-0.12.0-performance`)

## 1. 개요

`bindings/c/perf`(single/multi)와 `bindings/cpp/perf`(single/multi) 두 official
runner의 pattern·transport·message size·client 수·CLI option을 소스 레벨에서
직접 대조했다. 확인 방법은 다음과 같다.

- runner 진입 스크립트(`run_benchmarks.sh`, `run_benchmarks_multi.sh`,
  `run_binding_single.sh`, `run_binding_multi.sh`)의 CLI 파싱과 기본값을 읽는다.
- 실제 loop(패턴·transport·size 순회)를 구동하는 Python driver
  (`single/run_comparison.py`, root `run_comparison.py`)의 기본값 상수를 읽는다.
- 각 option이 최종적으로 C/C++ perf 소스에서 `zlink_ctx_set`/`setsockopt` 계열
  호출로 이어지는지 file:line 단위로 추적한다.

**결론(요약)**

| gate | 판정 |
|------|------|
| runner inventory (C vs C++, pattern/transport/size/clients) | **미통과** |
| Multi size 정책 (4 KiB 추가 + MULTI_STREAM 예외) | **통과** |
| 무시되는 runner option | **미통과** |
| (참고) memory guard | C: skip 방식, C++: cap 방식 — 정책과 다름, 별도 기록 |

세부 사유는 §6 gate 판정에 정리한다.

---

## 2. C runner inventory (`bindings/c/perf`)

### 2.1 Single suite (`run_benchmarks.sh` + `single/run_comparison.py`)

| 항목 | 값 | 근거 |
|------|-----|------|
| pattern | `PAIR, PUBSUB, DEALER_DEALER, DEALER_ROUTER, DEALER_ROUTER_REQREP, ROUTER_ROUTER, ROUTER_ROUTER_REQREP` (7개) | `run_benchmarks.sh:184` `STANDARD_PATTERNS` |
| transport | 기본 `tcp, tls, ws, wss, inproc` (+Linux는 `ipc`) | `single/run_comparison.py:51-54` `DEFAULT_SOCKET_TRANSPORTS` |
| message size | `64, 256, 1024, 65536, 131072, 262144` bytes (6개, STREAM 없음) | `single/run_comparison.py:49` `DEFAULT_MSG_SIZES_STANDARD` |
| client 수 | N/A (1:1 socket pair, `--clients` 개념 없음) | `bench_common_runtime.hpp:596-607` |
| STREAM 지원 | 없음(single suite는 STREAM을 다루지 않음) | `STANDARD_PATTERNS`에 STREAM 부재, PERF_MULTI_TEST_POLICY.md:793 "STREAM 소켓은 multi suite에서만 테스트" |

CLI option: `--pattern`, `--build-dir`, `--reuse-build`, `--clean-build`,
`--output`, `--results-dir`, `--results-tag`, `--runs`, `--duration`, `--hwm`,
`--send-hwm`, `--recv-hwm`, `--buf`, `--sndbuf`, `--rcvbuf`, `--sndtimeo`
(`--send-timeout-ms`), `--rcvtimeo`(`--recv-timeout-ms`), `--pin-cpu`,
`--io-threads`, `--msg-sizes`, `--transports`, `--auto-hwm-profile`,
`--core-version` (`run_benchmarks.sh:216-255` usage 블록).

### 2.2 Multi suite (`run_benchmarks_multi.sh` + root `run_comparison.py`)

| 항목 | 값 | 근거 |
|------|-----|------|
| pattern | `DEALER_DEALER, DEALER_ROUTER_SENDSEND, ROUTER_ROUTER_SENDSEND, DEALER_ROUTER_REQREP, ROUTER_ROUTER_REQREP, PUBSUB, STREAM` (7개, `MULTI_` prefix로 report) | `run_benchmarks_multi.sh:52` `PATTERNS` |
| transport | `tcp, tls, ws, wss` (4개, inproc/ipc 미포함) | `run_benchmarks_multi.sh:53` `TRANSPORTS="tcp,tls,ws,wss"` |
| message size (non-STREAM) | `64, 256, 1024, 4096, 65536, 131072` bytes | `run_benchmarks_multi.sh:54` `DEFAULT_MULTI_MSG_SIZES` |
| message size (MULTI_STREAM) | `64, 256, 1024, 65536` bytes | `run_benchmarks_multi.sh:745` `STREAM_MSG_SIZES` |
| 기본 client 수 | non-STREAM 100, STREAM 10000 | `run_benchmarks_multi.sh:733-734` `EFFECTIVE_DEFAULT_CLIENTS`/`EFFECTIVE_DEFAULT_STREAM_CLIENTS` |
| memory guard | budget 초과 시 **해당 pattern skip**(client 수 축소 아님) | `run_benchmarks_multi.sh:247-330` `ensure_memory_budget()`; 실패 시 `record_skip`, `run_benchmarks_multi.sh:1332-1333` |
| nofile guard | `required = clients*3+4096`; 부족 시 `ulimit` 상향 시도 후 실패하면 skip | `run_benchmarks_multi.sh:186-244` |

CLI option: `--transports`, `--msg-sizes`, `--pattern`, `--core-version`,
`--results-tag`, `--runs`, `--results-dir`, `--build-dir`, `--reuse-build`,
`--clean-build`, `--output`, `--pin-cpu`, `--io-threads`,
`--server-io-threads`, `--client-io-threads`, `--clients`, `--hwm`,
`--send-hwm`, `--recv-hwm`, `--buf`, `--sndbuf`, `--rcvbuf`, `--sndtimeo`,
`--rcvtimeo`, `--connect-concurrency`, `--transport-transition-ms`,
`--pattern-transition-ms`, `--server-ready-timeout-ms`,
`--connect-ready-timeout-ms`, `--server-shutdown-timeout-ms`,
`--server-bind-port`, `--auto-hwm-profile`, `--monitor-hwm-bytes`
(`run_benchmarks_multi.sh:521-593` usage 블록).

`ROUTER_ROUTER_MATCHED`는 `--pattern`으로 선택 가능한 정식 pattern이 아니라
`PERF_MULTI_MATCHED_BASELINE=1`일 때만 `run_comparison.py`가 내부적으로 바꿔
끼우는 진단용 baseline client(`perf_multi_router_router_matched_client.cpp`)다.
`resolve_multi_build_targets()`(`run_benchmarks_multi.sh:655-690`)에도
case가 없어 공식 7-pattern inventory에는 포함하지 않는다.

---

## 3. C++ runner inventory (`bindings/cpp/perf`)

`bindings/cpp/perf/single/run_benchmarks.sh`, `multi/run_benchmarks.sh`는
각각 `run_binding_single.sh`, `run_binding_multi.sh`로 exec하는 1줄 wrapper다.
실제 옵션 파싱과 기본값은 두 `run_binding_*.sh`에 있고, pattern/transport/size
loop 자체는 C와 동일하게 Python driver(`single/run_comparison.py`, root
`run_comparison.py`)가 담당한다.

### 3.1 Single suite (`run_binding_single.sh` + `single/run_comparison.py`)

| 항목 | 값 | 근거 |
|------|-----|------|
| pattern(shell 목록) | `PAIR, PUBSUB, DEALER_DEALER, DEALER_ROUTER, DEALER_ROUTER_REQREP, ROUTER_ROUTER, ROUTER_ROUTER_REQREP, SPOT` (8개 나열) | `run_binding_single.sh:54` `STANDARD_PATTERNS` |
| pattern(실제 실행 가능) | 위 7개 + **SPOT은 빌드 대상 없음** | `resolve_single_build_targets()`(`run_binding_single.sh:288-322`)에 `SPOT` case 없음, `single/src`에 `cpp_perf_spot` 바이너리 없음 |
| transport | 기본 `tcp, tls, ws, wss, inproc` (+Linux `ipc`) — C와 동일 | `single/run_comparison.py:52-54` |
| message size | `64, 256, 1024, 65536, 131072, 262144` — C와 동일 | `single/run_comparison.py:50-51` |
| client 수 | N/A — C와 동일 | 1:1 pair |

CLI option 목록(`run_binding_single.sh:81-119` usage 블록)은 C single과 이름·
기본값이 사실상 동일: `--pattern`, `--output`, `--hwm`, `--send-hwm`,
`--recv-hwm`, `--buf`, `--sndbuf`, `--rcvbuf`, `--sndtimeo`, `--rcvtimeo`,
`--pin-cpu`, `--io-threads`, `--msg-sizes`, `--transports`,
`--auto-hwm-profile` 등.

### 3.2 Multi suite (`run_binding_multi.sh` + root `run_comparison.py`)

| 항목 | 값 | 근거 |
|------|-----|------|
| pattern(shell 기본 목록) | `DEALER_DEALER, DEALER_ROUTER_SENDSEND, DEALER_ROUTER_REQREP, ROUTER_ROUTER_SENDSEND, ROUTER_ROUTER_REQREP, PUBSUB, SPOT, SPOT_REQREP, SPOT_SENDSEND, STREAM` (10개 나열, **기본값**) | `run_binding_multi.sh:17` `PATTERNS` |
| pattern(Python driver가 실제로 아는 pattern) | `DEALER_DEALER, DEALER_ROUTER_SENDSEND, DEALER_ROUTER_REQREP, ROUTER_ROUTER_SENDSEND, ROUTER_ROUTER_REQREP, PUBSUB, STREAM` (7개) — **SPOT 3종 없음** | `run_comparison.py:88-96` `MULTI_COMPARISONS`; `SPOT_CONTROL_PATTERNS = ()`(`run_comparison.py:40`, 빈 tuple) |
| transport | 기본 `tcp, tls, ws, wss` — C와 동일 | `run_binding_multi.sh:18` `TRANSPORTS="tcp,tls,ws,wss"` |
| message size (non-STREAM) | `64, 256, 1024, 4096, 65536, 131072` — C와 동일 | `run_binding_multi.sh:19` `DEFAULT_MULTI_MSG_SIZES` |
| message size (MULTI_STREAM) | `64, 256, 1024, 65536` — C와 동일 | `run_binding_multi.sh:357` `STREAM_MSG_SIZES` |
| 기본 client 수 | non-STREAM 100, STREAM 10000 — C와 동일 | `perf_common_multi.hpp:88-92` `resolve_multi_default_clients()`; `run_binding_multi.sh:344-345` |
| memory guard | budget 초과 시 **기본 client 수를 자체 상한(memory_max_clients)까지 축소**(C는 skip) | `run_binding_multi.sh:944-964`: `if EFFECTIVE_DEFAULT_CLIENTS > memory_max_clients: EFFECTIVE_DEFAULT_CLIENTS = memory_max_clients` |

CLI option 목록은 C multi와 이름이 대부분 동일:
`--pattern`, `--output`, `--pin-cpu`, `--io-threads`, `--server-io-threads`,
`--client-io-threads`, `--clients`, `--hwm`, `--send-hwm`, `--recv-hwm`,
`--buf`, `--sndbuf`, `--rcvbuf`, `--sndtimeo`, `--rcvtimeo`,
`--connect-concurrency`, `--transport-transition-ms`,
`--pattern-transition-ms`, `--server-ready-timeout-ms`,
`--connect-ready-timeout-ms`, `--server-shutdown-timeout-ms`,
`--server-bind-port`, `--auto-hwm-profile`, `--monitor-hwm`,
`--msg-sizes`, `--transports`, `--runs` 등(`run_binding_multi.sh:189-246`).

**중대 결함(직접 재현 확인)**: `run_binding_multi.sh`는 `--pattern`을 생략하거나
`--pattern ALL`을 주면 `PATTERN_LIST`(SPOT 3종 포함 10개)를 그대로
`EXPLICIT_PATTERNS`에 넣고(`run_binding_multi.sh:427-428, 966-969`), sentinel
문자열 `"ALL"`이 아니라 展開된 CSV를 Python에 넘긴다
(`run_binding_multi.sh:1129-1141`). Python 쪽 `known_patterns`은
`MULTI_COMPARISONS`(SPOT 미포함) 기준이므로(`run_comparison.py:4690-4699`),
**기본 실행(무인자 또는 `--pattern ALL`)이 다음 오류로 즉시 실패한다**:

```
Error: unsupported patterns: SPOT, SPOT_REQREP, SPOT_SENDSEND
```

즉 C++ multi runner는 현재 상태로는 "기본 ALL 실행"이 동작하지 않는다.
SPOT/SPOT_REQREP/SPOT_SENDSEND를 `--pattern` 목록에서 제외해야만 실행된다.

---

## 4. 대조 결과 (C vs C++)

| 항목 | C single | C++ single | 일치 | C multi | C++ multi | 일치 |
|------|----------|------------|------|---------|-----------|------|
| pattern 목록 | 7개 | 7개 실행 가능 + SPOT 1개(빌드 대상 없음, 사실상 무시됨) | 실행 가능 pattern은 **일치**, 목록 문구에 미구현 SPOT이 남아있음 | 7개(STREAM 포함) | 7개(SPOT 3종은 Python이 모름) 표기상 10개 | **불일치** — 기본 pattern 목록 자체가 다르고, 기본 실행이 깨짐 |
| transport | tcp/tls/ws/wss/inproc(+ipc) | 동일 | 일치 | tcp/tls/ws/wss | 동일 | 일치 |
| message size | 64,256,1K,64K,128K,256K | 동일 | 일치 | 64,256,1K,4K,64K,128K | 동일 | 일치 |
| MULTI_STREAM size | 64,256,1K,64K | 동일 | 일치 | 64,256,1K,64K | 동일 | 일치 |
| 기본 client 수 | 100 / stream 10000 | 동일 | 일치 | 100 / stream 10000 | 동일 | 일치 |
| memory guard 방식 | 초과 시 pattern skip | 초과 시 client 수 **cap** | **불일치** | (동일 행, 위와 같음) | | |
| CLI option 이름 | 위 목록 | 거의 동일(이름·기본값) | 대체로 일치 | 위 목록 | 거의 동일 | 대체로 일치 |
| `--buf`/`--sndbuf`/`--rcvbuf` 실제 적용 | 적용됨(gated) | **미적용(no-op)** | **불일치** | 적용됨(gated) | **미적용(no-op)** | **불일치** |

### 4.1 pattern 불일치 상세

- **C++ single의 `SPOT`**: `STANDARD_PATTERNS`에는 있지만
  `resolve_single_build_targets()`에 case가 없어 빌드 대상이 생성되지 않는다.
  `--pattern ALL`은 Python driver의 7-pattern 표(`PATTERN_TO_BINARY`)를 쓰므로
  `ALL` 실행 자체는 영향받지 않지만, 목록에 실행 불가능한 항목이 남아있는 것은
  정리 대상이다.
- **C++ multi의 `SPOT`, `SPOT_REQREP`, `SPOT_SENDSEND`**: 정책 문서
  (`doc/perf/PERF_MULTI_TEST_POLICY.md`)에는 `MULTI_SPOT_PUBSUB`,
  `MULTI_SPOT_REQREP`, `MULTI_SPOT_SENDSEND`가 Core 9 SpotNode 기반의 정식
  multi pattern으로 문서화되어 있으나, **C 공식 runner에는 SPOT 계열이 전혀
  구현되어 있지 않다**(`run_benchmarks_multi.sh`에 SPOT 문자열 없음). 계획서
  §4 규칙("공식 C에만 있고 binding runner에 없는 pattern은 제외")의 역방향
  케이스로, §5 "binding에 C와 같은 pattern이 없으면 같은 측정 의미로 binding
  perf만 추가한다"에 해당해 SPOT 자체는 policy 위반이 아니다. 문제는
  **C++ multi runner가 이 SPOT 항목을 default 목록에 넣어두고도 Python
  driver가 이를 인식하지 못해 기본 실행이 실패한다는 점**이다. 이는 C와의
  pattern inventory 대조 이전에 **C++ runner 자체의 기능 결함**이다.
  paired 측정을 시작하려면 `--pattern` 값에서 SPOT 3종을 제외한 7-pattern
  CSV를 명시적으로 지정해야 한다(그렇게 하면 C의 7-pattern과 정확히 일치).

### 4.2 memory guard 정책 불일치

계획서 §4는 "C multi runner의 memory guard가 기본 client 수를 줄였으면 그
결과는 paired 비교에 사용하지 않는다. binding runner에도 같은 종류의 cap이
있으면 동일하게 적용한다"고 규정한다. 그런데 C와 C++ multi runner의 guard
**메커니즘 자체가 다르다**:

- C(`run_benchmarks_multi.sh:300-333` `ensure_memory_budget`): 초과 시
  **해당 pattern 전체를 skip**한다. client 수를 줄여서 실행하지 않는다.
- C++(`run_binding_multi.sh:944-964`): 초과 시 **기본 client 수 자체를
  memory_max_clients로 낮춰서** 그대로 실행한다("Info: memory guard capped
  default clients ..." 로그 출력).

두 runner가 같은 `--clients` 값을 요청받았을 때 C는 skip, C++는 축소된
client 수로 계속 실행할 수 있다. 이 상태에서 얻은 C++ 결과의 client 수가 C의
client 수와 다르면 그 값은 paired 비교에 쓸 수 없다(계획서 §4 규칙). 측정
전에는 반드시 두 report의 실제 client 수가 같은지 확인해야 한다.

---

## 5. Multi size 정책 검증

계획서 §3.2 표(`bindings-library-...ko.md:251-263`)의 요구사항:

> multi 기본 크기에 64, 256, 1K, **4K(신규)**, 64K, 128K를 포함하고,
> `MULTI_STREAM`은 64, 256, 1K, 64K만 사용하며 4096/131072는 "해당 없음"으로
> 시작한다.

검증 결과 — **통과**.

| 대상 | 4 KiB 포함 여부 | MULTI_STREAM 예외 반영 여부 |
|------|-----------------|------------------------------|
| C multi 기본 size (`run_benchmarks_multi.sh:54`) | 포함(`64,256,1024,4096,65536,131072`) | 별도 `STREAM_MSG_SIZES="64,256,1024,65536"` (`run_benchmarks_multi.sh:745`)로 4096/131072 제외 |
| C++ multi 기본 size (`run_binding_multi.sh:19`) | 포함(동일 값) | 동일하게 별도 `STREAM_MSG_SIZES`(`run_binding_multi.sh:357`)로 4096/131072 제외 |
| Python driver 상수 (양쪽 `run_comparison.py`) | `DEFAULT_MULTI_MSG_SIZES=[64,256,1024,4096,65536,131072]` | `DEFAULT_MULTI_STREAM_MSG_SIZES=[64,256,1024,65536]`로 분리 |
| `doc/perf/PERF_MULTI_TEST_POLICY.md:819-821` | `[64,256,1024,4096,65536,131072]` | `MULTI_STREAM: [64,256,1024,65536]` |
| 이 계획서 §3.2 표 | 4 KiB 신규 추가, 256 KiB 제거 | MULTI_STREAM의 4096/131072 셀은 "해당 없음"으로 시작 | 

4개 소스(C runner, C++ runner, `doc/perf` 정책, 이 계획서 표) 모두 일치한다.
`--msg-sizes`를 명시적으로 지정하면 STREAM에도 같은 값이 적용되지만
(`run_benchmarks_multi.sh:1086-1088`, `run_binding_multi.sh:689-691`),
**기본값(옵션 없이 실행)에서는 두 runner 모두 예외가 정확히 반영**되어 있다.

---

## 6. 무시되는 runner option 검증

계획서 §4는 "`--output`, `--pin-cpu`, I/O thread, HWM, buffer, timeout
option"의 적용 여부를 측정 전에 확인하라고 요구한다(원문은 .NET single을
예로 들지만, C/C++에도 동일 기준을 적용해 확인했다).

| Option | C single | C multi | C++ single | C++ multi | 판정 |
|--------|----------|---------|------------|-----------|------|
| `--output` | `tee` 로 콘솔 출력 캡처 (`run_benchmarks.sh:1086-1091`) | 동일 (`run_benchmarks_multi.sh:1541-1546`) | 동일 (`run_binding_single.sh:731-736`) | 동일 (`run_binding_multi.sh:1145-1150`) | **적용됨** (perf 소스가 아니라 shell/tee 계층에서 적용되는 것이 설계 의도) |
| `--pin-cpu` | `taskset -c 1 ...`로 실행 wrap (`single/run_comparison.py:1398,2816`) | 동일 (`run_comparison.py:1398,2816`) | 동일 (`single/run_comparison.py:686-695`) | 동일 (`run_comparison.py:1657,3176`) | **적용됨** (OS-level, perf `.cpp`에는 없음 — 설계상 정상) |
| I/O thread 수 (`--io-threads`/`--server-io-threads`/`--client-io-threads`) | `PERF_IO_THREADS` → `zlink_ctx_set(ZLINK_IO_THREADS, ...)` (`bench_common.hpp:137-140`, `bench_common_runtime.hpp:49-59`) | `bench_io_threads()` → 동일 API (`perf_multi_runtime.hpp:51-54,399-409`) | `apply_ctx_options()` → `options.io_threads(...)` (`perf_single_common.cpp:220-225`) | `bench_io_threads()` → `options.io_threads(...)` (`perf_common.hpp:86-89,153-155`) | **적용됨**(4곳 모두) |
| HWM (`--hwm`/`--send-hwm`/`--recv-hwm`) | `PERF_SINGLE_ALLOW_MANUAL_SOCKET_OVERRIDES=1`일 때만 `ZLINK_OPT_SNDHWM/RCVHWM` 적용, 그 외에는 auto-HWM만 작동 (`bench_common_runtime.hpp:250-255,358-369`) | 동일 gate 방식 (`perf_multi_runtime.hpp:542-566`) | 동일 gate 방식 (`perf_single_common.cpp:187-192,262-270`) | 동일 gate 방식 (`perf_common.hpp:193-202`) | **적용됨(단, manual override flag 없이는 no-op — 문서화된 debug-only 설계)** |
| buffer 크기 (`--buf`/`--sndbuf`/`--rcvbuf`) | manual override flag 하에 `ZLINK_OPT_SNDBUF/RCVBUF` 설정 (`bench_common_runtime.hpp:463-483`) | 동일 (`perf_multi_runtime.hpp:630-684`) | **`perf::options::socket_option` enum에 sndbuf/rcvbuf 자체가 없음**(`common/perf_socket_options_adapter.hpp:13-23`); `PERF_SINGLE_SNDBUF`/`RCVBUF`는 Python "Effective Options" 표시용으로만 읽힘(`single/run_comparison.py:992-993,1002-1003`) — **소켓에 실제로 설정되지 않음** | **동일 결함**: `set_common_socket_option_impl`(`multi/common/perf_socket_option_helpers.hpp:26-59`)에 sndbuf/rcvbuf case 없음. `run_comparison.py:4084-4085`에서 표시 용도로만 읽힘 | **C++는 무시됨(ignored) — gate 위반** |
| timeout (`--sndtimeo`/`--rcvtimeo`) | 무조건 적용, `ZLINK_OPT_SNDTIMEO/RCVTIMEO` (`bench_common_runtime.hpp:180-188,481-482`) | 동일 (`perf_multi_runtime.hpp:568-585,642-651`) | 무조건 적용 (`perf_single_common.cpp:157-165,284-294`) | 동일 (`perf_common_multi.hpp:126-127`, `perf_common.hpp:204-213`) | **적용됨**(4곳 모두) |

### 6.1 확정된 무시(ignored) option

**C++ single/multi 양쪽 모두 `--buf`, `--sndbuf`, `--rcvbuf` option이 CLI
에서는 정상적으로 파싱되고 성공(exit 0)으로 처리되지만, 실제 socket에는
전혀 적용되지 않는다.** `perf::options::socket_option` enum
(`bindings/cpp/perf/common/perf_socket_options_adapter.hpp:13-23`)에
`sndbuf`/`rcvbuf` 항목 자체가 없어서, single의
`apply_single_benchmark_socket_options()`
(`bindings/cpp/perf/single/common/perf_single_common.hpp:171-185`)와 multi의
`set_common_socket_option_impl()`
(`bindings/cpp/perf/multi/common/perf_socket_option_helpers.hpp:26-59`) 둘 다
`linger`/`sndhwm`/`rcvhwm`/`sndtimeo`/`rcvtimeo`만 처리하고 buffer 옵션은
아예 건드리지 않는다. `PERF_SINGLE_SNDBUF`/`PERF_SINGLE_RCVBUF`,
`PERF_MULTI_SNDBUF`/`PERF_MULTI_RCVBUF` 값은 Python driver의 "Effective
Options" 로그 출력에만 쓰이고 zlink socket에는 반영되지 않는다.

C 쪽은 동일 option이 `bench_single_manual_socket_overrides_allowed()` 게이트
하에서 실제로 `ZLINK_OPT_SNDBUF`/`ZLINK_OPT_RCVBUF`에 적용되므로(C
single: `bindings/c/perf/single/common/bench_common_runtime.hpp:463-483`, C
multi: `bindings/c/perf/multi/common/perf_multi_runtime.hpp:630-684`), 이는
**C와 C++ 사이의 조건 정렬을 깨는 실제 결함**이며, 계획서 §4가 금지하는
"지원하지 않는 CLI option을 runner가 성공으로 받아들인 뒤 무시" 사례에 정확히
해당한다.

### 6.2 참고: `--monitor-hwm-bytes`(C) / `--monitor-hwm`(C++)

C++ 조사 subagent가 최초에는 "적용 위치 미발견"으로 보고했으나, 재확인 결과
C·C++ 모두 실제로 적용된다.

- C: `bench_hwm_from_env("PERF_MONITOR_HWM_BYTES", 4096000)` →
  `set_sockopt_u64(monitor, ZLINK_OPT_SNDHWM/RCVHWM, ...)`
  (`bindings/c/perf/multi/common/perf_common.hpp:161-166,191-196`).
- C++: `parse_positive_uint64_env("PERF_MULTI_MONITOR_HWM", 4096000)`
  (`bindings/cpp/perf/multi/common/perf_common_multi.hpp:128`)로 읽힌 뒤
  monitor socket에 적용된다.

따라서 이 option은 무시되는 목록에서 제외한다.

---

## 7. gate 판정 (통과 / 미통과)

### 7.1 runner inventory gate — **미통과**

사유:

1. **C++ multi runner의 기본 실행이 깨져 있다.** `--pattern` 생략 또는
   `--pattern ALL` 실행 시 `PATTERN_LIST`에 `SPOT, SPOT_REQREP,
   SPOT_SENDSEND`가 포함되지만, 실제 loop을 구동하는
   `bindings/cpp/perf/run_comparison.py`의 `MULTI_COMPARISONS`
   (`run_comparison.py:88-96`)와 `SPOT_CONTROL_PATTERNS = ()`
   (`run_comparison.py:40`, 빈 tuple)에는 SPOT 계열이 등록되어 있지 않아
   `Error: unsupported patterns: SPOT, SPOT_REQREP, SPOT_SENDSEND`로 즉시
   실패한다. 계획서 §4는 "각 binding의 공식 runner에 실제로 등록된
   pattern만 이 문서의 상세 표와 paired 측정 대상에 포함한다"고 규정하는데,
   현재는 공식 runner의 **기본 실행 자체**가 성립하지 않으므로 대조를 통과로
   판정할 수 없다.
2. C single의 `STANDARD_PATTERNS`에도 실행 불가능한 `SPOT` 항목이 남아있어
   inventory 문구와 실제 실행 가능 목록이 어긋난다(다만 `ALL` 실행 자체는
   영향받지 않는다).
3. C와 C++ multi의 memory guard 메커니즘이 다르다(C: skip, C++: client 수
   cap). 계획서 §4의 memory guard 요구("실제 client 수가 같은지 확인")를
   충족하려면 이 차이를 문서화하고 측정 시마다 실제 client 수 일치를
   개별 확인해야 한다 — 자동으로 보장되지 않는다.

paired 측정을 시작하려면 (a) C++ multi runner의 `run_binding_multi.sh` 기본
`PATTERNS` 목록에서 SPOT 3종을 제거하거나 Python driver에
`MULTI_COMPARISONS`/`SPOT_CONTROL_PATTERNS`를 채워 기본 실행이 성공하도록
고치고, (b) 두 runner의 memory guard 방식 차이를 알고 있는 상태에서 매 실행
report의 실제 client 수를 비교해야 한다.

### 7.2 Multi size 정책 gate — **통과**

C multi, C++ multi, `doc/perf/PERF_MULTI_TEST_POLICY.md`, 이 계획서 §3.2 표
네 곳 모두 기본 non-STREAM size가 `64,256,1024,4096,65536,131072`이고
`MULTI_STREAM`은 `64,256,1024,65536`로 4096/131072가 제외된 것을
일관되게 반영하고 있다(§5 참조).

### 7.3 무시되는 runner option gate — **미통과**

C++ single·multi의 `--buf`/`--sndbuf`/`--rcvbuf` option이 CLI에서는 성공으로
받아들여지지만 `perf::options::socket_option` enum에 대응 항목이 없어 socket에
전혀 적용되지 않는다(§6.1). 계획서 §4의 "지원하지 않는 CLI option을 runner가
성공으로 받아들인 뒤 무시해서는 안 된다. 실제로 적용하거나 명확한 오류로
거부해야 한다" 규정을 위반한다. 이 option이 조건 정렬(예: OS socket buffer
크기를 C와 맞춰야 하는 측정 시나리오)에 필요한 경우 해당 측정을 시작할 수
없다. 나머지 option(`--output`, `--pin-cpu`, I/O thread, HWM, timeout,
`--monitor-hwm*`)은 C·C++ 양쪽에서 실제로 적용됨을 확인했다.

### 7.4 종합

세 gate 중 두 개(inventory, ignored option)가 미통과이므로, 이 시점에서
C++ paired 성능 측정을 시작해서는 안 된다. 우선순위:

1. C++ multi runner 기본 실행 실패(SPOT pattern 불일치) 수정.
2. C++ single/multi의 `--buf`/`--sndbuf`/`--rcvbuf`를 실제로 적용하거나,
   적용할 계획이 없다면 CLI에서 명확한 오류로 거부하도록 수정.
3. C/C++ multi memory guard 방식 차이를 `doc/perf` 정책 또는 이 계획서에
   명시하고, 측정 시 실제 client 수 일치를 매번 확인하는 절차를 추가.
4. 위 수정 후 이 inventory gate를 다시 실행해 네 항목(C runner, C++ runner,
   `doc/perf` 정책, 계획서 상세 표)이 모두 일치함을 재확인한 뒤에만
   paired 측정을 시작한다.

---

## 수정 결과 (2026-08-23)

§7.1(runner inventory)과 §7.3(무시되는 runner option) 두 미통과 gate를 수정
했다. 아래는 수정 내용, 변경 파일, 검증 근거를 정리한 것이다. (memory guard
방식 차이(§4.2, §7.1의 3번)는 이번 수정 범위에 포함하지 않았다 — 별도 정책
결정이 필요하다.)

### 1. C++ multi 기본 pattern 목록 결함 수정 (§7.1 사유 1, 2)

- `bindings/cpp/perf/run_binding_multi.sh`: 기본 `PATTERNS` 변수와 `--help`
  사용법 텍스트에서 `SPOT`, `SPOT_REQREP`, `SPOT_SENDSEND` 3종을 제거했다.
  이제 기본 목록은 C multi(`run_benchmarks_multi.sh`)와 정확히 동일한 7개
  pattern(`DEALER_DEALER,DEALER_ROUTER_SENDSEND,DEALER_ROUTER_REQREP,
  ROUTER_ROUTER_SENDSEND,ROUTER_ROUTER_REQREP,PUBSUB,STREAM`)이다.
- `bindings/cpp/perf/run_binding_single.sh`: `STANDARD_PATTERNS`에서 빌드
  대상이 없던 죽은 `SPOT` 항목을 제거했다(`resolve_single_build_targets()`에
  `SPOT` case가 없고 `cpp_perf_spot` 바이너리도 존재하지 않음을 재확인).

- **SPOT 관련 코드를 완전히 제거**(추가 지시사항): `bindings/cpp/perf`
  전체에서 SPOT_CONTROL_PATTERNS(항상 빈 tuple)에 의존하던 죽은 분기와
  MULTI_SPOT 전용 처리를 정리했다.
  - `bindings/cpp/perf/run_comparison.py`: `SPOT_CONTROL_PATTERNS`,
    `SPOT_TRANSPORTS`와 관련 주석 블록 삭제. `select_transports()`,
    `_resolve_server_timeouts()`의 SPOT 분기 삭제(비-SPOT 경로는 동일하게
    유지). `_auto_hwm_pattern_is_spot()` 함수와 그 유일한 호출부(SPOT 전용
    auto-HWM 테이블 분기) 삭제. `spot_idle_sleep_ms`로만 쓰이던 미사용 지역
    변수 삭제. `run_sizes_test`의 "MULTI_SPOT clean-latency second pass"
    블록과 그 전용 클로저 `run_one_size_case_with_env` 삭제(유일한
    호출자였음). `defer_live_multi_rows()`를 `return False` no-op으로
    단순화. `use_control_plane`/`control_connected` 두 지점은
    `SPOT_CONTROL_PATTERNS` 참조를 없애고 `False`/`True` 상수로 대체했다
    (하위 20여 개 `if use_control_plane:` 분기는 이미 항상 거짓이던 죽은
    코드라 이번에는 상수만 바꾸고 그대로 남겨 두었다 — 모든 패턴이 공유하는
    실행 경로라 그 이상 걷어내는 것은 별도 리팩터로 분리하는 편이 안전
    하다고 판단했다).
  - `bindings/cpp/perf/single/run_comparison.py`: `if "SPOT" in patterns:`
    죽은 timeout 가산 블록 삭제(`parse_pattern_arg()`가 SPOT을 애초에
    통과시키지 않으므로 도달 불가능한 코드였다).
  - `bindings/cpp/perf/multi/common/perf_common.hpp`:
    `bench_max_sockets()`의 `pattern == "SPOT" || pattern == "MULTI_SPOT"`
    분기 삭제(항상 `else` 경로만 타던 죽은 분기).
  - 검증: `grep -rln SPOT bindings/cpp/perf | grep -v '/(results|baseline)/'`
    결과 0건(과거 실행 로그가 보관된 `results/`, `baseline/` 데이터 파일만
    예외 — 코드/스크립트/문서에는 SPOT 잔존 없음).

### 2. `--buf`/`--sndbuf`/`--rcvbuf`를 C++ 소켓에 실제로 적용 (§7.3)

C++ 바인딩 공개 API(`zlink::common_socket_options_t`)에는 애초에
SNDBUF/RCVBUF accessor가 없어 perf 코드가 이 옵션을 적용할 방법이 전혀
없었다(§6.1에서 확인한 근본 원인). raw 소켓 핸들도 공개 API로 노출되지
않아 perf 쪽에서 `zlink_set_option(ZLINK_OPT_SNDBUF, ...)`을 직접 호출할
수도 없었다. 그래서 C의 `HWM`(`send_hwm`/`recv_hwm`)과 동일한 패턴으로
바인딩 공개 API에 `send_buffer()`/`recv_buffer()`를 추가하고, perf 쪽
어댑터가 이를 통해 C와 동일한 gated 방식으로 적용하도록 배선했다.

- `bindings/cpp/src/Runtime/Options/option_ids.hpp`: `socket_option_id`에
  `sndbuf = 12293`(`ZLINK_OPT_SNDBUF`=0x3005), `rcvbuf = 12294`
  (`ZLINK_OPT_RCVBUF`=0x3006) 추가.
- `bindings/cpp/include/zlink/Contracts/Sockets/socket_options.hpp`,
  `bindings/cpp/src/Runtime/Options/socket_options.cpp`:
  `common_socket_options_t::send_buffer()/recv_buffer()`(get/set, `int`)를
  `linger()`와 동일한 패턴으로 추가. 이 두 accessor는 내부적으로
  `zlink_set_option`/`zlink_get_option`에 `ZLINK_OPT_SNDBUF`/
  `ZLINK_OPT_RCVBUF`를 전달한다(C의 `bench_common_runtime.hpp`가 raw
  `ZLINK_OPT_SNDBUF`/`ZLINK_OPT_RCVBUF`를 쓰는 것과 동일한 core 옵션).
- `bindings/cpp/perf/common/perf_socket_options_adapter.hpp`:
  `perf::options::socket_option`에 `sndbuf`/`rcvbuf` 추가,
  `socket_options::sndbuf`/`rcvbuf` 키 상수(`int`) 추가.
- `bindings/cpp/perf/common/perf_socket_adapter.hpp`(single이 쓰는
  `perf::socket_t::set`), `bindings/cpp/perf/single/common/
  perf_single_common.hpp`(제네릭 `SocketLike` 템플릿), `bindings/cpp/perf/
  multi/common/perf_socket_option_helpers.hpp`(multi가 쓰는
  `set_common_socket_option`): 세 곳 모두 `sndbuf`/`rcvbuf` case를 추가해
  `options().send_buffer(value)`/`recv_buffer(value)`로 위임.
- **single**: `bindings/cpp/perf/single/common/perf_single_common.cpp`에
  C의 `parse_single_byte_size_token()`을 그대로 이식한
  `parse_single_byte_size_token()`(b/k/m/g[b] 접미사 지원)과
  `resolve_single_socket_buffer(bool send_)`를 추가했다. 이 함수는 C의
  `bench_single_manual_socket_overrides_allowed()`와 동일한 게이트인
  `single_manual_socket_overrides_enabled()`
  (`PERF_SINGLE_ALLOW_MANUAL_SOCKET_OVERRIDES=1` 또는
  `PERF_ALLOW_MANUAL_SOCKET_OVERRIDES=1`)로 감싸여 있고, 미허용 시 `-1`을
  반환한다. `apply_single_benchmark_socket_options()`(.cpp 비템플릿
  버전과 .hpp 제네릭 템플릿 버전 둘 다)에서 `sndbuf > 0`/`rcvbuf > 0`일
  때만 적용한다 — C의 `apply_single_benchmark_socket_options()`
  (`bench_common_runtime.hpp:463-483`)와 게이트·조건·적용 순서가 동일하다.
- **multi**: `bindings/cpp/perf/multi/common/perf_common_multi.hpp`에 C의
  `parse_byte_size_token()`/`bench_socket_buffer_bytes_from_env()`를 이식
  하고, `multi_bench_settings_t`에 `sndbuf`/`rcvbuf`(`int`, 기본 `-1`)
  필드를 추가했다. `resolve_multi_bench_settings()`는 C의
  `bench_manual_socket_overrides_allowed()`와 동일한 게이트인
  `manual_socket_overrides_enabled()`
  (`PERF_MULTI_ALLOW_MANUAL_SOCKET_OVERRIDES=1` 또는
  `PERF_ALLOW_MANUAL_SOCKET_OVERRIDES=1`) 하에서만
  `PERF_MULTI_SNDBUF`/`PERF_MULTI_RCVBUF`를 읽고, 그 외에는 `-1`이다.
  `bindings/cpp/perf/multi/common/perf_common.hpp`의
  `apply_benchmark_socket_options()`에서 `settings.sndbuf > 0`/
  `settings.rcvbuf > 0`일 때만 적용한다 — C의
  `apply_benchmark_socket_options()`(`perf_multi_runtime.hpp:630-684`)와
  동일하다.
- shell 쪽(`run_binding_single.sh`/`run_binding_multi.sh`)은 이미
  `PERF_SINGLE_SNDBUF`/`PERF_SINGLE_RCVBUF`,
  `PERF_MULTI_SNDBUF`/`PERF_MULTI_RCVBUF`,
  `PERF_SINGLE_ALLOW_MANUAL_SOCKET_OVERRIDES`/
  `PERF_MULTI_ALLOW_MANUAL_SOCKET_OVERRIDES` env var를 올바르게 전달하고
  있었으므로 변경하지 않았다(문제는 항상 C++ perf 소스 쪽의 미구현이었다).

파악한 parity 제약: C++ 공개 바인딩에는 SNDBUF/RCVBUF의 "typed" accessor가
전혀 없었다는 점에서 C와 100% 동일한 구현 경로(raw `zlink_set_option`
직접 호출)를 재사용할 수 없었다. 대신 C의 `send_hwm`/`recv_hwm`이 이미
쓰던 것과 동일한 패턴(공개 바인딩에 typed accessor 추가 → perf가 그
accessor를 호출)으로 완전한 기능적 동등성을 달성했다. 이는 "일부 경로에서
완전한 parity가 불가능하면 CLI가 명확한 오류로 거부해야 한다"는 지시의
회피 조항을 쓸 필요가 없었던 경우다 — 실제로 완전히 적용 가능했다.

### 3. 검증

- 빌드: `ZLINK_CORE_SOURCE=local bash bindings/cpp/perf/run_binding_single.sh`
  / `run_binding_multi.sh`로 `bindings/cpp/build`(CMake, `-DZLINK_CPP_BUILD_
  BENCHMARKS=ON`, `-DZLINK_CPP_USE_CORE_BUILD_RUNTIME=ON`, core는 로컬
  `core/build` 재사용)를 구성 및 빌드 — 전체 single/multi perf 타겟(신규
  `send_buffer`/`recv_buffer` 심볼 포함)이 오류 없이 컴파일됨을 확인했다.
  `nm bindings/cpp/build/libzlink_cpp.a | grep send_buffer`로 심볼 존재
  확인.
- **(a) multi 기본 pattern 목록 결함 해소**:
  `python3 -c`로 `run_comparison.py`를 import해 `run_binding_multi.sh`의
  새 기본 `PATTERNS`(SPOT 3종 제외 7개)를 `MULTI_COMPARISONS` +
  `STREAM_VARIANT_PATTERNS`와 대조 → `unsupported: set()`로 전부 지원됨을
  확인. 실제로 `run_binding_multi.sh --pattern DEALER_DEALER --transports
  tcp --msg-sizes 64 --duration 1 --clients 4 --runs 1`을 실행해
  `status: complete`, `RESULT,...`5줄 정상 출력을 확인했다(과거처럼
  `Error: unsupported patterns: SPOT, ...`로 즉시 실패하지 않음).
- **(b) `--sndbuf`/`--rcvbuf` 실제 적용**:
  1. `PERF_MULTI_ALLOW_MANUAL_SOCKET_OVERRIDES=1`로
     `run_binding_multi.sh --pattern DEALER_DEALER --transports tcp
     --sndbuf 131072 --rcvbuf 65536 ...`를 실행 — "Effective Options"에
     `sndbuf: 131072`/`rcvbuf: 65536`가 표시되고 `status: complete`로
     정상 종료됨을 확인(표시 자체는 수정 전에도 있었으므로 이것만으로는
     불충분).
  2. **실제 소켓 반영 증명**: `perf_common.hpp`/`perf_single_common.hpp`를
     그대로 링크하는 임시 검증 프로그램(수정 후 삭제)을 작성해 —
     - multi: `PERF_MULTI_ALLOW_MANUAL_SOCKET_OVERRIDES=1`,
       `PERF_MULTI_SNDBUF=131072`, `PERF_MULTI_RCVBUF=65536` 설정 후
       `perf::multi::apply_benchmark_socket_options()`를 dealer 소켓에
       적용하고 `socket.options().send_buffer()/recv_buffer()`로 즉시
       재조회 → `socket send_buffer=131072`, `socket recv_buffer=65536`
       확인.
     - single: `PERF_SINGLE_ALLOW_MANUAL_SOCKET_OVERRIDES=1`,
       `PERF_SINGLE_SNDBUF=128k`, `PERF_SINGLE_RCVBUF=64k`(단위 접미사
       파서 경로) 설정 후 `perf::single::apply_single_benchmark_socket_
       options()`를 pair 소켓에 적용 → `send_buffer=131072`,
       `recv_buffer=65536` 확인(k 접미사가 1024배로 정확히 환산됨).
     - 두 경우 모두 core socket에 실제로 `ZLINK_OPT_SNDBUF`/
       `ZLINK_OPT_RCVBUF`가 설정되었음을 `get_option`(재조회) 값 일치로
       증명했다. 검증에 쓴 임시 `.cpp` 파일과 바이너리는 저장소 밖
       scratchpad에서 컴파일했고 작업 완료 후 삭제했다 — 저장소에는
       흔적이 남지 않는다.
  3. override 게이트가 없을 때(`PERF_MULTI_ALLOW_MANUAL_SOCKET_OVERRIDES`
     미설정)는 기존과 동일하게 `sndbuf: -1`/`rcvbuf: -1`(적용 안 함)임을
     최초 smoke 실행에서 재확인했다 — 게이트 없이 값이 새어 나가지
     않는다.
- `python3 -m py_compile`로 두 `run_comparison.py`(root, single) 모두
  구문 오류 없음을 확인했다.

### 4. 이번 수정에서 다루지 않은 항목

- §4.2/§7.1의 3번(C: pattern skip vs C++: client 수 cap이라는 memory guard
  메커니즘 차이)은 이번 defect 수정 범위 밖이다 — 별도 정책 결정 후 수정이
  필요하다.
- `bindings/cpp/perf/run_comparison.py`의 `use_control_plane`을 참조하던
  하위 20여 개 `if use_control_plane:`/`control_connected` 분기 자체는
  삭제하지 않고 상수(`False`/`True`)로 대체하는 선에서 정리했다 — 모든
  multi pattern이 공유하는 실행 경로라, 분기 전체를 걷어내는 리팩터는
  이번 defect 수정보다 위험 대비 효용이 낮다고 판단해 범위에서 제외했다.
  향후 SPOT 계열 pattern을 다시 추가하지 않는 한 이 죽은 분기들은 실행되지
  않는다.

## Core runtime 선택 규약 통일 (2026-08-23)

### 정책

`bindings/c/perf/run_benchmarks.sh` / `run_benchmarks_multi.sh`가 이미 쓰던
core-runtime 선택 규약을 모든 바인딩의 perf runner에 동일하게 적용했다.

- **기본값**: 별도 옵션 없이 실행하면 현재 워크스페이스의 로컬 core
  빌드(`core/build`, `ZLINK_CORE_SOURCE=local` 의미)를 사용한다.
- **`--core-version X.Y.Z`**: 검증된 release runtime을 내려받아 사용한다
  (`bindings/tools/local_core_runtime.sh`의 `fetch-release.sh` 경로,
  `ZLINK_CORE_SOURCE=release` + `ZLINK_CORE_RELEASE_VERSION=X.Y.Z` +
  `ZLINK_CORE_ALLOW_VERSION_MISMATCH=1`).
  - `MAJOR.MINOR.PATCH` 형식이 아니면 즉시 에러.
  - 같은 값으로 두 번 주는 것은 허용하지만 서로 다른 값으로 두 번 주면
    에러("--core-version may be specified only once").
- **우선순위**: 사용자가 명시적으로 설정한 `ZLINK_CORE_SOURCE` 환경변수가
  `--core-version` 없이 주어진 기본값(local)보다 우선한다. 단
  `--core-version`과, `release`가 아닌 값으로 명시된 `ZLINK_CORE_SOURCE`를
  동시에 주면 충돌 에러를 낸다.
- 이 core-version 파싱/검증/우선순위 블록은 각 스크립트에서
  `bindings/tools/local_core_runtime.sh`를 `source`하기 **직전**에 실행되어,
  해당 공용 스크립트가 원래 갖고 있던 기본값(`ZLINK_CORE_SOURCE:-release`)을
  각 러너 안에서 `local`로 오버라이드한다. 공용 스크립트 자체는 수정하지
  않았다(`tests`/`samples`/`build`도 같은 파일을 sourcing하기 때문).

### 수정한 파일 (14개, 언어별 single/multi 쌍)

- `bindings/cpp/perf/run_binding_single.sh`, `run_binding_multi.sh`
  (`run_benchmarks*.sh`, `single|multi/run_benchmarks.sh`는 이 두 파일로
  위임하는 얇은 wrapper라 별도 수정 불필요)
- `bindings/rust/perf/run_benchmarks.sh`, `run_benchmarks_multi.sh`
- `bindings/node/perf/single/run_benchmarks.sh`,
  `bindings/node/perf/multi/run_benchmarks.sh`
- `bindings/dotnet/perf/single/run_benchmarks.sh`,
  `bindings/dotnet/perf/multi/run_benchmarks.sh`
- `bindings/java/perf/single/run_benchmarks.sh`,
  `bindings/java/perf/multi/run_benchmarks.sh`
- `bindings/python/perf/single/run_benchmarks.sh`,
  `bindings/python/perf/multi/run_benchmarks.sh`
- `bindings/go/perf/run_benchmarks.sh`, `run_benchmarks_multi.sh`

`core/`, `bindings/c/`는 건드리지 않았다.

### C 규약과 구조가 달라 별도 처리한 바인딩

- **Node**: 쉘 래퍼가 `bindings/tools/local_core_runtime.sh`를 source한 뒤
  나머지 인자를 그대로 `node dist-tools/.../run_benchmarks.js`로 전달하는
  구조다. JS 쪽 `parseCommonArgs`는 알 수 없는 옵션을 만나면
  `unsupported argument`로 즉시 에러를 내므로, `--core-version`은 쉘
  래퍼 단계에서 파싱·검증한 뒤 node로 넘기는 인자 목록에서 제거(strip)
  했다. `.ts`/`dist-tools` 파일은 건드리지 않았다. `--help` 경로에서는
  쉘 래퍼가 자체적으로 짧은 안내 문구를 출력한 뒤 node의 기존 usage를
  그대로 보여주도록 했다.
- **Python**: 기존에는 core 소스 선택 개념 자체가 없었고,
  `ZLINK_LIBRARY_PATH` 환경변수를 사용자가 반드시 명시적으로 지정해야
  했다(미지정 시 에러). 이번 작업으로 `ZLINK_LIBRARY_PATH`가 비어 있을 때만
  `local_core_runtime.sh`를 source해 로컬 core를 기본값으로 채우도록
  바꿨고, `--core-version`을 주면 release 경로로 채운다. 사용자가
  `ZLINK_LIBRARY_PATH`를 직접 지정한 경우는 그 값이 항상 최우선(가장
  명시적인 override)이며, 이 경우 `--core-version`과 함께 쓰면 충돌
  에러를 낸다. `argparse`가 알 수 없는 옵션에 엄격히 에러를 내므로
  `--core-version`은 Node와 동일하게 쉘 래퍼 단계에서 strip했다.
- **Go**: `bindings/tools/local_core_runtime.sh`를 전혀 사용하지 않고,
  `include/zlink.h`에서 파싱한 SONAME과 `bindings/go/native/<platform-arch>/`
  디렉터리(현재 워크스페이스 core가 미리 복사되어 있는 디렉터리)만 사용하는
  구조였다 — 코드 주석에 "다른 workstream의 root VERSION이 이 러너를 다른
  네이티브 페이로드로 흔들지 못하게 한다"고 명시되어 있어, 동시에 진행 중인
  core 디버깅과의 격리가 의도된 설계였다. 이 격리를 지키기 위해
  **기본 동작(플래그 없음)은 전혀 손대지 않았고**, `--core-version`이 주어질
  때만 `local_core_runtime.sh`를 source해 release 패키지를 받아
  `GO_NATIVE_DIR_OVERRIDE`(release 패키지의 `lib/` 디렉터리)를
  `prepare_core_runtime()`이 플랫폼별 서브디렉터리 매핑 없이 그대로 쓰도록
  분기를 추가했다.

### 검증 (계측 실행 없음, 정적 검증 + `--help`/인자 파싱만)

- 위 14개 파일 전체 `bash -n` 통과.
- 각 파일에서 `ZLINK_CORE_SOURCE=release` / `ZLINK_CORE_SOURCE=${ZLINK_CORE_SOURCE:-local}`
  export가 `source .../local_core_runtime.sh` **이전**에 나타나는지
  `grep`으로 확인 완료(14/14).
- 모든 파일에서 `--help`(또는 python/go는 인자 없이 `-h`)가 정상적으로
  옵션 목록/새 `--core-version` 문서를 출력함을 확인.
- 모든 파일에서 다음 오류 경로를 실제로 트리거해 메시지를 확인:
  - `--core-version 1.2`(형식 오류) → `must be MAJOR.MINOR.PATCH`.
  - `--core-version`을 서로 다른 값으로 두 번(cpp/dotnet/go에서 대표
    확인) → `may be specified only once`.
  - `ZLINK_CORE_SOURCE=local` 환경에서 `--core-version` 병행 → 충돌 에러.
  - python은 `ZLINK_LIBRARY_PATH`를 명시적으로 준 상태에서
    `--core-version` 병행 → "explicit ZLINK_LIBRARY_PATH" 충돌 에러.
- cpp/node/python/go에서 캐시된 release 패키지(0.12.0)를 대상으로
  `--core-version 0.12.0`을 준 뒤 `--help`(또는 node/python은
  `PYTHON_EXECUTABLE=/bin/true` 등으로 실측정 실행 없이) 경로를 실행해,
  release 경로가 실제로 `bindings/tools/local_core_runtime.sh`의
  fetch-release 캐시를 찾아 core를 정상 해석함을 확인했다(네트워크 접근
  없이 로컬 캐시만 사용).
- python single 검증 중 실수로 `--pattern PAIR --duration 1` 짧은 실측정
  1건을 로컬/릴리스 각 1회 실행했다(스크립트 wiring 확인이 목적이었으나
  실제 계측이 수행됨을 인지함). 이후로는 모든 검증을 `--help`/인자 오류
  경로 또는 `PYTHON_EXECUTABLE=/bin/true` 같은 실행 스텁으로만 진행했고,
  다른 13개 파일에서는 실측정 벤치마크를 실행하지 않았다.
