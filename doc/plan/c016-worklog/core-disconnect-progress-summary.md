# Core disconnect·reconnect progress 판정

감독자가 Framework의 두 번째 POLLIN poller 제거 여부를 판단하기 위한 공개 C API 검증 결과다.

**기존 Core 결함을 재현하고 수정했다. 수정 Core에서는 application DEALER를 poll하지 않아도 terminal monitor edge와 자동 reconnect READY가 진행된다. 같은 endpoint에 즉시 reconnect한 뒤 제출한 REQUEST도 새 연결에서 응답을 받는다.** 이 수정이 반영된 Core를 사용하면 Framework의 두 번째 poller를 유지할 진행성 근거가 없다. Framework·binding 구현은 이 job에서 수정하지 않았다.

- 작업 tree: `/home/hep7/project/zlink-core-b`, detached HEAD `25bb4764afa4e677dac7029779eff13a74096274`.
- Core·test diff는 위 tree에 uncommitted 상태로 남긴다. main tree에는 이 요약만 작성한다.
- `doc/spec/**`, `core/doc/spec/**`, `bindings/**`, `framework/**` 원본 파일 변경 없음. main의 `core/build-dev` 사용 없음.
- 관측 범위: Linux, DEALER–ROUTER, tcp loopback·inproc. Application poller 1개, `POLLIN` 또는 `POLLIN|POLLCOMPLETION` 등록. ROUTER의 RID 중복 정책은 공개 HANDOVER 옵션이다.

## 소유 계약과 변경 분류

- **소유 계층:** Core socket command owner·pipe lifecycle·inproc endpoint registry. Framework가 command drain이나 reconnect를 대신 소유하지 않는다.
- **Spec 조항:** `core/doc/spec/core/05-polling.ko.md` §3·§4; `core/doc/spec/core/socket/README.ko.md` §6 `zlink_connect`·`zlink_disconnect`·`zlink_disconnect_rid`; `core/doc/spec/core/06-monitoring.ko.md` §2·§3.1·§4.
- **교차언어 대조:** Framework runtime 변경 없음. 동일 public C API를 사용하는 C++ contract/sample 및 Python test/sample로 수정 Core를 추가 검증했다. 특정 언어에서만 필요한 보상 로직을 추가하지 않았다.
- **분류:** **B — 기존 Core 결함 수정.** 새 public API나 spec 변경 없음.

## 재현 원인과 수정

### Completion poller가 monitor command owner를 중지

기존 `socket_base_dispatch.cpp:222`의 `acquire_completion_poller()`는 async executor가 있으면 monitor의 lease와 관계없이 중지했다. Monitor를 먼저 열고 completion bit를 등록한 뒤 application이 idle이면 disconnect의 pipe termination command가 mailbox에 남았다. TCP `tcp_completion_disconnect_idle`에서 `zlink_disconnect()`는 **0.015 ms**에 반환했지만 **2,000 ms**까지 old connection의 terminal edge가 없었다. 같은 조건의 `POLLIN` 등록에서는 약 10 ms에 발생했다.

수정 위치는 worktree의 `core/src/runtime/sockets/common/socket_base_dispatch.cpp:222`, `socket_base_lifecycle.cpp:919`다. Completion owner 등록은 기존 monitor의 command lease를 보존한다. Async executor는 기존 `_completion_poller_refs` 검사로 completion drain을 public poller에 맡기며, command 처리는 계속 수행한다. Monitor lease가 없는 기존 소유권 이전 경로는 유지된다.

대안으로 모든 completion 등록에서 async executor 중지를 없애는 방법을 검토했다. Monitor lease에 대한 중지만 막는 방법이 기존 monitor 없는 소유권 이전 계약을 보존하고 변경 범위가 작다. `test_ctx_destroy`의 기존 explicit-quiesce 검증도 통과했다.

### Inproc monitor connection identity 불일치

기존 `pipe.cpp:3397`의 `set_endpoint_pair()`는 inproc pipepair의 두 endpoint description을 설치할 때마다 shared transport connection ID를 덮어썼다. 초기 READY는 ID **9**, 같은 연결의 DISCONNECTED는 ID **8**로 관측됐다. Terminal event 자체는 왔지만 기존 connection과 correlation할 수 없었다.

