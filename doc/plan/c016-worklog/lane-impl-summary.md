# DEALER-ROUTER single-lane Core 구현 요약

## 결과

승인 설계의 A안을 Core runtime에 반영했다. DEALER-DEALER와 DEALER-ROUTER는 Application connection 하나를 사용하고, ROUTER-ROUTER만 Application·Completion connection을 사용한다. ZMP `VERSION`은 `0x01`을 유지하며, DEALER·ROUTER READY의 `Zlink-Lane-Count` 누락·잘못된 길이·값·계산 불일치는 data plane이 열리기 전에 거절한다.

최종 Core build와 전체 CTest는 통과했다. OOM 재현 대상은 8GB 제한에서 23/23, 전체 test는 16GB 제한에서 134/134다. 신규 contract test 29개는 최종 tree에서 세 번 연속 통과했다. Core gate를 막는 build 또는 runtime 실패는 없다. 다만 승인 설계의 malformed READY monitor event를 표현할 public enum 값이 정식 spec에 없다는 계약 공백은 아래 `BLOCKERS`에 분리했다.

- branch: `main`
- git commit·push·branch 변경: 수행하지 않음
- `doc/**`, `framework/**`, language binding source: 이 작업에서 수정하지 않음
- 허용된 raw C header mirror: C/C++/Go/Rust 각 3개, 합계 12개를 Core header와 byte-identical하게 동기화
- Core 구현 범위: tracked non-doc Core 파일 72개와 신규 `core/tests/integration/test_dealer_router_single_lane_contract.cpp` 1개
- 현재 `git diff --stat -- core`에는 동시 작업의 `core/doc` 변경 32개가 포함되고 신규 untracked test는 포함되지 않으므로 lane 구현량으로 사용하지 않음
- 추가된 line에서 `fprintf`·`printf`·`std::cerr`·debug 환경 변수 사용을 검색했으며 임시 진단 출력은 남아 있지 않음. 검색에 걸린 항목은 test routing ID를 만드는 `snprintf`뿐임

## 설계 단계별 변경

### 1. 대칭 lane-count owner

`core/src/runtime/protocol/zmp_control.hpp:208-227`에 `(local socket type, peer socket type)`을 대칭으로 판정하는 owner를 두었다. DEALER-DEALER·DEALER-ROUTER는 count `1`, ROUTER-ROUTER는 count `2`이며 ready mask는 각각 `1`, `3`이다. `core/src/runtime/core/options.hpp`의 shared pair state와 `core/src/runtime/sockets/common/socket_base.hpp:121-150`의 socket pair record가 이 count를 immutable topology로 보존한다.

### 2. READY metadata와 HELLO-first handshake

`core/src/runtime/protocol/zmp_metadata.hpp:90-122`가 D/R READY에 1-byte `Zlink-Lane-Count`를 쓰고, 누락과 길이·값 오류를 구분해 검증한다. `core/src/runtime/engine/asio/asio_zmp_engine.cpp:237-278`은 paired transport에서 HELLO만 먼저 보내며, `:423-499`는 peer HELLO type을 socket owner에 전달한 뒤 count를 확정하고 READY를 보낸다. `:574-650`은 READY의 Lane·Lane-Count·Socket-Type·Routing-Id를 함께 검증하고 count `1`의 Completion lane과 mixed model을 `EPROTO`로 닫는다. ZMP wire version은 `core/src/runtime/protocol/zmp_protocol.hpp:12`의 `0x01`을 유지했다.

### 3. Application-first endpoint 생성

Network connect는 `core/src/runtime/sockets/common/socket_base_endpoint.cpp:454-469`에서 Application intent만 먼저 만든다. Owner가 peer type을 확인한 뒤 `:748-795`에서 count를 정한다. `:821-877`은 count `2`인 Application owner의 Completion options와 address만 준비하고, exact owner·generation lease를 확정한 뒤 `:879-907`에서 Completion child session을 만든다. TCP·IPC·TLS·WS connecter는 같은 intent와 generation을 전달한다.

