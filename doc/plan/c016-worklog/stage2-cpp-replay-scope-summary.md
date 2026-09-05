# C++ durable replay scope Stage 2 결과

## 결과

**BLOCKED — 승인 진단의 handover 회귀 전제 보완 필요.** Runtime과 test는 수정하지 않았다.
사용자의 “진단이 불완전하면 구현을 중단하고 보고” 지시에 따라 중단했다.
조사 기준은 `main`, HEAD `502b915bb5`다.

- 소유 계층: Framework의 `infrastructure_request_retry_state_t`가 durable operation replay를 소유하고, Core가 request timeout과 handover를 소유한다.
- Spec 조항: Actor model `framework/doc/framework/common/spec/server/03-spot-actor/04-actor-model.ko.md:668-680`; Core socket `core/doc/spec/core/socket/README.ko.md:159-165`; C++ binding `bindings/doc/spec/cpp/README.ko.md:457-462`.
- 교차언어 대조: Java `framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/actors/ZLinkActorSubmitFaults.java:28-56`도 session-actor bind는 timeout을 retry하지만 bound-session bind는 제외한다. C++는 이미 typed request 결과를 보존한다는 구조적 차이가 있어 adapter 변경이 필요 없다는 진단은 확인했다.
- 변경 분류: 승인된 **A — 계약 적응**의 구현을 보류했다. 새로운 runtime 설계나 Core·binding 변경으로 확장하지 않았다.

## Diff

| 파일 | 변경 |
| --- | --- |
| `doc/plan/c016-worklog/stage2-cpp-replay-scope-summary.md` | 결과와 BLOCKERS 기록 |
| C++ runtime / test | 변경 없음 |

기존 .NET·Node 작업 및 untracked 디렉터리는 보존했다. Core·binding package 재빌드,
deadline 변경, assertion 변경과 commit은 수행하지 않았다.

## BLOCKERS

승인 진단의 C++ 회귀 3번은 “전체 deadline 전에 발생한 1회 typed timeout 뒤 두 번째
attempt가 성공”하는 fixture를 요구한다. 그러나 현재 Core 계약과 C++ 호출 경로에는
그 조기 timeout의 근거가 없다.

1. `raw_mesh_node_owner.cpp:195-204`는 `ceil(deadline - now)` 전부를
   `raw_route_port_t::request()`에 전달한다.
2. `raw_route_port.cpp:149`는 그 값을 그대로 binding `.timeout(timeout).async()`에
   전달한다. `:172-177`은 Core의 typed request terminal을 받아 전달하며 별도의
   조기 timeout을 만들지 않는다.
3. C++ binding spec `README.ko.md:460-462`는 builder timeout이 Core 소유 reply
   deadline을 지정한다고 명시한다.
4. Core socket spec `README.ko.md:159-165`는 handover에서 밀려난 transport pair의
   request가 **자기 timeout**으로 한 번 종결된다고 명시한다. Handover가 즉시
   `timed_out`을 반환한다는 계약이 아니다.
5. Core의 기존 공개 C API 회귀
   `core/tests/integration/test_router_reciprocal_handover_lanes.cpp:395-434`도
   losing request에 자체 timeout을 설정하고 handover 후 그 timeout completion을 받는다.
   이 fixture를 이번 작업에서 실행하거나 수정하지는 않았다.

따라서 실제 handover 또는 reply 유실로 첫 request가 자신의 timeout까지 기다리면,
completion 시점에는 Framework operation deadline도 소진된다. Predicate에 timeout을
추가해도 `schedule_retry()`의 deadline 검사(`raw_mesh_node_owner.cpp:268-275`)에서
종료하므로 요청된 두 번째 ingress 성공을 입증할 수 없다. `admittedOnce` 추가는 이
회귀 전제 문제를 해결하지 않는다.

감독이 보완할 사항은 조기 typed timeout의 근거와 회귀의 검증 범위다. Synthetic typed
completion을 주입하는 fixture는 조건부 retry 동작을 검증할 수 있지만, 현재 transport의
실제 handover 복구를 입증하는 테스트와는 검증 범위가 다르다. 현재 raw port는 구체 타입이며
기존 request 주입 지점이 없다(`raw_route_port.hpp:73-96`). Test seam을 추가하거나
timeout을 분할하는 방식으로 임의 확장하지 않았다.

## 검증 결과

| 검증 | 결과 |
| --- | --- |
| Branch·기존 변경 확인 | `main`; 요청 범위 밖 변경 보존 |
| 승인 진단·Framework/Core/binding 계약·교차언어 코드 대조 | 위 BLOCKER 확인 |
| `git diff --check` | 통과 |
| C++ build·4개 unit binary | 미실행 — 구현 전 중단 |
| Focused `ctest -R 'm6a|m6b|channel_messaging|execution|raw_route_port'` | 미실행 — 구현 전 중단 |
| 전체 `ctest -j2` (요청 inventory 278) | 미실행 — 구현 전 중단; inventory 재확인 안 함 |
| TicTacToe·GameQuest 각 1회 | 미실행 — 구현 전 중단 |

실행한 runtime test가 없으므로 새로운 test 실패나 통과를 주장하지 않는다. 기존
never-admitted `Unavailable` 및 withheld-reply `DeadlineExceeded` assertion은 그대로다.