`core/src/runtime/core/pipe.cpp:3397`에서 inproc은 pipepair 생성 시 할당한 shared connection ID를 사용한다. Session transport의 engine-assigned identity 경로는 기존 동작을 유지한다.

### Inproc remote detach 뒤 connect intent 소실

기존 `socket_base_api.cpp:1853`은 inproc pipe 종료 시 registry entry를 지우고 재연결하지 않았다. ROUTER의 public `zlink_disconnect_rid()` 뒤 DEALER에서 자동 READY가 2,000 ms 안에 오지 않았다.

`core/src/runtime/sockets/common/socket_base_api.cpp:1853`에서 registry에 남아 있는 connect intent를 확인하고, remote pipe 종료 뒤 같은 endpoint 연결을 다시 생성한다. Explicit endpoint disconnect는 registry entry를 먼저 지우므로 자동 재연결하지 않는다. Socket/context 종료와 `RECONNECT_IVL == -1`도 재연결에서 제외한다.

별도 retry timer·연결 상태 map을 추가하는 대신 기존 inproc 연결 생성 코드를 `socket_base_endpoint.cpp:292`의 private `connect_inproc_endpoint()`로 분리해 재사용한다. Peer가 없으면 기존 context pending-inproc 경로가 후속 bind를 기다린다. 함수 본문은 기존 block과 들여쓰기 및 local `rc` 선언 외 동일함을 대조했다. 새로운 public 진입점은 없다.

## 측정 방법

- `automatic`: 연결과 초기 DATA admission을 확인한 뒤 ROUTER가 `zlink_disconnect_rid()`로 peer pipe를 종료한다. DEALER에서 Disconnected와 새 connection의 READY를 기다린다. DEALER의 disconnect/connect/submit 추가 호출은 없다.
- `disconnect`: DEALER의 `zlink_disconnect(endpoint)` 반환 뒤 해당 DEALER를 호출하지 않고 old pipe의 terminal monitor event를 기다린다.
- `immediate-request`: DEALER에서 `disconnect` → `connect` → blocking public `zlink_request_part(..., NONE)`를 연속 호출한다. Terminal edge를 기다리거나 connect를 재시도하지 않는다. ROUTER 쪽 old pipe의 DISCONNECTED와 new READY까지 관측한 뒤 REQUEST를 receive하고 reply한다. REQUEST completion ID, 성공 결과, reply payload를 검증한다.
- `idle`: DEALER는 등록만 하며 application socket poll을 한 번도 호출하지 않는다.
- `blocked`: 별도 thread가 같은 application poller에서 **한 번** `zlink_poller_wait()`에 진입한다. Public `zlink_poller_size()`의 BUSY로 진입을 확인하며, monitor 검증 완료 후 등록된 timer로 wait를 종료한다. 반복 poll이나 두 번째 DEALER poller는 없다.
- Monitor 관측은 별도 monitor handle에 대한 `zlink_poll`과 `zlink_socket_monitor_recv`로 수행한다. DEALER의 `EVENTS` query·recv·completion drain으로 측정을 진행시키지 않는다.
- 모든 시간은 disconnect 호출 **반환 이후 monitor event를 받아 확인한 시점**까지의 wall time이다. `immediate-request`에는 connect와 submit 실행 시간이 포함된다. 각 row는 5회 최소–최대이며 계약상 latency 상한을 새로 정의하지 않는다. Event watchdog은 처음부터 2,000 ms다.
- Inproc에서는 새 READY가 old terminal보다 먼저 관측될 수도 있다. 테스트는 두 physical connection 간 monitor 순서를 추가로 가정하지 않는다.

## 결과: case × transport × application poll

`IN+COMPLETION`은 `POLLIN|POLLCOMPLETION`이다. Terminal은 모두 기존 connection ID에 해당하는 DISCONNECTED로 확인했다.