Inproc은 `socket_base_endpoint.cpp:249-445`에서 peer type을 알 수 있으면 count에 맞춰 pipe를 만들고, connect-before-bind면 Application 하나만 보류한다. `core/src/runtime/core/ctx_inproc_registry.cpp:239-260,337-365`가 bind resolution 뒤 count를 확정하고 Application을 먼저 attach한 다음 count `2` materialization을 요청한다. 실제 no-RID, HWM `0` Completion pipe는 `socket_base_endpoint.cpp:487-515`가 만든다.

### 4. Pair readiness·reconnect·detach

`core/src/runtime/sockets/common/socket_base_api.cpp:288-402`는 staged count-unknown Application을 공개하지 않고, 검증된 count의 expected mask가 충족된 pair만 ready로 바꾼다. Count `1`은 Application 자체를 completion source로 사용하고, count `2`는 두 lane의 peer identity까지 일치해야 한다. `:451-568`은 scheduler attach, pre-ready FLOWSTATE 적용, write-hold 해제와 pending submit wake를 pair admission 순서로 실행한다.

Reconnect는 `core/src/runtime/core/session_base.cpp:771-806`에서 shared pair generation을 한 번 올리고 Application session을 다시 시작한다. Peer HELLO가 count `2`를 확정한 경우에만 `socket_base_endpoint.cpp:821-907`이 같은 generation의 Completion sibling을 만든다. Count `1`에는 Application 하나만 남는다.

Detach는 `core/src/runtime/sockets/common/socket_base_api.cpp:1605-1653`에서 exact pair ID·generation의 ready 상태, sibling, pending flow와 pause 회계를 정리하며, 이어지는 lifecycle 경로가 monitor readiness와 request correlation을 끝낸다. Request route 선택은 `core/src/runtime/sockets/dealer/dealer.cpp:41-62,182-216,243-275`와 `core/src/runtime/sockets/internal/lb.cpp:218-289`에서 inactive·connection ID `0`·retired pair/generation을 제외한다.

### 5. Single FIFO 직렬화·reply route·control route

`core/src/runtime/core/pipe.hpp:55-67,270-276`와 `core/src/runtime/core/pipe.cpp:45-75,979-1000`에 first frame을 소비하지 않는 normalized head-kind probe를 추가했다. `core/src/runtime/sockets/common/socket_base_api.cpp:1048-1105`는 count `1` Application FIFO의 DATA·REQUEST를 public receive에 남기고 REPLY·error reply·control만 completion owner로 넘긴다. DEALER와 ROUTER receive는 multipart FINAL을 모두 꺼낸 뒤에만 다음 head를 다시 분류한다(`dealer.cpp:424-433`, `router_recv_path.cpp:242-315,407-456`). 따라서 DATA multipart 뒤의 REPLY가 FINAL을 앞지르지 않는다.

Requester는 submit 때 pair ID·generation을 고정한다(`core/src/api/socket/socket_request_reply_submit_api.cpp:141-157`). Reply matcher는 source pipe·connection ID까지 같은 현재 transport만 허용한다(`socket_request_reply_pending_api.cpp:98-155`). Responder는 저장된 source peer type에 따라 현재 ready DEALER Application 또는 ROUTER Completion route를 고른다(`socket_request_reply_runtime_io.cpp:1699-1742`). Multipart reply는 `:1788-1828`에서 connection ID snapshot부터 terminal flush까지 transport generation lock으로 묶었다. D/R reply는 Application HWM·PAUSED를 따르므로 `zlink_reply_part(FINAL)`의 `BACKPRESSURED/EAGAIN` 계약도 유지한다.

FLOWSTATE와 WEIGHT는 같은 physical FIFO에서 별도 latest-value slot과 공유 enqueue sequence를 사용한다. `core/src/runtime/core/pipe.cpp:2101-2117,2333-2390,3753-3814`는 pair ID·generation과 D/R peer type이 확인된 pipe에만 slot·flush·rollback을 허용한다. Count `1`에서는 Application, count `2`에서는 FLOWSTATE가 Completion을 사용하며 WEIGHT는 Application만 사용한다. PAIR·PUB·SUB·XPUB·XSUB·STREAM에는 이 분기가 적용되지 않는다.

### 6. Monitor와 physical queue 회계

