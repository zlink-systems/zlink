# perf 러너 측정 조건 통일 pass 1 — 2026-09-07

> 범위: 설계 문서 `2026-09-07-runner-parity-design.ko.md` §3.1 (a) 측정 조건 / §5 단계 1 / §6-D3.
> `doc/perf/**`의 정책 문서, 계획서, `decisions.ko.md`, `framework/**`, `core/**`는 수정하지 않았다.
> Core는 `core/build/lib/libzlink.so.0.17.0`(Build ID `af759a1c5532fb7100c6baede89144814200d798`)
> 하나로 고정했고 재빌드하지 않았다. 모든 perf 실행은 완전히 직렬이었다.

---

## 1. 작업 A — D3 확정: **(ii) 보고 시점 문제**

### 1.1 결론

C++ multi report의 server DEALER `4096000`은 **effective HWM 차이가 아니라 보고 시점 차이**다.
두 러너의 실제 effective HWM은 같고(양쪽 `1048576`), **지금까지의 C↔C++ multi paired 결과는
이 항목 때문에 무효가 되지 않는다.**

### 1.2 스펙 근거 (먼저 읽은 순서)

1. `core/doc/spec/core/socket/README.ko.md:357-358`
   — `ZLINK_OPT_SNDHWM`/`ZLINK_OPT_RCVHWM`의 기본값이 **4,096,000 byte**다.
   즉 `4096000`은 "monitor HWM 오적용"이 아니라 **Core socket option 기본값**이다.
2. `core/doc/spec/core/systems/06-auto-hwm.ko.md:113`, `:177`
   — applied HWM은 **physical directional queue(pipe) 단위 속성**이다
   (registry가 "manual HWM, current applied HWM과 accounted byte"를 소유하고,
   `total_applied_hwm_bytes`는 "**live application 방향에** 실제 적용된 HWM 합계").
   → **live application pipe가 하나도 없는 socket에는 적용된 auto-HWM이 존재하지 않는다.**
3. `core/doc/spec/core/06-monitoring.ko.md:382`
   — `auto_hwm_applied_sndhwm_bytes`는 "수동 override 포함 **실제 사용하는** byte HWM".
4. 구현 확인 `core/src/runtime/sockets/common/socket_base_monitor.cpp:56-64`, `:116-123`
   — snapshot은 attached application pipe를 순회해 `applied_out_hwm()`의 max를 담고,
   **pipe를 하나도 못 보면(`application_pipe_seen == false`)**
   `auto_hwm_applied_sndhwm_bytes = options.sndhwm`(= 4,096,000)으로 대체한다.

### 1.3 실측 근거 (스펙이 답하지 않은 "러너에서 실제로 어느 쪽인가")

- 러너 raw 라인(실제 실행에서 채취):
  - client: `sndhwm=1048576 rcvhwm=1048576 rcv_bytes_in_flight=0 role=peer_queue enabled=1`
  - server: `sndhwm=4096000 rcvhwm=4096000 **snd_bytes_in_flight=0 rcv_bytes_in_flight=0**
    role=peer_queue enabled=1 last_recalc_reason=refresh`
  → server row는 auto-HWM plan(`role=peer_queue`)은 이미 있는데 **pipe 기여가 0**이다.
    §1.2-4의 fallback 분기에 정확히 해당한다.
- 같은 C++ 바이너리를 handshake만 수동으로 구동해 snapshot이 첫 payload 도착 뒤에 찍히게 하면
  server row가 `sndhwm=1048576 ... rcv_bytes_in_flight=28416`으로 나온다. 즉 **소켓·옵션은
  그대로이고 채취 시점만 다르다.**
- 보조 확인(독립 C 프로그램, `core/build` 링크): bind DEALER + connect DEALER 100개에서
  `zlink_getsockopt(ZLINK_OPT_SNDHWM)`는 pipe 유무와 무관하게 **항상 4096000**을 돌려주고,
  monitor snapshot의 applied 값만 pipe attach 이후 `1048576`이 된다.
  → **`zlink_getsockopt`은 effective HWM 측정 수단이 아니다**(옵션 값이지 pipe 적용값이 아님).

