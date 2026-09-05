# Core monitor connection identity 조사·수정 요약

## 결론

- 변경 분류: **B — 기존 Core 결함**.
- 소유 계층: physical transport attempt identity와 reconnect는 Core endpoint/transport lifecycle이 소유한다.
- 공개 ABI 변경 없음. 테스트는 공개 C API만 사용한다.
- `core/build`, `core/build-dev` symlink는 사용·수정하지 않았고 `core/build-monid`만 사용했다.
- 금지된 spec/framework/bindings/doc 경로는 수정하지 않았다.

## 공개 C API 관찰 결과표

모든 케이스는 fixed routing ID의 DEALER→ROUTER이며, 모든 physical event의 `transport_lane`은 `Application(0)`이었다.

| transport | 관찰 시나리오 | connection_id 관계 | flags | 결과 |
|---|---|---|---|---|
| TCP | 최초 CONNECT_DELAYED→READY | 같은 물리 시도이므로 동일 | DELAYED=0, READY=EDGE | PASS |
| TCP | 서버 close→DISCONNECTED | 직전 READY와 동일 | 0 | PASS |
| TCP | 서버 rebind→자동 재연결 READY | 이전 READY와 다른 새 ID | EDGE | PASS |
| TCP | 접속 실패 CONNECT_DELAYED→CLOSED | 동일 실패 시도이므로 동일 | 둘 다 0 | PASS |
| TCP | client `zlink_disconnect`→DISCONNECTED | 직전 READY와 동일 | 0 | PASS |
| TCP | client close, peer monitor DISCONNECTED | peer READY와 동일 | 0 | PASS |
| IPC | 최초 CONNECT_DELAYED→READY | 같은 물리 시도이므로 동일 | DELAYED=0, READY=EDGE | PASS |
| IPC | 서버 close→DISCONNECTED | 직전 READY와 동일 | 0 | PASS |
| IPC | 서버 rebind→자동 재연결 READY | 이전 READY와 다른 새 ID | EDGE | PASS |
| IPC | 접속 실패 CONNECT_DELAYED→CLOSED | 동일 실패 시도이므로 동일 | 둘 다 0 | PASS |
| IPC | client `zlink_disconnect`→DISCONNECTED | 직전 READY와 동일 | 0 | PASS |
| IPC | client close, peer monitor DISCONNECTED | peer READY와 동일 | 0 | PASS |
| inproc | 최초 READY→서버 close DISCONNECTED | 동일 | READY=EDGE, DISCONNECTED=0 | PASS(수정 전 불일치) |
| inproc | 서버 rebind→자동 재연결 READY | 이전 READY와 다른 새 ID | EDGE | PASS(수정 전 5초 내 미발생) |
| inproc | client `zlink_disconnect`→DISCONNECTED | 직전 READY와 동일 | 0 | PASS(아래 command-progress 제약 있음) |
| inproc | client close, peer monitor DISCONNECTED | peer READY와 동일 | 0 | PASS(아래 command-progress 제약 있음) |
| inproc | peer close→CLOSED | event 자체가 없음 | N/A | **SPEC GAP — 미수정** |

주의: TCP/IPC에서 서버 close 뒤 관찰되는 `CLOSED`는 이전 READY transport의 close가 아니라 이후 자동 재연결의 실패한 새 connect attempt이다. 따라서 올바른 correlation은 `이전 READY == DISCONNECTED`, `새 CONNECT_DELAYED == CLOSED`이고 새 attempt의 ID는 이전 READY와 달라야 한다. Java disabled 테스트의 `이전 READY == 다음 CLOSED` assertion은 attempt 경계를 합친다.

## 적용한 spec 조항