`core/src/runtime/sockets/common/socket_monitor_runtime.cpp:9-64,153-179`는 endpoint·pair ID·generation으로 count `2` lane readiness를 모으고 stale generation만 정확히 제거한다. `core/src/runtime/sockets/common/socket_base_monitor.cpp:560-635`는 logical peer당 `CONNECTION_READY`를 한 번만 내고 Application connection의 endpoint·RID·connection ID를 event identity로 사용한다. Inproc은 `:638-683`에서 count `1` Application을 바로 ready로, count `2`만 two-lane accumulator로 처리한다.

Monitor owner 전환 TOCTOU는 `core/src/runtime/sockets/common/socket_base_lifecycle.cpp:470-625`에서 고쳤다. Monitor acquire, owner lease 취소, 마지막 mailbox empty 확인과 physical detach가 `_transport_pair_owner_progress_sync` 하나로 직렬화된다. `core/tests/integration/test_ctx_destroy.cpp:1185-1290`의 deterministic interleaving test가 monitor acquire가 retiring executor를 빌리지 않고 handoff 뒤에도 event progress를 유지하는지 검증한다.

Count `1` D/R pipe 전체는 Application physical queue로 등록한다. `core/src/runtime/core/ctx_inproc_registry.cpp:309-330`과 `core/src/runtime/sockets/common/socket_base_endpoint.cpp:293-352`는 Application 방향만 auto-HWM reservation에 넣고 count `2` Completion은 HWM `0`과 별도 queue class를 유지한다. `core/tests/unittest/unittest_auto_hwm_policy.cpp:375-425`는 single-lane REPLY byte의 Application current/provisional 회계와 Completion current/peak/pending `0`을 검증한다. 실제 D/R reply의 Application current/provisional/peak 증가와 Completion 통계 불변은 `core/tests/integration/test_dealer_router_single_lane_contract.cpp:2846-2976`이 검증한다.

### 7. Public header 주석과 raw mirrors

Signature·enum 값·ABI layout은 변경하지 않았다. Socket type별 receive-flow route 설명을 다음 Core public header에 반영했다.

- `core/include/zlink/socket/api.h:179-199`
- `core/include/zlink/eventing/api.h:159-177`
- `core/include/zlink_enum.h:182-187,243-250`

같은 3개 raw header를 `bindings/{c,cpp,go,rust}/include`에 복제했으며 12/12 `cmp`를 통과했다. Binding projection과 Framework source는 사용자 제한에 따라 수정하지 않았다.

### 8. Contract·회귀·perf 검증

`core/tests/integration/test_dealer_router_single_lane_contract.cpp:1264-3577`에 wire connection 수와 READY property, mixed-model fail-fast, FIFO와 request-reply, reply backpressure, receive-flow, monitor lane·ready·detach, reconnect와 status ABI를 검증하는 29개 case를 추가했다. CTest 등록은 `core/tests/CMakeLists.txt:743-772,851-865`에 있다.

기존 test는 count별 topology, old generation fence와 async teardown을 현재 계약으로 확장했다. 주요 파일은 다음과 같다.

- handshake·wire: `test_zmp_metadata.cpp`, `unittest_zmp_contract_edges.cpp`, `unittest_zmp_decoder.cpp`
- request·reply·reconnect: `test_phase3_request_reply_contract.cpp`, `test_router_handover.cpp`, `test_router_multiple_dealers.cpp`, `test_transport_matrix.cpp`
- flow·monitor: `test_flow_state_c_api.cpp`, `test_flow_state_paired.cpp`, `monitoring/test_monitor_enhanced.cpp`, `monitoring/test_monitor_socket_contract.cpp`
- lifecycle·회계: `test_ctx_destroy.cpp`, `test_ctx_options.cpp`, `unittest_socket_runtime.cpp`, `unittest_auto_hwm_policy.cpp`

## 파일 그룹

