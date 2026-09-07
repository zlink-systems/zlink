# Core RF MP-7 결과 보고서

## 결과

열린 caller-local REQUEST sequence와 무관하게, completion poller가 등록된 socket에서
`zlink_completion_recv(NONE)`가 transport reply를 public completion queue까지 직접 진행하도록
수정했다. MP-6에서 추가한 REQUEST 회귀는 수정 전 inproc·tcp 모두
`ZLINK_RECV_NO_DATA`/`EAGAIN`이었고, 최종 코드에서 REQUEST·SEND 두 변형 각각 10/10을
통과했다.

MP-7 source delta는 기존 MP-3+4+5+6 patch 위의 3개 파일, 57 insertions/10 deletions다.
공개 API와 ABI, 스펙은 바꾸지 않았다.

## 원인

원인은 열린 public staging을 검사하는 잔존 분기가 아니라 **completion drain owner의 진행
공백**이었다.

- `socket_base_dispatch.cpp:130-162`의 `ensure_completion_processing()`은 completion poller ref가
  있으면 그 poller를 안정된 transport completion owner로 인정하고 async owner를 만들지 않는다.
- 수정 전 `socket_message_handler_api.cpp:137-151`의 blocking completion pull은
  `ensure_completion_processing()` 뒤 public queue의 condition variable만 기다렸다. Poller가
  등록된 경우 async publisher는 없고, 이 pull도 poller의 physical drain turn을 실행하지 않아
  transport head가 준비돼도 queue를 채울 주체가 없었다.
- 실제 reply→completion publish는 `socket_base_api.cpp:1532-1545`의
  `drain_claimed_completion_pipe()`가
  `socket_request_reply_dispatch.cpp:189`의 `process_completion_pipe()`를 호출한 뒤,
  `socket_request_reply_dispatch.cpp:151-165`에서 request ID로 pending entry를 꺼내 수행한다.
  임시 gated trace에서는 B의 `request_seq=2` reply가 blocking pull의 timeout 동안 이 경로에
  진입하지 않았고, timeout 뒤 A FINAL이 만든 다음 socket progress에서 즉시 drain·publish됐다.
  따라서 pending cookie나 request ID 충돌, queue publish 거절은 아니었다.
- `part_helper_api.cpp:187-224`에서 REQUEST/SEND caller-local staging은 socket-wide public send
  scope나 marker를 잡지 않는다. `socket_send_complete.cpp:440-447`의
  `public_multipart_send_active()` control defer는 physical incremental write를 유지하는 PUB/XPUB에만
  해당하므로 이번 REQUEST 정지 원인이 아니다. D-B198 D3의 public staging 중 control/completion
  진행 계약과도 이 구분이 일치한다.

## 수정

- `socket_base.hpp:574-576`, `socket_base_lifecycle.cpp:578-615`에 내부
  `prepare_completion_pull()`을 추가했다.
- public queue가 이미 준비됐으면 즉시 반환한다. Poller가 없으면 기존
  `ensure_completion_processing()`과 async owner/queue wait를 그대로 사용한다.
- Poller가 등록된 blocking pull이면 기존 `get_events(ZLINK_POLLCOMPLETION)` owner gate로 같은
  physical drain turn을 수행한다. 아직 reply가 도착하지 않았으면 기존 mailbox
  `process_commands()`를 하나의 `wait_timeout_budget_t` 안에서 기다린 뒤 다시 drain한다. 새 poller,
  상태, timer, retry 횟수는 추가하지 않았다.
- `socket_message_handler_api.cpp:137-155`는 위 함수가 이미 timeout budget을 소비해 queue head를
  publish했을 때 queue 자체는 DONTWAIT로 한 번만 꺼낸다. Poller가 없는 async 경로는 종전 queue
  timeout과 lifecycle wake를 유지한다.
- DONTWAIT completion pull은 종전처럼 public queue만 확인한다. 첫 구현에서 DONTWAIT에도 physical
  drain을 허용하자 `unittest_phase3_request_reply_owners`의 owner fairness 계약이 timeout되어,
  blocking `NONE`에만 owner progress를 부여하고 관련 suite로 복원 여부를 확인했다.

비교한 대안은 다음과 같다.

1. 채택: blocking pull을 이미 등록된 단일 completion owner의 또 다른 wait 형태로 보고 기존
   poller drain gate와 timeout budget을 재사용한다.
2. 기각: poller ref를 무시하고 별도 async publisher를 유지하면 physical drain owner가 둘이 된다.
3. 기각: caller/test가 reply 뒤 반드시 `zlink_poller_wait()`를 호출하게 하면 blocking
   `zlink_completion_recv()`의 공개 계약을 호출자 절차로 우회한다.