### 1.4 Core 결함 여부

Core 결함으로 보지 않는다. `auto_hwm_applied_*`는 사양상 "실제 사용하는 byte HWM"이고,
live pipe가 없으면 그 socket에서 실제로 쓰일 값은 socket option 값이 맞다. 다만 **"pipe가 없어
fallback했다"는 사실을 호출자가 구분할 수 있는 field가 없다**(`detail_flags`도 동일). 러너는
`snd/rcv_bytes_in_flight == 0`으로 간접 추정할 수밖에 없다. → 감독자 판단 항목(§5-1).

---

## 2. 작업 B — 수정 항목 (러너별·파일별)

### 2.1 판정 기준 메모

- **정책이 규정한 것은 정책을 따랐다.**
  - `PERF_MULTI_TEST_POLICY.md:1245` `PERF_MULTI_DEFAULT_STREAM_CLIENTS` 기본 **100**
  - `PERF_MULTI_TEST_POLICY.md:1265` `PERF_MULTI_SEND_DRAIN_TIMEOUT_MS` 기본 **5000**
  - `PERF_MULTI_TEST_POLICY.md:988`,`:1264`,`:1353`
    monitor HWM CLI/env는 **C/Go가 `--monitor-hwm-bytes`·`PERF_MULTI_MONITOR_HWM_BYTES`,
    나머지 binding이 `--monitor-hwm`·`PERF_MULTI_MONITOR_HWM`** — 정책이 이 분기를 명시적으로
    허용한다. 따라서 **CLI/env 이름은 통일하지 않았다**(설계 문서 §5 단계1-3의 지시와 다름).
  - `--server-shutdown-timeout-ms` 기본 **5000**(`PERF_MULTI_TEST_POLICY.md` §8.1 CLI 표)
  - `PERF_POLICY.md:1028` Python multi의 io_threads 기본 `1` 예외와 Effective Options 기록 의무
- **정책이 규정하지 않은 것은 C를 준용했다 [정책 미규정 → C 구현 준용].**
  - Effective Options **report key 이름/순서**. `PERF_POLICY.md:1045-1069`는 `lang`,`suite`만
    필수로 규정하고 나머지 key 집합을 정하지 않는다. → C key 집합으로 통일.

### 2.2 C (`bindings/c/perf`)

수정 없음. 이번 pass에서 C가 어긴 정책 조항은 발견하지 못했다(§4에 미수정 관찰 1건).

### 2.3 C++ (`bindings/cpp/perf`)

| 파일 | 변경 |
|---|---|
| `multi/src/perf_dealer_dealer_server.cpp` | auto-HWM snapshot을 START 직후 → **active 수신 중 첫 유효 metric 메시지 1회**로 이동(C `perf_multi_dealer_dealer_server.cpp:136-142`와 동일 지점). D3 (ii)의 직접 수정 |
| `multi/common/perf_common_multi.hpp:22` | `default_send_drain_timeout_ms` `1000` → **5000** (정책 `:1265`) |
| `run_comparison.py` | `routed_echo_borrow_payload` → **`routed_echo_per_socket_payload`**, 대상 패턴을 C와 같은 SENDSEND 2종으로 축소(비-SENDSEND 항목은 normalize 결과 도달 불가한 사문이었다) |
| `run_comparison.py` | `default_stream_clients` `10000` → **100** (정책 `:1245`; 실행값은 원래 100이라 report만 거짓이었다) |
| `run_comparison.py` | `("monitor_hwm", parse_env_int("PERF_MONITOR_HWM", **1000**))` → `("monitor_hwm_bytes", ... **4096000**)`. 기본값 `1000`은 실제 기본값과 다른 단순 버그였다 |
| `run_comparison.py` | `CONTROL_PLANE_PATTERNS = ()` 추가 + `select_transports()`를 C 규칙으로 교체 — **사용자 지정 `--transports` 순서 유지 + base 교집합 필터**(기존은 base 순서로 재정렬) |
| `run_binding_common.sh` | `resolve_configured_core_build_dir()`가 **재configure 이전의 `CMakeCache.txt`** 를 읽던 문제 수정. non-reuse 빌드는 항상 `-DZLINK_CPP_CORE_BUILD_DIR=${DEFAULT_CORE_BUILD_DIR}`로 재configure되므로, cache 값은 `BUILD_MODE == reuse`일 때만 신뢰한다 |