| Case | Transport | 등록 bit | Application poll | Terminal ms | 새 READY ms | 반복 |
|---|---|---|---|---:|---:|---:|
| automatic | tcp | IN | idle | 9.322–10.673 | 129.447–187.333 | 5/5 |
| automatic | tcp | IN | blocked | 8.687–9.850 | 141.245–201.718 | 5/5 |
| automatic | tcp | IN+COMPLETION | idle | 8.464–10.647 | 128.444–209.494 | 5/5 |
| automatic | tcp | IN+COMPLETION | blocked | 8.770–10.426 | 129.902–181.368 | 5/5 |
| automatic | inproc | IN | idle | 9.377–10.657 | 9.379–10.661 | 5/5 |
| automatic | inproc | IN | blocked | 9.511–11.017 | 9.514–11.019 | 5/5 |
| automatic | inproc | IN+COMPLETION | idle | 9.342–10.679 | 9.346–10.682 | 5/5 |
| automatic | inproc | IN+COMPLETION | blocked | 9.643–10.689 | 9.646–10.694 | 5/5 |
| disconnect | tcp | IN | idle | 8.857–10.763 | 해당 없음 | 5/5 |
| disconnect | tcp | IN | blocked | 9.767–10.784 | 해당 없음 | 5/5 |
| disconnect | tcp | IN+COMPLETION | idle | 9.709–10.851 | 해당 없음 | 5/5 |
| disconnect | tcp | IN+COMPLETION | blocked | 9.690–10.855 | 해당 없음 | 5/5 |
| disconnect | inproc | IN | idle | 9.320–10.808 | 해당 없음 | 5/5 |
| disconnect | inproc | IN | blocked | 9.413–10.523 | 해당 없음 | 5/5 |
| disconnect | inproc | IN+COMPLETION | idle | 9.396–10.659 | 해당 없음 | 5/5 |
| disconnect | inproc | IN+COMPLETION | blocked | 9.432–10.674 | 해당 없음 | 5/5 |
| immediate-request | tcp | IN | idle | 8.449–10.565 | 8.452–10.568 | 5/5 |
| immediate-request | tcp | IN | blocked | 9.645–10.650 | 9.650–10.655 | 5/5 |
| immediate-request | tcp | IN+COMPLETION | idle | 9.424–10.734 | 9.426–10.745 | 5/5 |
| immediate-request | tcp | IN+COMPLETION | blocked | 9.472–10.430 | 9.477–10.435 | 5/5 |
| immediate-request | inproc | IN | idle | 8.650–10.731 | 8.647–10.729 | 5/5 |
| immediate-request | inproc | IN | blocked | 9.431–10.811 | 9.429–10.809 | 5/5 |
| immediate-request | inproc | IN+COMPLETION | idle | 9.297–10.690 | 9.294–10.685 | 5/5 |
| immediate-request | inproc | IN+COMPLETION | blocked | 9.528–10.693 | 9.526–10.690 | 5/5 |

총 **24 case × 5 = 120회 PASS**. 전체 관측 최대는 terminal **11.017 ms**, 자동 READY **209.494 ms**다. 같은 endpoint의 즉시 REQUEST/reply는 tcp·inproc, 두 mask, idle·blocked 모두 성공했다.

## 변경 파일

Worktree 기준 runtime 파일:

- `core/src/runtime/core/pipe.cpp`
- `core/src/runtime/sockets/common/socket_base.hpp`
- `core/src/runtime/sockets/common/socket_base_api.cpp`
- `core/src/runtime/sockets/common/socket_base_dispatch.cpp`
- `core/src/runtime/sockets/common/socket_base_endpoint.cpp`
- `core/src/runtime/sockets/common/socket_base_lifecycle.cpp`
- `core/src/runtime/sockets/common/socket_endpoint_runtime.cpp`
- `core/src/runtime/sockets/common/socket_runtime.hpp`

테스트는 `core/tests/integration/test_disconnect_progress.cpp`, 등록은 `core/tests/CMakeLists.txt`다. CTest 이름은 `test_disconnect_progress_<tcp|inproc>_<in|completion>_<automatic|disconnect|request>_<idle|blocked>` 24개다. `ZLINK_TEST_CASE`로 단일 case를 선택하고 `ZLINK_PROGRESS_TRACE=1`로 monitor event correlation을 출력할 수 있다. Public C API만 사용하며 sleep, 내부 hook, private header, assertion 완화가 없다.

## Gate 결과