수정 전/후 규칙 수: **2 → 1**. 수정 전에는 poller wait만 transport reply를 queue로 옮기고
blocking pull은 다른 호출의 진행에 의존했다. 수정 후에는 등록된 단일 completion owner의 blocking
wait도 동일한 drain turn을 수행한다. DONTWAIT의 queue-only 규칙은 기존 nonblocking/fairness 계약을
그대로 유지한다.

## 검증

모든 build와 test는 foreground에서 `JOBS=4`/parallel 4 이하로 실행했고, 동시에 실행 중인
`ninja`/`gmake`가 없음을 확인했다.

| 검증 | 결과 |
|---|---:|
| 수정 전 신규 REQUEST, inproc·tcp | 0/2, `NO_DATA/EAGAIN` 재현 |
| 신규 REQUEST·SEND | 각 10/10 통과 |
| dev 관련 정규식 81 target `until-fail:3` | 243/243 통과, 408.51초 |
| dev `wake-invariant` 4 target `until-fail:10` | 40/40 통과, 312.14초 |
| ASan+LSan 관련 8 target | 8/8 통과, sanitizer/leak 없음 |
| GCC TSan 관련 10 target | 10/10 통과, race 없음, 24.61초 |
| Release+LTO library 및 정적 `hotpath_bench` | 빌드 성공 |
| `git diff --check` | 통과 |
| `core/include`, `core/src/libzlink.vers` diff | 없음 |
| 임시 `ZLINK_MP7_DEBUG` | 최종 source에 없음 |

TSan은 `-fsanitize=thread -fno-omit-frame-pointer -fPIE`로 계측된 기존 build tree와 기존
`receive_once_guarded`, `check_read` suppression을 사용했다. 대상은 MP-5의 8개에 신규 REQUEST·SEND
2개를 더한 10개다.

## 성능

Release+LTO library와 정적 runner를 현재 소스로 갱신했다. 다른 build가 없는 상태에서 측정 시작
load average는 **2.66 / 1.83 / 1.34**였고, 지정된 `PERF_LOCK` 아래 hotpath 5셀을 한 번 실행했다.
Reference는 갱신하지 않았다.

| cell | MP-5 Ir/msg | MP-7 Ir/msg | MP-5 대비 | ±1% 목표 | 공식 gate |
|---|---:|---:|---:|---:|---:|
| `dealer_dealer_inproc` | 3,249.922 | 3,249.975 | +0.002% | PASS | PASS |
| `dealer_router_reqrep_inproc` | 18,329.474 | 18,437.315 | +0.588% | PASS | PASS |
| `pair_inproc` | 2,326.905 | 2,325.784 | -0.048% | PASS | PASS |
| `router_router_tcp` | 2,915.899 | 2,921.401 | +0.189% | PASS | PASS |
| `stream_tcp` | 14,234.011 | 14,050.047 | **-1.292%** | 목표 밖(개선) | PASS |

네 셀은 MP-5 ±1% 안이다. `stream_tcp`는 목표보다 0.292%p 더 낮지만 instruction 감소 방향이고,
공식 reference 대비 ratio 0.9608로 통과했다. 5셀 1회 조건에 따라 재측정하지 않았다. 이번 변경은
completion poller가 등록된 blocking completion pull의 cold wait 경로에만 있고 이 5개 steady-state
send hotpath에는 새 분기를 넣지 않는다.

## 변경 경계와 판정

- MP-7 source: `core/src/api/socket/socket_message_handler_api.cpp`,
  `core/src/runtime/sockets/common/socket_base.hpp`,
  `core/src/runtime/sockets/common/socket_base_lifecycle.cpp`.
- MP-6 신규 테스트는 기존 untracked 상태로 유지했으며 MP-7에서 수정하지 않았다.
- 시작 전 baseline은 지정 경로의 `mp6-before-mp7.patch`로 보존했다(202,045 bytes). 이를 임시 index에
  적용해 현재 tracked source와 비교한 MP-7-only patch는 4,988 bytes, 3 files임을 확인했다.
- stash·commit·branch 전환·스펙 수정은 하지 않았다.

소유 계층: Core socket completion owner가 transport completion pipe의 drain과 public completion
queue publish를 소유한다. Request/reply correlation은 기존 req/rep 모듈이 계속 소유한다.

spec 근거: `core/doc/spec/core/socket/README.ko.md:1132-1195`의 socket-local completion queue,
단일 drain owner, blocking timeout 계약과 `:991-1001`의 exact WRITABLE record/queue drain 계약;
D-B198 D3의 public staging 중 control 진행 계약.

교차언어: C++, .NET, Node, Java, Rust, Go binding 모두 이 Core completion C API에 위임한다. 언어별
reply publisher나 staging gate가 없으므로 Core 한 곳의 수정으로 동일 동작을 얻으며 binding 변경은
필요 없다.

변경 분류: **B — 기존 결함**. 등록된 단일 completion owner의 blocking API가 자신의 transport
drain을 진행하지 않아 공개된 completion timeout 계약을 위반했다.

spec 변경: 없음.