### 2.4 .NET (`bindings/dotnet/perf`)

| 파일 | 변경 |
|---|---|
| `multi/Zlink.BindingBench.Multi/common/PerfCommonMulti.cs:54` | send drain timeout `1000` → **5000** |
| `multi/run_benchmarks.sh` | `routed_echo_borrow_payload` → `routed_echo_per_socket_payload`, **대상 패턴에서 REQREP 제거**(C는 SENDSEND 2종만; 이름뿐 아니라 **의미가 달랐다**) |
| `multi/run_benchmarks.sh` | `monitor_hwm` → `monitor_hwm_bytes`, `fail_fast` 행 제거 |
| `run_comparison.py` | `--runs`/`--pin-cpu`를 하위 suite runner에 **전달**(A-8: 파싱만 하고 버려서 `runs>1`이 무시되었다) |

### 2.5 Java (`bindings/java/perf`)

| 파일 | 변경 |
|---|---|
| `multi/run_benchmarks.sh` | `routed_echo_per_socket_payload` 행 **신규 추가**(C 규칙), `monitor_hwm` → `monitor_hwm_bytes`, `fail_fast` 행 제거 |

### 2.6 Node (`bindings/node/perf`)

| 파일 | 변경 |
|---|---|
| `multi/perf_multi_dealer_dealer_server.ts` | auto-HWM snapshot을 bind 직후 → **첫 유효 payload 수신 1회**로 이동. C++와 동일한 D3 (ii) 결함이었다(server row가 `4096000`이었다) |
| `multi/perf_multi_runtime.ts` | manual override 경로의 HWM fallback `1000` → **C와 같은 `0`(=auto 유지)** 로 교체하고, `PERF_MULTI_HWM != 0` 또는 `PERF_MULTI_SNDHWM/RCVHWM`이 명시된 경우에만 적용하도록 C `apply_benchmark_hwm`(`perf_multi_runtime.hpp:562-585`) 의미를 복제. `hasEnvValue()` 헬퍼 추가 |
| `multi/perf_multi_routed_sendsend.ts:262` | send drain timeout `1000` → **5000** |
| `common/perf_c_emitter.ts` | `routed_echo_per_socket_payload` 추가(+ `SENDSEND_PATTERNS`), `monitor_hwm` → `monitor_hwm_bytes`, `fail_fast` 제거 |
| `dist-tools/**` | 위 TS 변경의 컴파일 산출물(저장소가 추적하는 파일) |

### 2.7 Go (`bindings/go/perf`)

| 파일 | 변경 |
|---|---|
| `multi/perf_multi_dealer_dealer.go` | server: START 수신 뒤 `RecalculateAutoHwm()` 추가(C `:492`와 동일 지점). client: 전 연결 `CONNECTION_READY` 뒤 추가 |
| `multi/perf_multi_dealer_router.go`, `multi/perf_multi_router_router.go` | server: bind 직후 recalc 추가(C relay server `perf_multi_relay_server.hpp:658`와 동일 지점). client: 연결 준비 뒤 recalc 추가 |
| `multi/perf_multi_pubsub.go` | publisher: START 뒤 recalc 추가(C `perf_multi_pubsub_server.cpp:316`). subscriber: 연결 준비 뒤 추가 |
| `multi/perf_multi_socket_reqrep.go` | client context를 `runMultiReqRepClients`에 전달해 연결 준비 뒤 recalc 추가 |
| `run_benchmarks_multi.sh` | `routed_echo_per_socket_payload`·`service_clients` 행 추가, `fail_fast` 제거 |
| `run_benchmarks_multi.sh` | `clients` 표시가 `auto (default=100, stream=100)` → **실제 값 `100`**(C `resolve_clients_meta` 규칙). `META,clients`도 같이 정상화된다 |