- `core/doc/spec/core/06-monitoring.ko.md:69-72`: `connection_id`는 하나의 물리적 transport 시도를 식별하고 DEALER→ROUTER physical event lane은 Application이다.
- `core/doc/spec/core/06-monitoring.ko.md:92-95`: 새 `CONNECTION_READY` edge는 EDGE flag로 구분한다.
- `core/doc/spec/core/06-monitoring.ko.md:518`: 같은 monitor event 순서는 Core가 state transition을 commit한 순서다.
- `core/doc/spec/core/06-monitoring.ko.md:523-536`: DISCONNECTED value, READY edge, lane, correlation 값 계약.
- `core/doc/spec/core/socket/README.ko.md:795-802,825-827`: 지원 transport에 inproc을 포함하며 peer가 unavailable이면 library가 자동 재연결한다.
- `core/doc/spec/core/socket/README.ko.md:599-619,851-863`: socket close lifecycle 및 명시적 endpoint disconnect 계약.
- CLOSED enum/value는 있으나 physical fd close인지 모든 transport lifecycle close인지, 특히 inproc에서 언제 발생해야 하는지 trigger 계약이 없다.

## 원인과 수정

1. **inproc identity 분열**
   - 원인: `core/src/runtime/core/endpoint.cpp:25-33`의 endpoint pair 생성자가 매번 새 ID를 발급하는데, 기존 `core/src/runtime/sockets/common/socket_base_endpoint.cpp` inproc 연결/완료 lane 생성은 connect/bind 양쪽 pair를 별도로 만들었다.
   - 수정: `core/src/runtime/sockets/common/socket_base_endpoint.cpp:340-347,538-544`에서 한 physical inproc connection의 두 pipe half가 같은 ID를 공유한다. `core/src/runtime/sockets/common/socket_base_api.cpp:1726-1729`는 pipe termination record를 pipe의 transport connection ID로 정규화한다.

2. **TCP/IPC 한 connect attempt 안에서 identity 재발급**
   - 원인: connecter가 CONNECT_DELAYED, CONNECT_RETRIED, engine 생성, CLOSED마다 새 endpoint pair를 만들었고 `endpoint.cpp:25-33`이 각각 새 ID를 발급했다. `socket_base_monitor.cpp:435-440`은 전달받은 새 ID를 그대로 CLOSED record에 기록했다.
   - 수정: `core/src/runtime/transports/tcp/asio_tcp_connecter.cpp:151-155,219-222,292-310,339-342`와 IPC 대응부 `:151-155,185-188,237-255,281-284`가 `_attempt_endpoint_pair`를 attempt 시작 때 한 번 만들고 해당 attempt의 모든 event/engine에 재사용한다.

3. **inproc peer detach 후 자동 재연결 intent 소실**
   - 원인: 연결 전 bind 부재는 pending inproc connection으로 보존되지만, 이미 연결된 peer가 닫힌 뒤 connector pipe는 새 pending connect로 재등록되지 않았다.
   - 수정: `core/src/runtime/sockets/common/socket_base_api.cpp:1740-1751,1915-1920`이 connector-side `_inprocs` record가 남은 unexpected detach만 재연결 대상으로 판별한다. `socket_endpoint_runtime.cpp:127-143`의 lookup과 `core/src/runtime/core/command.hpp:49,209-214`, `object.cpp:165-171,485-496`, `socket_base_lifecycle.cpp:1372-1380`의 owner self-command로 command dispatch 재진입 없이 connect intent를 다시 등록한다. 명시적 `zlink_disconnect`는 먼저 `_inprocs` record를 지우므로 자동 재연결하지 않는다.

## 회귀 테스트

- 신규: `core/tests/integration/monitoring/test_monitor_connection_identity.cpp`
  - `test_tcp_connection_identity_lifecycle`
  - `test_ipc_connection_identity_lifecycle`
  - `test_inproc_connection_identity_lifecycle`
  - `test_tcp_failed_attempt_closed_identity`
  - `test_ipc_failed_attempt_closed_identity`
  - `test_tcp_explicit_disconnect_identity`
  - `test_ipc_explicit_disconnect_identity`
  - `test_inproc_explicit_disconnect_identity`
  - `test_tcp_client_close_identity`
  - `test_ipc_client_close_identity`
  - `test_inproc_client_close_identity`