| 검증 | 결과 |
|---|---|
| `nice -n 10 env JOBS=4 scripts/build-core.sh dev` | PASS, worktree의 shared/static runtime와 test 실행파일 갱신 |
| 새 regression `ctest --test-dir core/build-dev -R '^test_disconnect_progress_' -j2 --repeat until-fail:5 -V` | 24 × 5 = 120 PASS |
| 관련 polling/monitor/reconnect/wake subsystem | 7/7 PASS |
| 전체 `ctest --test-dir core/build-dev -j2 --output-on-failure` | **167/168 PASS**, hotpath_gate 1건 FAIL; 191.87 s |
| `^test_single_lane_` 2회, `-j2` | 29/29 + 29/29 PASS |
| C/Cpp/Go/Rust public header mirror | 12/12 PASS |
| `git diff --check` | PASS |
| C++ contract / sample-smoke, `-j2` | 16/16 + 7/7 PASS |
| Python tests / samples | 180 PASS, 4 subtests PASS / 7/7 PASS |
| LTO `hotpath_gate` | **4/4 cell PASS**, CTest 1/1 PASS |
| `scripts/` hotpath policy checker | 독립 checker 없음. §3 금지 항목 diff 검토 완료 |

C++ 검증은 원본 `bindings/cpp`를 source로 사용하고 출력만 `core/build-cpp-gate`에 둔 동등 CMake configuration으로 실행했다. `ZLINK_CPP_CORE_BUILD_DIR`는 이 worktree의 `core/build-dev`다. Python은 원본 `bindings/python`을 수정하지 않기 위해 동일 소스를 `core/build-dev/python-gate-root/bindings/python`에 복사하고 그 안에 native extension을 배치했다. Core header와 binding spec 참조는 원본으로 연결했다. 원본 test assertion을 그대로 실행했으며 결과를 건너뛴 test는 없다. 두 binding 검증 모두 이 worktree의 수정된 dev library를 사용했다.

Dev 전체 ctest의 hotpath 실패는 숨기지 않는다. No-LTO dev에서는 모든 cell이 reference보다 25.66–31.67% 높았다. `scripts/build-core.sh release-gate --lib-only` 후 worktree의 `core/build`에서 `hotpath_bench`만 빌드하고 해당 gate를 대조했다. 전체 ctest는 반복하지 않았다. Reference 파일은 변경하지 않았다.

| Hotpath cell | Reference instructions/message | Dev 측정 | LTO 측정 | LTO/reference | LTO 결과 |
|---|---:|---:|---:|---:|---|
| dealer_dealer_inproc | 3455.381 | 4476.003 | 3451.928 | 0.9990 | PASS |
| dealer_router_reqrep_inproc | 12054.895 | 15148.667 | 12071.287 | 1.0014 | PASS |
| pair_inproc | 2681.957 | 3531.224 | 2702.381 | 1.0076 | PASS |
| router_router_tcp | 2972.882 | 3855.825 | 2986.103 | 1.0044 | PASS |

Hotpath spec §2의 poll/command 경로에서는 socket owner 이전 및 pipe 종료 시 동작만 변경한다. 메시지마다 allocation, 문자열 lookup, socket table 조회, sleep 또는 새 작업을 추가하지 않는다. Endpoint 구성과 reconnect는 lifecycle의 드문 경로다. Release throughput 비교 benchmark는 per-message 구현 변경이 없어 실행하지 않았다.

## 증거와 BLOCKERS

로그 보존 위치: `/home/hep7/project/zlink-core-b/core/build-dev/disconnect-progress-logs/`.

- 재현: `idle-repro.log`, `inproc-auto-baseline.log`, `inproc-trace.log`.
- 측정: `progress-repeat5.log`; 기능 검증: `subsystem.log`, `full-gate.log`, `single-lane-1.log`, `single-lane-2.log`.
- 추가 gate: `cpp-contract.log`, `cpp-sample.log`, `python-final.log`, `hotpath-lto.log`.

**BLOCKERS: 수정 범위의 기능 실패와 LTO 성능 gate 실패 없음.** Dev 전체 invocation은 hotpath 기준과 build mode 차이로 167/168이며 green으로 표시하지 않는다. 동일 source의 LTO hotpath 대조는 통과했다. Supervisor는 이 diff를 반영한 Core가 실제 사용되는지 확인한 뒤 Framework의 두 번째 poller를 제거할 수 있다. Spec·binding·Framework 변경, commit·push는 수행하지 않았다.