Go는 이전에 **STREAM 1곳에서만** auto-HWM을 재계산했다(`PERF_MULTI_TEST_POLICY.md:313-315` 위반).
이제 7개 multi 패턴 전부에서 "target 연결 준비 뒤 재계산"을 수행한다.

### 2.8 Rust (`bindings/rust/perf`)

| 파일 | 변경 |
|---|---|
| `multi/src/perf_common.rs:1112` | send drain timeout `1_000` → **5_000** |
| (report는 §2.9의 공용 `perf_report.py` 경유) | `monitor_hwm` 리터럴 하드코딩 제거로 `--monitor-hwm` override가 report에 반영된다 |

### 2.9 Python + 공용 report (`bindings/python/perf`)

| 파일 | 변경 |
|---|---|
| `perf_report.py` (Rust·공용 `render-multi`) | `default_stream_clients` fallback `10000` → **100**; `monitor_hwm: 4096000` **리터럴 하드코딩** → `monitor_hwm_bytes`(env 실제값); `routed_echo_per_socket_payload`·`service_clients` 추가; `fail_fast` 제거 |
| `multi/run_benchmarks.py` | `routed_echo_per_socket_payload`(신규 헬퍼)·`service_clients` 추가, `smoke`·`fail_fast` report 행 제거, `monitor_hwm` → `monitor_hwm_bytes` |
| `multi/run_benchmarks.py` | **`DEALER_DEALER_SERVER_SHUTDOWN_TIMEOUT_MS = "30000"` 특례 제거.** 정책 기본값은 5000이고, 이는 "timeout을 늘려 통과시키는" 완화에 해당했다. 제거 후에도 smoke는 `status: complete`로 통과한다 |

---

## 3. 작업 C — 검증 결과

모든 실행은 `PERF_FAIL_FAST=1 ZLINK_CORE_SOURCE=local ... --pattern MULTI_DEALER_DEALER
--transports tcp --msg-sizes 64 --duration 1 --runs 1`, **한 번에 하나씩 직렬**. 실행 시 load_avg는
0.19~0.57. 측정 조건(duration/HWM/timeout/client 수)은 어느 것도 완화하지 않았다.

| # | 러너 | report | status | Effective Options vs C |
|---|---|---|---|---|
| 1 | C (기준) | `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260907_093755.txt` | complete | 기준 |
| 2 | C++ | `bindings/cpp/perf/results/multi/report/perf_cpp_multi_linux_20260907_094448.txt` | complete | **`lang` 외 완전 일치** |
| 3 | Go | `bindings/go/perf/results/multi/report/perf_go_multi_linux_20260907_093952.txt` | complete | `go_gomaxprocs`·`go_gomaxprocs_source`·`go_gomaxprocs_case_overrides` 3행만 추가(§5-2) |
| 4 | Rust | `bindings/rust/perf/results/multi/report/perf_rust_multi_linux_20260907_094003.txt` | complete | **`lang` 외 완전 일치** |
| 5 | .NET | `bindings/dotnet/perf/results/multi/report/perf_dotnet_multi_linux_20260907_094015.txt` | complete | **`lang` 외 완전 일치** |
| 6 | Java | `bindings/java/perf/results/multi/report/perf_java_multi_linux_20260907_094039.txt` | complete | **`lang` 외 완전 일치** |
| 7 | Node | `bindings/node/perf/results/multi/report/perf_node_multi_linux_20260907_094241.txt` | complete | **`lang` 외 완전 일치** |
| 8 | Python | `bindings/python/perf/results/multi/report/perf_python_multi_linux_20260907_094200.txt` | complete | `server/client_io_threads: 1 (python default)` 2행만 차이 — **`PERF_POLICY.md:1028`이 명시 허용**하는 예외 |

