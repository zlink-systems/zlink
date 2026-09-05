# C++ pre-admission route 재시도 terminal 분리 결과

## 결과

Route가 한 번도 request를 받아들이지 못한 채 전체 deadline이 끝나면
`Unavailable`로 종료한다. Route가 request를 받아들인 뒤 reply가 오지 않아 deadline이
끝난 경우에는 `DeadlineExceeded`를 유지한다.

- **소유 계층:** Framework raw mesh owner가 각 `raw_request_completion_t`의 admission
  결과를 보고 재시도와 최종 terminal을 결정한다. Public error model로 바꾸는 책임은
  `mesh_node_runtime_t`에 남는다.
- **Spec 조항:** `00-foundation/07-framework-error-model.ko.md:75-85`와
  `03-spot-actor/08-routing.ko.md:322-331`은 받아들여지지 않은 route 상실을
  `Unavailable`로 정한다. `03-spot-actor/04-actor-model.ko.md`의 sender replay 조항은
  남은 전체 deadline 동안 같은 요청을 다시 보내며, 받아들여진 request가 reply를
  기다리는 동안 deadline이 끝난 경우에만 `DeadlineExceeded`로 종료한다.
- **교차언어 대조:** Node Actor Join은 admission 전 `NotConnected`가 계속되면 마지막
  오류를 유지하여 `Unavailable`로 끝나므로 이번 C++ 결과와 같다. .NET과 Java Actor
  Join, Node STREAM actor bind에는 admission 전 소진을 `DeadlineExceeded`로 만드는
  경로가 남아 있어 후속 parity 작업이 필요하다.
- **변경 분류:** **B — 기존 결함.** Stage 1에서 initial admission과 completion
  terminal을 분리하자, C++ retry owner가 두 deadline 원인을 다시
  `operation_terminal_t::timed_out` 하나로 합치던 기존 결함이 드러났다.

## 변경

- `framework/src/runtime/foundation/operation_registry.hpp:21-29`
  - 내부 `operation_terminal_t`에 `route_unavailable`을 추가했다. 기존 enum 값의 순서를
    보존하려고 마지막에 추가했다.
- `framework/src/runtime/mesh/raw_mesh_node_owner.cpp:193-242,268-328`
  - Stage 1 adapter가 만든 `raw_request_result_t::route_unavailable`만 pre-admission
    재시도 대상으로 사용한다. errno나 topology로 다시 분류하지 않는다.
  - 재시도 중 deadline이 끝나면 `route_unavailable`을 operation callback까지 전달한다.
    이미 받아들여진 request가 `raw_request_result_t::timed_out`으로 끝나면 기존
    `timed_out` terminal을 그대로 전달한다.
  - 호출자별 `deadline_terminal` 인자를 제거해 같은 재시도 상태가 서로 다른 terminal을
    지정하지 못하게 했다.
- `framework/src/runtime/mesh/mesh_node_runtime.cpp:3850-3857`
  - 기존 public mapper가 `timed_out`만 `DeadlineExceeded`로 바꾸고 새
    `route_unavailable`은 `Unavailable`로 바꾼다. topology를 다시 조회하거나 별도 상태를
    추가하지 않았다.
- 새 terminal을 받는 기존 내부 경계도 같은 의미를 유지했다.
  - `client_server_failure_mapper.hpp`와 `spot_runtime.cpp`: disconnected 경계 오류
  - `user_spot_terminal_mapping.hpp`: `Unavailable`
  - `public_host_runtime.cpp`: binding `RequestResult.NotConnected`
- `test_cpp_framework_m6a_runtime.cpp:231-258,375-406`
  - bound-session bind와 Actor-create의 permanent route absence가 최종
    `route_unavailable` terminal인지 검사한다.
