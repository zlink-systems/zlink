# Core RF MP-8 결과 보고서

## 결과

2차 리뷰의 차단 항목 B201~B206과 비차단 항목 W201~W206, S201~S202를 Core의 기존
registry, lifecycle admission, physical send scope, mailbox epoch 소유자 안에서 수정했다. 기능
검증은 dev 전체 209/209, 관련 반복 291/291, 신규·변경 반복 60/60, lost-wake 40/40,
ASan+LSan 6/6, TSan 10/10으로 통과했다.

성능 5셀 중 네 셀은 MP-7 대비 ±1% 안이다. `dealer_router_reqrep_inproc`은
**-11.912%**로 명령 수가 크게 줄어 ±1% 목표와 공식 양방향 gate를 벗어났다. 기능 실패나
회귀 방향은 아니지만 지정 gate 결과는 숨기지 않고 FAIL로 남긴다. 5셀 1회 조건에 따라
재측정하지 않았고 reference도 수정하지 않았다.

작업 시작 전 MP-3~7 patch는 지정 경로의 `mp7-before-mp8.patch`에 보존했다. 크기는
206,778 bytes(4,622행), SHA-256은
`96f4c07e0e38811a1e440649f7f7c9148f54fad62e632019cbb0c92cefa74221`이다.

## 리뷰 항목별 수정

| ID | 수정과 소유자 | 파일:행 | 고정 테스트 |
|---|---|---|---|
| B201 | registry의 checked-out tombstone이 이미 `revoked`이면 revoke 순회에서 건너뛴다. live→revoked 전이에서만 checkout·slot을 한 번 반환한다. | `core/src/api/socket/socket_request_reply_runtime_io.cpp:836-879` | `unittest_phase3_request_reply_owners.cpp:891-1024`: 반복 public disconnect, 같은 RID replacement generation 재제거, 늦은 restore와 commit |
| B202 | DONTWAIT FINAL도 helper lookup·조작·실패 분리까지 기존 public lifecycle admission을 유지한다. 분리된 payload 해제 전에는 helper mutex를, physical submit 전에는 staging admission을 놓는다. | `socket_message_send_api.cpp:391-503`, `socket_request_reply_submit_api.cpp:476-706,812-905,1038-1128` | `unittest_phase3_request_reply_owners.cpp:770-813`: REQUEST FINAL staging allocation failpoint의 callback에서 다른 thread close가 `ZLINK_CLOSE_BUSY` |
| B203 | detached buffer와 family context의 해제를 `release_detached_send_ownership()` 하나로 모으고, 공용 abort helper의 local socket pin을 context reset까지 유지한다. Entry별 pin 복사는 추가하지 않았다. | `part_helper_api.cpp:123-134,1021-1078` | `unittest_phase3_request_reply_owners.cpp:815-889`: REPLY MORE 뒤 publish invalid flags, zero-copy callback을 막은 동안 close 완료 후 context restore |
| B204 | mailbox command epoch 관찰을 drain보다 먼저 시작하고, ready 재확인 뒤 같은 epoch로 command wait한다. 단일 entry `wait_timeout_budget_t`를 registration 전환 뒤에도 재사용한다. Async monitor owner이면 epoch signal을 직접 기다려 busy loop를 제거한다. | `socket_base_lifecycle.cpp:606-660`, `socket_message_handler_api.cpp:128-151` | `unittest_phase3_request_reply_owners.cpp:1166-1228`: 다른 command owner가 wait 직전 reply command 소비. `:1230-1348`: poller_wait/NONE pull 중첩, monitor owner와 remove/add 전환, 200 ms entry deadline을 150~300 ms로 확인 |
| B205 | REPLY physical attempt는 원본의 shallow copy만 pipe에 넘기고 원본 whole record는 scope 밖까지 유지한다. REQUEST 성공도 physical sync를 놓은 뒤 원본을 소비한다. | `socket_request_reply_runtime_io.cpp:1388-1527`, `socket_request_reply_submit_api.cpp:541-575,654-706`, `socket_send_submit.cpp:603-647,798-819` | `unittest_phase3_request_reply_owners.cpp:1027-1097`: REPLY EIO callback의 `zlink_set_option`; `:1099-1164`: blocking REQUEST 성공 callback의 `zlink_set_option` |
| B206 | token의 RID·wire/type capability 일치를 checked-out 판정보다 먼저 검사한다. 다른 RID+checked-out token은 `ENOENT`, 유효 token의 중복 checkout만 `EBUSY`다. | `socket_request_reply_runtime_io.cpp:678-718` | `test_phase3_request_reply_contract.cpp:1643-1728`: 다른 RID `ENOENT`, 같은 RID 중복 `EBUSY`, active sequence의 RID/token 불일치 `EINVAL` 분리 |
| W201 | 최초 B FINAL 실패와 WRITABLE 뒤 재제출을 서로 다른 두 thread에서 실행해 실패 record의 caller 이전을 직접 관찰한다. | `test_writable_resubmit_from_other_thread_while_sequence_open.cpp:569-587,729-750` | REQUEST·SEND의 inproc/TCP case 통과 |
| W202 | 20 ms quiet-window 추론을 제거했다. Application lane을 prime한 뒤 공개 PAUSED flow 상태를 monitor로 확인하고, filler와 B를 같은 고정 rejection 경계에서 거절한다. 즉시 poller 조회로 filler token이 pending임도 확인한 뒤 RUNNING 전환으로 두 WRITABLE을 해제한다. | `test_writable_resubmit_from_other_thread_while_sequence_open.cpp:311-353,408-422,493-592` | REQUEST·SEND TCP가 time 부재 추론 없이 통과 |
| W203 | B02 회귀 callback이 `ZLINK_OPT_SNDHWM`을 실제 호출해 physical/helper lock 부재를 확인한다. Fresh worker가 MORE와 FINAL을 모두 수행해 이전 TLS 상태에 의존하지 않는다. | `test_helper_ownership.cpp:45-68,552-617` | `test_expired_zero_copy_prefix_is_released_outside_dontwait_send_scope` 통과 |
| W204 | contender message 초기화 실패도 `more_done`을 발행하고, 생성된 MORE·FINAL handle은 모든 종료 경로에서 닫는다. | `unittest_complete_record_admission.cpp:48-125` | target 전체 4/4 및 sanitizer 통과 |
| W205 | helper 탐색·회수 비용을 아래 별도 절에 수치화했다. | `part_helper_api.cpp:79-109,740-840` | helper ownership/interleave 및 hotpath |
| W206 | monitor가 async command owner를 보유하면 `process_commands()` 반복 대신 관찰한 mailbox epoch로 잠든다. | `socket_base_lifecycle.cpp:643-652` | B204 registration 전환 case에서 timeout 범위와 종료를 함께 확인 |
| S201 | abort 두 경로의 buffer/context close 순서와 pin 전제를 공용 detached-owner helper로 통일했다. | `part_helper_api.cpp:123-134,1021-1078` | B203과 helper ownership 17/17 |
| S202 | completion pull 주석을 실제 단일 drain/queue 동작으로 맞추고 timeout 0 전용 도달 불가 분기를 제거했다. | `socket_base.hpp:581-586`, `socket_base_lifecycle.cpp:606-660`, `socket_message_handler_api.cpp:128-151` | completion pull 두 신규 case와 전체 dev suite |