### 3.1 Auto-HWM detail (MULTI_DEALER_DEALER / tcp / 64B)

| 러너 | client SNDHWM/RCVHWM | server SNDHWM/RCVHWM |
|---|---|---|
| C | 1048576 / 1048576 | 1048576 / 1048576 |
| **C++** | 1048576 / 1048576 | **1048576 / 1048576** (수정 전 4096000) |
| .NET | 1048576 / 1048576 | 1048576 / 1048576 |
| Java | 1048576 / 1048576 | 1048576 / 1048576 |
| **Node** | 1048576 / 1048576 | **1048576 / 1048576** (수정 전 4096000) |
| Go / Rust / Python | `unavailable` | `unavailable` (§4-3) |

**C↔C++는 Effective Options와 Auto-HWM detail 표가 모두 일치한다.**

### 3.2 자동 테스트

| 대상 | 결과 |
|---|---|
| `bindings/go` `go build ./perf/...` + `go test ./perf/...` | **통과** |
| `bindings/python/tests/test_perf_multi_runner.py` (38건) | **통과** |
| `bindings/python/tests/test_perf_monitor_hwm.py`, `test_perf_runner.py` | **통과** |
| `bindings/node` `tsc -p tsconfig.tools.json` + perf contract test 4종(18건) | **통과** |
| `bindings/c/perf/single/tests/test_run_comparison_policy.py` (15건) | **통과** |
| `bindings/c/perf/single/tests/test_multi_run_comparison_policy.py` (32건) | **4건 실패 — 이번 변경 이전부터 실패**(변경분을 stash한 상태에서 동일하게 4건 실패함을 확인). 이번 pass 범위 밖 |

---

## 4. 조건 문제가 아니거나 다른 단계로 넘긴 항목

1. **.NET이 `SNDTIMEO`를 설정하지 못한다 (설계 문서 A-6-8)** — 러너 버그가 아니라
   **.NET binding 공개 표면의 공백**이다. `ISocket.Options`(`CommonSocketOptions`,
   `bindings/dotnet/src/Zlink/Contracts/Sockets/SocketOptionFacades.cs`)에 `ReceiveTimeout`은
   있으나 **`SendTimeout` facade가 없다**. `SocketOptions.SndTimeo` 키는 `Zlink.Runtime.*`에만
   있어, 러너가 그것을 쓰면 `PERF_POLICY.md:70-79`(public API만 사용)에 걸린다.
   → binding 수정이 필요하므로 손대지 않았다.
2. **C·C++ relay/pubsub server의 auto-HWM snapshot도 pipe attach 이전이다(= §1의 동일 결함).**
   실측: `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260907_094357.txt`
   (MULTI_DEALER_ROUTER_SENDSEND/tcp/64B)의 server row가 **`4096000/4096000`**.
   근거 지점 — C `multi/common/perf_multi_relay_server.hpp:662`(bind+recalc 직후),
   C `multi/src/perf_multi_pubsub_server.cpp:323`(START 직후),
   C++ `multi/src/perf_dealer_router_server.cpp:93`·`perf_router_router_server.cpp:98`·
   `perf_pubsub_server.cpp:153`.
   **C와 C++가 같은 지점이라 두 러너 사이 표는 일치하므로 이번 pass에서 바꾸지 않았다.**
   고치려면 8개 러너의 relay/pubsub server 채취 지점을 함께 옮겨야 한다(별도 pass).
   D3가 (ii)이므로 **측정값에는 영향이 없고 보고만 부정확하다.**
3. **Go·Rust·Python은 multi에서 AUTO_HWM_DETAIL 행을 내지 않는다**(표가 `unavailable`).
   Go는 `PrintSocketAutoHWMDetail`이 있으나 multi 경로에서 호출되지 않고, Rust·Python은
   emit 자체가 없다. 조건이 아니라 **진단 출력 공백**이므로 별도 항목으로 남긴다.