- `test_cpp_framework_m6b_runtime.cpp:1305-1376`
  - 복원된 route-absent 20 ms case가 `Unavailable`인지 검사한다.
  - 연결된 target mailbox가 `boundSessionBind`를 실제로 claim한 뒤 reply를 보류한다.
    이 20 ms case는 `DeadlineExceeded`인지 검사한다. operation deadline은 늘리지 않았다.

Core, bindings, 보호된 Framework 문서와 다른 언어 구현은 수정하지 않았고 local Core
package를 다시 만들지 않았다.

## 교차언어 근거

| 구현 | admission 전 route 실패 | deadline 소진 결과 | 판정 |
| --- | --- | --- | --- |
| .NET submit mapper | `ZLinkSubmitFailureMapper.cs:25-41`에서 `NotConnected`를 `Unavailable`로 변환 | `ZLinkActorRemoteJoiner.cs:180-209`는 Actor Join admission 전 deadline 소진을 `DeadlineExceeded`로 변환 | Actor Join 후속 필요 |
| Java | `ZLinkBackendRequestResult.java:30-41`에서 `NOT_CONNECTED`를 `UNAVAILABLE`로 변환 | `ZLinkActorSpotJoinCall.java:225-248`은 admission 전 deadline 소진을 `DEADLINE_EXCEEDED`로 생성 | Actor Join 후속 필요 |
| Node Actor Join | `actor-local-native-join.ts:689-705`가 `NotConnected`를 deadline까지 다시 시도한 뒤 마지막 오류를 그대로 던지고, `framework-errors-internal.ts:55-72`가 `RouteNotConnected`를 `Unavailable`로 변환 | `Unavailable` | C++과 같음 |
| Node STREAM actor bind | `managed-stream.ts:458-483`이 `NotConnected`를 다시 시도 | admission 전 deadline 소진을 `DeadlineExceeded`로 생성 | bound-session 후속 필요 |

## 검증

환경은 `TMPDIR=/dev/shm/zlink-tmp-cpp`, build directory는
`framework/languages/cpp/build/linux-ninja-c-e2e`를 사용했다.

| 명령 또는 대상 | 결과 |
| --- | --- |
| 다섯 target `cmake --build ... -j4` | PASS: m6a, m6b, channel_messaging, execution, raw_route_port_contract |
| `test_cpp_framework_m6a_runtime` | PASS, 5.21 s |
| `test_cpp_framework_m6b_runtime` | PASS, 6.00 s; route-absent와 admitted-withheld-reply 경계 확인 |
| `test_cpp_framework_channel_messaging` | PASS, 1.31 s |
| `test_cpp_framework_execution` | PASS, 1.54 s |
| `ctest -R 'm6a\|m6b\|channel_messaging\|execution\|raw_route_port' --output-on-failure` | PASS, 5/5, 14.06 s |
| 전체 `ctest --test-dir ... -j2 --output-on-failure` 한 번 | 67/69 PASS, 337.56 s; 아래 BLOCKERS 참조 |
| 전체 run 직후 `test_cpp_framework_m6b_runtime` 단독 재실행 | PASS, 6.04 s |
| `git diff --check` | PASS |

## BLOCKERS

- 전체 gate의 `test_cpp_framework_common_e2e_inventory`는 알려진 278개 open inventory
  condition 때문에 실패했다. 출력도 `FAIL: 278 required inventory conditions are open`을
  보고했다.
- 전체 gate에서 병렬 실행된 m6b는 이번 terminal case가 아닌
  `verify_public_host_route_cache_stops_at_owner_admission_deadline()`의
  `test_cpp_framework_m6b_runtime.cpp:1939` assertion에서 한 번 실패했다. 같은 binary는
  focused `ctest`와 전체 run 전후 단독 실행에서 모두 통과했다. 전체 gate는 요청대로
  한 번만 실행했으며 반복하지 않았다.
- C++ 구현 blocker는 없다. 다만 위 교차언어 표의 .NET·Java Actor Join과 Node STREAM
  actor bind 차이는 별도 parity 작업으로 남는다.