## W205 비용과 회수 범위

- 2-part SEND 성공은 MORE prepare 1회와 FINAL prepare 1회로 helper mutex **2회**다.
- 2-part REQUEST와 REPLY 성공은 MORE prepare 1회, FINAL active/prelookup 1회, FINAL prepare
  1회로 각각 helper mutex **3회**다.
- 만료 회수는 identity가 있는 helper 접근에서 caller map `k`개를 **한 번 O(k) 순회**한다.
  만료 node가 있으면 잠금 밖 해제를 위해 한 번 unlock/relock한다.
- helper state가 없거나 identity가 없는 새 caller의 single FINAL은 relaxed-zero negative filter로
  scan·identity 생성을 **0회** 수행한다. 따라서 그 cold call은 만료 slot을 회수하지 않으며,
  다음 identity 보유 helper 접근 또는 socket close가 회수 경계다.

## 설계 비교

대안 1은 별도 revoked counter/generation 표, entry별 pin, completion 전용 poller 또는 재시도
횟수·timeout을 추가하는 방식이다. 동일 사실의 소유자가 늘고 RID·helper·completion 상태가 이중화된다.

선택한 대안 2는 기존 `revoked` bit가 live→revoked 전이를 소유하고, 기존 public admission이
helper 수명을, 공용 abort helper가 detached payload/context와 pin을, 기존 completion drain gate와
mailbox epoch가 drain/wake/deadline을 각각 소유하게 하는 방식이다. 새 public API, option, poller,
retry counter, timeout은 추가하지 않았다.

수정 전/후 규칙 수: **차단 경로별 예외 6개 → 기존 소유자 규칙 4개**(registry 전이,
helper detach 수명, physical attempt/whole-record 수명, completion epoch/deadline)로 줄였다.

## 검증

| 검증 | 결과 |
|---|---:|
| `JOBS=4 scripts/build-core.sh dev` | PASS |
| dev 전체 `ctest -E hotpath_gate` 1회 | **209/209 PASS**, 232.25초 |
| 관련 정규식 97 target `until-fail:3` | **291/291 PASS**, 418.72초 |
| 신규·변경 6 target `until-fail:10` | **60/60 PASS**, 136.43초 |
| W202 최종 REQUEST·SEND inproc/TCP `until-fail:10` | **20/20 PASS**, 1.51초 |
| B204 lost-wake 두 case `until-fail:20` | **40/40 PASS** |
| ASan+LSan 관련 6 target | **6/6 PASS**, 14.48초, sanitizer/leak 오류 0 |
| MP-3 GCC TSan 관련 10 target | **10/10 PASS**, 24.64초, race 오류 0 |
| W202 최종 ASan+LSan / TSan 재실행 | 각각 **2/2 PASS**, sanitizer·race 오류 0 |
| Release+LTO shared lib 및 static `hotpath_bench` | PASS |
| `git diff --check` | PASS |
| `git diff --stat -- core/include core/src/libzlink.vers` | 출력 없음 |

