# C++ REQUEST terminal 분리 결과

## 결과

`raw_route_port_t::request()`가 D-B85의 두 실패를 operation phase와 binding의
structured result를 포함해 구분하도록 수정했다.

| 관측 단계 | 설치 binding에서 확인한 값 | Framework backend 결과 |
| --- | --- | --- |
| Routing map에 없는 RID로 DONTWAIT REQUEST | initial admission, `submit_result_t::not_connected`, `EHOSTUNREACH`; task가 즉시 끝나므로 wait token이 없다 | `route_unavailable`; durable sender가 전체 deadline 안에서 재전송할 수 있다 |
| route가 있으나 PAUSED라 wait token이 발급된 뒤 `disconnect_rid()` | completion terminal, `submit_result_t::not_found`, `ENOENT` | `failed`; 재전송하지 않고 상위 mapper에서 `Unavailable`로 끝낸다 |

첫 결과는 Core ROUTER 계약 `core/doc/spec/core/socket/07-router.ko.md:202-205,
215-222`, 두 번째 결과는 같은 문서의 `:215-220` 및 directed submit 요약의
wait-token terminal 계약을 따른다. 설치된 `.artifacts/wsl/install/zlink-cpp/0.17.0`
package만 사용했고 Core와 local package를 다시 만들지 않았다.

회귀 테스트는
`framework/languages/cpp/tests/Zlink.Framework.ContractTests/test_cpp_framework_raw_route_port_contract.cpp:87-167`에
추가했다. 두 번째 테스트는 connection-ready 뒤 receiver를 PAUSED 상태로 유지해 실제
nonzero wait token 경로를 만들고, request task가 pending인 것을 확인한 다음
`disconnect_rid()`를 호출한다. 이 target에는 release build에서도 assertion을 실행하도록
`-UNDEBUG`를 적용하고 `framework-unit` label을 추가했다. deadline, retry 횟수 또는 fixture
timeout은 늘리지 않았다.

## 변경

- `raw_route_port.hpp:51-69`: 내부 request completion에
  `initial_admission`/`completion_terminal`, binding submit/request result와
  `internal_errno`를 보존한다. public Framework API는 바뀌지 않았다.
- `raw_route_port.cpp:133-188`: `.async()`의 동기 initial admission과 `co_await` 뒤
  completion terminal을 별도 `try` 경계로 처리한다. 따라서 wait-token `ENOENT`가
  initial route absence 분기로 돌아가지 않는다.
- `raw_binding_adapter.hpp:79-108`: 분류표 소유자는 adapter 하나뿐이다. Core socket
  README의 submit-retry 표(`core/doc/spec/core/socket/README.ko.md:456-465`)대로 initial
  `NOT_CONNECTED+(ENOTCONN|EHOSTUNREACH)`와
  `NOT_ADMITTED+ECONNREFUSED`만 transient이며 `ENOENT`는 제외했다.
- `test_cpp_framework_m6a_runtime.cpp:2940-2956`: errno만 검사하던 회귀 assertion을
  structured result와 phase 조합을 검사하도록 바꿨다.
- `framework/languages/cpp/CMakeLists.txt`: runtime 검증으로 확장된 raw-route target에
  assertion 강제 실행과 `framework-unit` label을 적용했다.
- `test_cpp_framework_m6b_runtime.cpp:1305-1321`: known route를 사용할 수 없는 경우의
  기대를 `Unavailable`로 복원했다. 근거는 error model
  `00-foundation/07-framework-error-model.ko.md:75-85`와 routing
  `03-spot-actor/08-routing.ko.md:322-331`이다. 실제 reply가 deadline 안에 오지 않은
  경우에만 `DeadlineExceeded`다.

## 검증

환경은 `TMPDIR=/dev/shm/zlink-tmp-cpp`, build directory는
`framework/languages/cpp/build/linux-ninja-c-e2e`를 사용했다.

| 명령 또는 대상 | 결과 |
| --- | --- |
| `cmake --build ... -j4 --target test_cpp_framework_raw_route_port_contract` | PASS |
| `test_cpp_framework_raw_route_port_contract` | PASS; 위 두 structured outcome 확인 |
| `cmake --build ... -j4 --target test_cpp_framework_m6a_runtime test_cpp_framework_m6b_runtime test_cpp_framework_channel_messaging test_cpp_framework_execution` | PASS |
| `ctest -R 'm6a\|m6b\|channel_messaging\|execution' --output-on-failure` | 3/4 PASS; m6b만 아래 BLOCKER로 실패 |
| `test_cpp_framework_m6a_runtime` | PASS, 5.23 s; permanent-absence 50 ms case의 raw terminal은 기존대로 `timed_out` |
| `test_cpp_framework_execution` | PASS, 1.46 s |
| `test_cpp_framework_channel_messaging` | PASS, 1.20 s |

`git diff --check`도 통과했다.

## BLOCKERS

### m6b public terminal은 아직 `DeadlineExceeded`다

복원한 assertion은
`test_cpp_framework_m6b_runtime.cpp:1320`에서 실패한다. 기존 mesh trace를 켠 재현은
해당 호출에 다음 순서를 남겼다.

1. `correlation=4`의 raw request가 initial
   `route_unavailable`(`raw_request_result_t` 값 3)로 끝난다.
2. remote handler admission이나 reply completion은 없다.
3. `raw_mesh_node_owner.cpp:230-235`가 initial route absence를 재전송 대상으로 처리한다.
4. 20 ms 전체 deadline이 먼저 소진되고,
   `request_bound_session_bind()`가 지정한
   `operation_terminal_t::timed_out`(`raw_mesh_node_owner.cpp:1922-1939`)으로 끝난다.
5. `mesh_node_runtime.cpp:3850-3857`이 원인이 pre-admission route retry 소진인지
   admitted request의 reply timeout인지 구분하지 않고 모든 `timed_out`을
   `DeadlineExceeded`로 바꾼다.

따라서 이번 ENOENT phase 수정으로 이 assertion을 통과시키는 것은 올바르지 않다. m6b
scenario는 wait-token `ENOENT`를 밟지 않고 처음부터 routing map에 없는 RID를 사용한다.
public 오류를 `Unavailable`로 수렴하려면 raw infrastructure retry가 최종 callback까지
“한 번도 admission되지 않은 route retry 소진”과 “admission 뒤 reply deadline 만료”를
별도로 전달해야 한다. 현재 `operation_terminal_t::timed_out` 하나로 두 원인이 합쳐진다.

또한 actor model의 durable lifecycle replay 절
`03-spot-actor/04-actor-model.ko.md:668-678`은 route 부재를 같은 OperationId로 재전송하고
전체 deadline 소진 시 `DeadlineExceeded`라고 적지만, error model과 routing 절은 usable
owner route 상실을 `Unavailable`로 규정한다. m6b scenario에는 두 조항이 동시에 적용된다.
assertion을 다시 `DeadlineExceeded`로 낮추거나 topology 상태로 reply admission을 추정하지
않았다. supervisor가 public error 우선순위를 확정한 뒤 cause-bearing terminal을 별도 작업으로
연결해야 한다.

작업 중 이미 존재하거나 다른 작업에서 생긴 Node, binding provenance 및 untracked 변경은
수정하지 않았다. Core, bindings, 보호된 Framework 문서, 다른 언어 tree도 변경하지 않았다.