- 등록: `core/tests/CMakeLists.txt:82,363-367`; CTest `TIMEOUT 30`, labels `integration;serial` 확인.

## 교차언어 대조

- `bindings/java/.../MonitorConnectionIdentityContractTest.java`의 disabled inproc READY/DISCONNECTED 및 재연결 관찰은 최초 공개 C API 재현과 일치했고 Core 수정으로 해소됐다.
- Java의 TCP CLOSED disabled case는 `READY(old attempt) == CLOSED(next failed attempt)`를 요구해 물리 시도 경계를 합친다. Core 공개 C API 회귀 테스트는 spec 정의대로 old READY↔DISCONNECTED, new CONNECT_DELAYED↔CLOSED를 각각 correlation한다.
- 언어 구조 차이가 아니라 Core identity 생성 결함이므로 binding/framework는 변경하지 않았다.

## 게이트 수치

- configure: `RelWithDebInfo`, `ENABLE_LTO=OFF`, `ZLINK_BUILD_TESTS=ON`, `BUILD_TESTS=ON`; `core/build-monid`.
- 최종 전체 build: `cmake --build core/build-monid -j3` 성공.
- 신규 11-case test: `--repeat until-fail:5` **5/5 green**, 총 3.37초(회당 0.67~0.68초).
- monitoring CTest label은 등록돼 있지 않음(`-L monitoring`: 0개). 대체 monitor 이름 suite `-R monitor -j2`: **13/13 green**, 9.93초.
- 최종 `-L integration -j2`: **94/94 green**, 163.33초.
- 전체 `ctest -j2`: **144/145 green**, 196.64초. 유일 실패는 감독자 별도 대상 `hotpath_gate`.
- `hotpath_gate` 제외 전체: **144/144 green**, 195.46초.
- `hotpath_gate` 측정/기준 비율: dealer_dealer_inproc 1.2954, dealer_router_reqrep_inproc 1.2553, pair_inproc 1.3167, router_router_tcp 1.2971.
- 마지막 client-close 3 case 추가 뒤 신규 반복, monitor suite, integration 전체를 다시 실행해 모두 green.
- 최종 `git diff --check`: PASS.

## BLOCKERS / 후속 판단

1. **inproc CLOSED spec gap:** 현재 Core inproc 경로에는 `event_closed` 호출이 없고 monitoring/socket spec은 CLOSED의 transport-independent 발생 조건을 정의하지 않는다. CLOSED를 OS fd/listener close로 한정할지, inproc peer detach에도 요구할지 사용자 결정이 필요하다. 본 변경에서는 event를 추가하지 않았다.
2. **inproc peer command progress:** explicit disconnect/client close 뒤 peer socket에서 공개 API를 전혀 호출하지 않으면 peer-side DISCONNECTED가 5초 안에 진행되지 않았다. 회귀 테스트는 public `zlink_poll`로 peer command queue를 진행시킨 뒤 identity만 검증한다. command progress/lost-wake 소유권의 별도 조사 대상이며 여기서 우회 수정하지 않았다.
3. **WS/TLS 선택 범위 미검증:** optional 범위라 회귀 matrix에 넣지 않았다. 두 connecter는 아직 event마다 `make_unconnected_connect_endpoint_pair`를 호출하므로 같은-attempt identity parity를 후속 확인해야 한다.
4. **hotpath_gate:** 위 4개 workload가 기준보다 25.53~31.67% 높아 실패했다. 사용자 지시대로 감독자 별도 판정 대상으로 남겼다.

A가 쓸 한 줄: Core B 결함 수정 완료 — TCP/IPC attempt identity와 inproc 양쪽 identity·자동 재연결을 Core 소유 계층에서 바로잡았고 공개 C API 신규 11-case 5/5, monitor 13/13, integration 94/94, hotpath 제외 전체 144/144가 green이며 inproc CLOSED는 spec gap으로 남겼다.