| 그룹 | 변경 파일 |
|---|---|
| Protocol·handshake | `runtime/protocol/zmp_control.hpp`, `zmp_metadata.hpp`; `runtime/engine/{i_engine.hpp,asio/asio_engine.cpp,asio/asio_zmp_engine.*}`; `runtime/core/{options.*,session_base.*}` |
| Endpoint·pair lifecycle | `runtime/core/{command.hpp,object.*,ctx_inproc_registry.*}`; `runtime/sockets/common/socket_base{,_api,_endpoint,_lifecycle,_dispatch,_msg}.*`; TCP·IPC·TLS·WS connecter |
| FIFO·request-reply | `runtime/core/pipe.*`; `api/socket/part_helper*`, `socket_message*`, `socket_request_reply*`; `runtime/sockets/{dealer,router,internal}` |
| Flow·monitor·polling·회계 | `socket_base_flow_state.cpp`, `socket_send_complete.cpp`, `socket_base_monitor.cpp`, `socket_monitor_runtime.cpp`, `socket_lifecycle_runtime.cpp`, `socket_runtime.hpp`와 관련 Core tests |
| Public surface | Core public header 3개와 허용된 C/C++/Go/Rust raw mirror 12개 |
| Contract tests | `core/tests/CMakeLists.txt`, 신규 single-lane executable와 기존 integration·unit test 보강 |

## OOM 근본 원인

OOM은 `test_zmp_metadata.cpp`의 loop나 READY property parser가 만든 큰 allocation이 아니었다. 새 peer-control drain의 종료 조건이 잘못되어 production pipe가 control frame을 무한히 만들었다.

OOM 당시 `pending_peer_controls_unlocked()`의 pending 판정은 `_pending_peer_control_sequence`를 slot 존재 표식처럼 취급했다. 이 값은 WEIGHT·FLOWSTATE update 순서를 배정하는 monotonic allocator다. 마지막 FLOWSTATE slot을 비운 뒤에도 allocator 값이 남아 drain loop가 계속 true가 되었고, 이미 비운 slot을 다시 frame으로 만들면서 mailbox/yqueue allocation이 누적되어 이전 job에서 anon RSS 약 55GB까지 증가했다.

현재 수정은 `core/src/runtime/core/pipe.cpp:2090-2098`처럼 실제 slot인 `_pending_peer_weight != unset || _pending_flow_state_valid`만 pending으로 판정한다. Network와 inproc drain은 `:2194-2247,2286-2317`에서 slot을 비우고, 두 slot이 모두 비면 aggregate sequence를 `0`으로 되돌려 loop를 끝낸다. READY parser는 `core/src/runtime/protocol/zmp_metadata.hpp:103-122`에서 property 길이를 `1`로 제한하고 값 `1/2`만 받으므로 이 경로에는 unbounded length allocation이나 loop가 없다. `core/tests/integration/test_zmp_metadata.cpp:1822-1858`은 누락, 길이 `0/2`, 값 `0/3`, count mismatch와 count `1` lane `1`을 각각 검증한다.

수정 후 최종 relink된 `core/build/bin/test_zmp_metadata`를 `ulimit -v 8388608` 아래에서 실행했다. 결과는 23/23 PASS, peak RSS 7,028KB다.

## Gate 결과

| Gate | 결과 |
|---|---|
| `ulimit -v 16777216; cmake --build core/build -j2` | PASS, 100%, tests 포함 |
| `ulimit -v 16777216; ctest --test-dir core/build --output-on-failure -j2` | PASS, 134/134, 165.15초. 기준선 105 + 신규 29 |
| `test_dealer_router_single_lane_contract` 반복 | 29/29 PASS × 3, 총 87 case |
| 기존 reconnect·flow-state·request-reply CTest matrix | 17/17 PASS, 44.50초 |
| `test_flow_state_paired` | 25/25 PASS × 3; deterministic pre-ready promotion 20/20 |
| `test_router_handover` | executable 4/4, isolated handover 20/20 PASS. A→Z current를 먼저 확정한 뒤 reciprocal standby를 붙이는 test sequencing으로 비동기 가정을 제거했으며 production route 계약은 변경하지 않음 |
| OOM 대상 `test_zmp_metadata` | 8GB 제한, 23/23 PASS, peak RSS 7,028KB |
| Raw header mirror | Core 변경 header 3개 × C/C++/Go/Rust = 12/12 byte-identical |
| `git diff --check` | PASS |
| `bash bindings/cpp/tests/run_tests.sh` | contract 15/15, samples 7/7 PASS |
| `ZLINK_CORE_SOURCE=local bash bindings/python/tests/run_tests.sh` | 144 tests PASS, 4 subtests PASS, samples 7/7 PASS |
| C single perf: `run_benchmarks.sh --pattern DEALER_ROUTER_REQREP --transports tcp --msg-sizes 1024 --runs 1` | complete, 5/5 result lines. 351.02 Kops/s, 718.88 MB/s, mean/p95/p99 0.165/0.293/0.391ms |
| C multi perf: `run_benchmarks_multi.sh --pattern STREAM --transports tcp --runs 1` | complete, 4 sizes, 20/20 result lines, unsupported/skip/fail 0 |