TSan은 MP-3 구성의 `-fsanitize=thread -fno-omit-frame-pointer -fPIE`, `setarch x86_64 -R`,
기존 `/tmp/mp2-tsan.supp`의 `receive_once_guarded`·`check_read` suppression을 그대로 사용했다.
관련 10개는 public multipart, helper ownership/interleave, phase3 request/reply, REQUEST·SEND
WRITABLE 이전, single-lane control boundary, complete admission, request/reply owner, ZMP edge다.

## 성능

Release+LTO library와 static runner를 현재 source로 갱신했다. 다른 ninja가 없는 상태에서
측정 시작 load average는 **3.46 / 1.76 / 1.14**였고, 지정된 `PERF_LOCK` 아래 5셀을 한 번
실행했다.

| cell | MP-7 Ir/msg | MP-8 Ir/msg | MP-7 대비 | ±1% 목표 | 공식 reference gate |
|---|---:|---:|---:|---:|---:|
| `dealer_dealer_inproc` | 3,249.975 | 3,249.917 | -0.002% | PASS | PASS |
| `dealer_router_reqrep_inproc` | 18,437.315 | 16,241.043 | **-11.912%** | **FAIL(개선)** | **FAIL(0.8702)** |
| `pair_inproc` | 2,325.784 | 2,325.990 | +0.009% | PASS | PASS |
| `router_router_tcp` | 2,921.401 | 2,909.839 | -0.396% | PASS | PASS |
| `stream_tcp` | 14,050.047 | 14,036.155 | -0.099% | PASS | PASS |

Req/rep 셀만 B204가 직접 바꾼 blocking completion pull을 매 record 사용한다. MP-7의 no-poller
경로는 async owner 설치 뒤 public queue condition wait로 넘겼지만, MP-8은 동일한 기존 drain
gate에서 즉시 drain하고 ready queue를 재확인한다. 따라서 2,196.272 Ir/msg 감소는 이 왕복 제거와
방향이 일치한다. 이는 코드 경로에 근거한 해석이며 별도 재측정 증거로 확대하지 않는다.

## 변경 경계와 계약 판정

MP-8 source 수정은 다음 파일에 한정된다.

- `core/src/api/socket/part_helper_api.cpp`
- `core/src/api/socket/socket_message_handler_api.cpp`
- `core/src/api/socket/socket_message_send_api.cpp`
- `core/src/api/socket/socket_request_reply_runtime_io.cpp`
- `core/src/api/socket/socket_request_reply_submit_api.cpp`
- `core/src/runtime/sockets/common/socket_base.hpp`
- `core/src/runtime/sockets/common/socket_base_lifecycle.cpp`
- `core/src/runtime/sockets/common/socket_send_submit.cpp`

MP-8 테스트 수정은 `test_helper_ownership.cpp`, `test_phase3_request_reply_contract.cpp`,
`unittest_complete_record_admission.cpp`, `unittest_phase3_request_reply_owners.cpp`와 MP-6부터
untracked인 `test_writable_resubmit_from_other_thread_while_sequence_open.cpp`다. Worktree 전체에는
MP-3~7 변경이 함께 남아 있다. stash, commit, branch 전환, 스펙 수정은 하지 않았다.

소유 계층: Core request/reply registry가 token capability와 live→revoked 전이를, Core part helper가
caller별 staging과 detached cleanup을, Core socket physical scope가 pipe admission을, Core completion
owner가 transport drain·mailbox wake·public queue publish를 소유한다.

spec 근거: main 미커밋 `core/doc/spec/core/socket/README.ko.md:1120-1149`의 REPLY checkout,
RID별 capability, 실패 재시도와 invalidation/slot 계약; `:1201-1219`의 단일 drain owner,
poller와 NONE pull, queue-only DONTWAIT, entry RCVTIMEO 계약; `core/doc/spec/core/05-polling.ko.md:100-126`의
poller 비소비와 registration owner/전환 계약이다. 어느 문장도 다른 동작이 되지 않았다.

교차언어: Framework 언어 runtime은 변경하지 않았다. C++, .NET, Node, Java, Rust, Go binding은
동일한 Core C API의 token registry, part submit, completion pull에 위임하므로 언어별 retry·poller·
generation 보상 없이 Core 한 곳에서 같은 계약을 얻는다.

변경 분류: **B — 기존 결함**. 채택된 B201·B204·B206과 같은 Core 소유 경로의 counter,
lifecycle, wake, 오류 우선순위 결함이며 상위 계층 우회가 아니다.

spec 변경: 없음. 공개 header·ABI/export 변경: 없음. patch는 미커밋 상태다.

남은 실패: 기능·sanitizer 실패는 없다. 성능에서 개선 방향의 req/rep instruction count가
MP-7 ±1% 및 공식 양방향 gate를 벗어났으므로 감독자가 reference 재기준 여부를 판단해야 한다.