4. **`select_transports()` 규칙 이식은 C++만 완료.** .NET·Java·Rust는 `--transports`에
   base 교집합 필터가 없고, Python `_transports_for_pattern`도 사용자 지정 시 무필터,
   Node는 패턴별 정적 테이블이다. 현재 범위(tcp 단독)에서는 영향이 없고, 바꾸면 전체 matrix의
   **실행 케이스 집합 자체**가 달라지므로(UNSUPPORTED 보고 → skip) full-matrix 검증과 함께
   진행해야 한다.
5. **Java·Go·Python에는 `PERF_MULTI_SEND_DRAIN_TIMEOUT_MS` 자체가 없다.**
   `PERF_POLICY.md:544-549`는 이 bounded drain을 "수행할 수 있다"로 규정하므로 위반은 아니다.
   다만 세 러너의 종료 경로 정합(설계 문서 §5 단계 3)에서 같이 다뤄야 한다.
6. **Go의 `go_gomaxprocs_case_overrides`** — 특정 case에서 GOMAXPROCS를 8로 올린다.
   이것은 report key 문제가 아니라 **실제 실행 조건 차이**다. 삭제하면 조건을 숨기게 되므로
   유지하고 §5-2로 올린다.
7. Effective Options `clients` 표기: Python은 여러 패턴 동시 선택 시 `100 (stream=100)`
   형태를 쓴다(C는 `100`). 이번 smoke(단일 패턴)에서는 양쪽 `100`이라 차이가 드러나지 않았다.
   full-matrix 실행 전에 맞춰야 한다.
8. 실행 모델·집계 anchor(설계 문서 §3.2~§3.4)는 지시대로 손대지 않았다.

---

## 5. 감독자 판단이 필요한 사항

1. **Core 진단 API 개선 제안(선택).** `zlink_monitor_status`는 live application pipe가 없을 때
   `auto_hwm_applied_*`를 socket option 기본값으로 대체하지만, 호출자가 그 대체를 구분할
   수단이 없다. `detail_flags` bit 하나 또는 pipe count field가 있으면 러너가 "아직 이르다"를
   알 수 있다. Core 수정은 하지 않았고 보고만 한다.
2. **Go의 case별 GOMAXPROCS override 유지 여부.** §4-6. 유지하면 Go의 Effective Options는
   영원히 C와 3행 다르다. 제거하면 과거 Go 튜닝 결정(D-B12x)을 되돌리게 된다.
3. **정책 내부 충돌: monitor HWM 이름.** `PERF_MULTI_TEST_POLICY.md:988`·`:1264`·`:1353`은
   C/Go와 나머지 binding의 CLI/env 이름이 **다른 것을 명시적으로 허용**한다. 설계 문서의
   K15("옵션 이름을 언어별로 바꾸지 않는다")와 정면으로 어긋난다. 나는 **정책 조문을 따라
   CLI/env는 그대로 두고 report key만 `monitor_hwm_bytes`로 통일**했다. 정책 개정 담당
   에이전트와 조정이 필요하다.
4. **§4-2(relay/pubsub snapshot 지점) 를 이번 캠페인에서 고칠지.** 측정값에는 영향이 없으나
   report의 `Auto-HWM Detail`이 SENDSEND·PUBSUB에서 계속 raw 기본값을 보여준다.
5. **`test_multi_run_comparison_policy.py` 기존 실패 4건.** 이번 pass 이전부터 실패한다.
   담당 단계를 지정해 주면 좋겠다.
6. **Python perf 실행에 `ZLINK_LIBRARY_PATH`가 필수**다(`ZLINK_CORE_SOURCE=local`만으로는
   기동하지 않는다). 이번 smoke는
   `ZLINK_LIBRARY_PATH=/home/hep7/project/zlink/core/build/lib/libzlink.so.0.17.0`로 실행했다.
   환경 manifest에 이 요구를 추가할지 판단이 필요하다.