Perf 결과 파일:

- `/home/hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260902_212926.txt`
- `/home/hep7/project/zlink/bindings/c/perf/results/multi/report/perf_c_multi_linux_20260902_212956.txt`

Exit `86` 또는 `134`는 발생하지 않아 재실행 규칙을 적용할 필요가 없었다. 제공된 reference baseline이 없으므로 perf 수치는 sanity 완료 결과로만 기록하며 변화율 판정은 하지 않는다.

## BLOCKERS

### B1. Malformed READY의 public protocol-error event 값

승인 설계 `doc/plan/dealer-router-single-lane-design.ko.md:829-831`은 잘못된 `Zlink-Lane-Count`에 `HANDSHAKE_FAILED_PROTOCOL`과 disconnect를 함께 관찰하도록 요구한다. 정식 ZMP spec은 이를 READY protocol error로 규정한다(`core/doc/spec/core/protocol/01-zmp.ko.md:190-195`). 그러나 정식 monitoring spec과 public enum은 HELLO 전용 값 하나만 정의한다.

- `core/doc/spec/core/06-monitoring.ko.md:315-316`
- `core/include/zlink_enum.h:213-216`

READY 오류를 `ZLINK_PROTOCOL_ERROR_ZMP_MALFORMED_COMMAND_HELLO`로 보고하면 enum 의미를 위반한다. 새 public enum 값이나 generic mapping은 spec/API 변경이므로 이번 범위에서 만들지 않았다. 현재 contract test는 invalid READY가 즉시 disconnect되고 `CONNECTION_READY`와 payload가 나오지 않는 것까지 검증한다(`core/tests/integration/test_dealer_router_single_lane_contract.cpp:1442-1539`). 정확한 `HANDSHAKE_FAILED_PROTOCOL` value가 정식 계약에 추가되면 event assertion을 보강해야 한다.

### B2. 수정 금지된 C++ projection 주석

Core public C header와 raw mirrors는 socket type 기준으로 갱신했지만, 다음 higher-level C++ binding header에는 Completion-only 설명이 남아 있다.

- `bindings/cpp/include/zlink/Contracts/Eventing/status.hpp:134-136`
- `bindings/cpp/include/zlink/Contracts/Eventing/events.hpp:37-39`
- `bindings/cpp/include/zlink/Contracts/Sockets/socket_options.hpp:38-42`
- `bindings/cpp/include/zlink/Contracts/Sockets/socket_contracts.hpp:73-79`

이 파일은 허용된 raw header mirror가 아닌 binding source이므로 사용자 제한에 따라 수정하지 않았다.

### B3. 동시 작업의 out-of-scope dirty tree

Lane 작업 시작 뒤 `core/doc`과 `bindings/doc`에 다수 변경이 유입되었고, 최종 audit 중에도 dirty file 수가 계속 변했다. 외부 작업은 계속 진행 중일 수 있으므로 고정된 최종 개수를 기록하지 않고 lane 구현 범위에서도 제외한다. 또한 다음 binding source·generated 문서 변경은 기존 사용자 작업으로 보존했다.

- `bindings/cpp/CMakeLists.txt`
- `bindings/dotnet/src/Zlink/Runtime/Native/NativeLibraryLoader.cs`
- `bindings/dotnet/src/Zlink/Zlink.csproj`
- `bindings/go/README.godoc.md`
- `bindings/java/build.gradle`

`framework/**`에는 현재 dirty entry가 없다. 위 외부 변경은 lane 구현과 gate 결과에 포함하지 않았고 정리·restore·reset하지 않았다.
