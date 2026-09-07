# MP-9 결과 — 3차 리뷰 수정과 req/rep Ir 감소 귀속

## 결과

3차 리뷰의 차단 B301을 해소했다. Completion drain이 늦은 REPLY 또는 payload export OOM으로
폐기할 record를 owner turn의 로컬 저장소로 분리하고, registry mutex·physical API sync·completion
owner gate가 모두 풀린 뒤 공용 함수 하나로 닫는다. 무등록 `completion_recv(NONE)`과 poller,
async owner, blocking send의 poller-owner 차용 경로가 같은 정리 함수를 사용한다.

W301·W303 테스트를 요구된 실제 대기와 FINAL 경계로 강화했고, S301의 SEND DONTWAIT staging
admission과 complete scope 중첩을 record detach 직후 인계로 없앴다. S302의 도달 불가 REQUEST
MORE 분기도 제거했다.

Req/rep Callgrind 재측정은 MP-7 **18,443.1536** → 최종 **16,352.5132 Ir/msg**,
**−2,090.6404 Ir/msg(−11.335%)**다. D-B212의 MP-8 측정치와 비교한 공식 감소
−2,196.2722 Ir/msg와 105.6318 Ir/msg 차이가 있지만, 이번 두 endpoint는 각각 기존 MP-7
측정의 +0.032%, MP-8 측정의 +0.686%로 같은 성능 상태다. 함수 self 비용과 호출 수는 감소의
주인이 async post/dispatch·queue wait 왕복 제거임을 확인하며, 새 direct drain·copy는 오히려
비용을 더한다.

시작 전 누적 diff는
`/tmp/claude-1000/-home-hep7hep7-project-zlink/a5b31a9a-1a3b-4bcb-a080-53988ed569cb/scratchpad/mp8-before-mp9.patch`
에 보존했다. Patch는 미커밋
상태이며 stash·commit·스펙 수정은 하지 않았다.

## 항목별 수정

| 항목 | 수정과 근거 |
|---|---|
| B301 | `socket_request_reply_dispatch.cpp:131-170`에서 pending lookup의 mutex 범위에는 registry 전이만 남기고, pending 없음과 payload export OOM은 `completion_message_discard_deferred`를 반환한다. `:192-204`의 공용 정리가 errno를 보존하며 record를 닫는다. Single frame은 `:304-325`에서 move, multipart는 `:371-387`에서 buffer ownership을 이동하므로 분리 자체는 무할당이다. |
| B301 owner 경계 | `socket_base_api.cpp:762-808`의 무등록 NONE용 `get_events()`는 public physical sync와 owner gate를 모두 벗어난 뒤 닫는다. Poller 경로도 `:880-904`, 공통 pipe owner는 `:1363-1497,1610-1687`에서 한 폐기 record를 분리하면 현재 pipe를 재게시하고 turn을 양보한다. Async owner는 `socket_base_lifecycle.cpp:1499-1519`, poller-owner 차용 send는 `socket_send_submit.cpp:805-845`에서 같은 정리를 gate 밖에서 호출한다. |
| B301 테스트 | `unittest_phase3_request_reply_owners.cpp:1215-1336`에 requester timeout 뒤 늦은 zero-copy REPLY의 free callback이 같은 DEALER에 `zlink_set_option`을 호출하는 무등록 NONE·poller wait 두 변형과 payload export OOM 변형을 추가했다. |
| W301 | `unittest_phase3_request_reply_owners.cpp:1338-1642`에 짧은 RCVTIMEO, 별도 condition-variable watchdog, elapsed 상한을 뒀다. Monitor 경로는 기존 mailbox waiter count를 노출한 test-only `socket_base.hpp:409`, `socket_base_api.cpp:1357-1360`으로 registration 1→0과 0→1 뒤 실제 CV sleep을 각각 확인한다. |
| W303 | `test_helper_ownership.cpp:552-650`의 기존 B02 범위를 이름에 명시하고 순서를 “B가 MORE open → A 종료/join → B FINAL이 A의 만료 prefix 회수”로 고쳤다. Callback은 FINAL 전 0회이며 FINAL 뒤 같은 socket의 SNDHWM setter에 재진입한다. |
| S301 | `socket_message_send_api.cpp:496-510`에서 helper lock 아래 record detach가 끝난 직후 `staging_scope.reset()`으로 complete scope에 인계한다. Prepare/append/detach 실패 동안은 기존 admission을 유지하므로 B202 close 보호는 그대로다. |
| S302 | `socket_request_reply_submit_api.cpp:808-838`의 buffered helper에서 호출 계약상 도달할 수 없는 `part_flag == MORE` 분기와 인자를 제거하고 staging admission 보유 invariant를 assert한다. |

MP-9 production 변경 파일은 `socket_request_reply_dispatch.cpp`,
`socket_request_reply_internal.hpp`, `socket_request_reply_submit_api.cpp`,
`socket_message_send_api.cpp`, `socket_base.hpp`, `socket_base_api.cpp`,
`socket_base_lifecycle.cpp`, `socket_send_submit.cpp`다. Test 변경은
`unittest_phase3_request_reply_owners.cpp`, `test_helper_ownership.cpp`와 drain scope signature를
따른 `unittest_single_lane_accounting.cpp`, `unittest_ws_metadata.cpp`,
`unittest_zmp_engine_controls.cpp`다.

## 설계 비교

1. Recursive physical lock, callback 억제, timeout 증가 또는 무등록 async-only 복구는 user
callback의 계약을 바꾸거나 B204의 direct progress를 되돌리고 별도 규칙을 만든다.
2. 선택한 방식은 기존 completion drain owner가 폐기 ownership만 로컬로 detach하고 한 owner
turn을 양보한 뒤 잠금 밖에서 닫는다. Poller/NONE/async/send 모두 동일한 정리 함수와 동일한
owner gate를 유지하며 public API·retry·timeout·두 번째 poller가 필요 없다.

S301도 staging admission을 complete scope와 중첩 유지하는 방식 대신 helper record가 local
owner로 분리되는 단일 인계 지점을 사용했다. 수정 전/후 규칙 수는 **잠금 안 폐기 2개 +
admission 중첩 1개 + dead MORE 1개 = 4개 예외 → deferred discard 1개 + admission handoff
1개 = 2개 owner 규칙**이다.

## 검증

| 검증 | 결과 |
|---|---:|
| `JOBS=4 scripts/build-core.sh dev` | PASS |
| dev 전체 `ctest -E hotpath_gate` 1회 | **209/209 PASS**, 244.34초 |
| 관련 정규식 97 target `until-fail:3` | **291/291 PASS**, 446.24초 |
| 신규·변경 직접 12 case `until-fail:10` | **120/120 PASS**, 166.95초 |
| single-lane 확장 29 case `until-fail:10` | **290/290 PASS**, 191.43초 |
| lost-wake 두 case 각각 `until-fail:20` | **40/40 PASS** |
| ASan+LSan 관련 12 target | **12/12 PASS**, leak/sanitizer 오류 0 |
| GCC TSan 관련 12 target | **12/12 PASS**, race 오류 0 |
| Release+LTO shared lib·static `hotpath_bench` | PASS |
| `git diff --check` | PASS |
| `git diff --stat -- core/include core/src/libzlink.vers` | 출력 없음 |

ASan은 `detect_leaks=1:halt_on_error=1`로 실행했다. TSan은 MP-3 구성
`-fsanitize=thread -fno-omit-frame-pointer -fPIE`, `setarch x86_64 -R`, 기존
`/tmp/mp2-tsan.supp`를 사용했다. 관련 12개는 public multipart, helper
ownership/interleave, phase3 request/reply, REQUEST·SEND WRITABLE 이전, single-lane,
complete admission, request/reply owner, WS metadata, ZMP engine/edge다.
신규·변경 반복은 CMake의 single-lane executable이 펼치는 29개 개별 case까지 합쳐
**41 case, 410/410 PASS**다.

## Hotpath 5셀

Release+LTO lib와 runner를 최종 source로 갱신하고 다른 ninja가 없는 상태에서 실행했다.
측정 시작 load average는 **0.90 / 1.75 / 1.25**였으며 전체 실행을 지정 `PERF_LOCK`으로
직렬화했다.

| cell | MP-8 Ir/msg | MP-9 Ir/msg | MP-8 대비 | ±1% |
|---|---:|---:|---:|---:|
| `dealer_dealer_inproc` | 3,249.917 | 3,250.449 | +0.016% | PASS |
| `dealer_router_reqrep_inproc` | 16,241.043 | 16,347.604 | +0.656% | PASS |
| `pair_inproc` | 2,325.990 | 2,326.223 | +0.010% | PASS |
| `router_router_tcp` | 2,909.839 | 2,929.929 | +0.690% | PASS |
| `stream_tcp` | 14,036.155 | 14,034.684 | −0.010% | PASS |

기존 reference gate는 reqrep만 ratio 0.8759로 FAIL했다. 이는 성능 하한보다 12.41% 낮아진
의도된 개선이며, 나머지 네 셀은 공식 reference도 PASS다. Reference는 수정하지 않았다.

## W302 Callgrind 함수별 대조

MP-7은 clean detached HEAD에 보존 `mp7-before-mp8.patch`
(SHA-256 `96f4c07e0e38811a1e440649f7f7c9148f54fad62e632019cbb0c92cefa74221`)를
적용한 별도 Release+LTO build다. 최종 build와 같은 runner·5,000 iterations·Callgrind 조건으로
`PERF_LOCK` 아래 실행했다. 표는 두 profile의 inclusive Ir가 큰 함수 합집합 상위 20이며,
self/inclusive는 message당 값, calls는 수집 구간의 총 호출 수다. Root/thread entry는 incoming
call edge가 없어 “—”로 표시했다. Inclusive 비용은 중첩되므로 합산하지 않는다.

| # | 함수 | MP-7 self | MP-7 incl | MP-7 calls | 최종 self | 최종 incl | 최종 calls | Δ incl |
|---:|---|---:|---:|---:|---:|---:|---:|---:|
| 1 | `bench::run_request_reply` | 71.002 | 11,837.423 | — | 71.002 | 16,295.146 | — | +4,457.724 |
| 2 | `request_reply_once` | 371.828 | 8,390.350 | 5,000 | 344.000 | 12,214.794 | 5,000 | +3,824.444 |
| 3 | `asio_poller_t::loop` | 218.547 | 6,605.731 | — | 5.726 | 57.367 | — | −6,548.364 |
| 4 | `asio::scheduled_mailbox_op` | 45.963 | 5,859.566 | 4,996 | 0.644 | 38.907 | 70 | −5,820.660 |
| 5 | `socket_base_t::async_mailbox_handler` | 198.856 | 5,813.603 | 4,996 | 0.336 | 38.263 | 70 | −5,775.340 |
| 6 | `socket_base_t::get_events` | 0.000 | 0.000 | 0 | 90.000 | 4,359.019 | 5,000 | +4,359.019 |
| 7 | `router_recv_part_impl` | 177.000 | 4,205.826 | 5,000 | 177.000 | 4,197.040 | 5,000 | −8.786 |
| 8 | `recv_router_message_direct` | 537.000 | 3,975.826 | 5,000 | 537.000 | 3,967.040 | 5,000 | −8.786 |
| 9 | `request_part_common` | 316.000 | 3,331.071 | 5,000 | 316.000 | 3,964.351 | 5,000 | +633.280 |
| 10 | `submit_pull_blocking_request` | 158.000 | 2,462.071 | 5,000 | 159.000 | 3,144.351 | 5,000 | +682.280 |
| 11 | `socket_base_t::recv_routed` | 179.000 | 3,131.826 | 5,000 | 179.000 | 3,105.040 | 5,000 | −26.786 |
| 12 | `public_router_reply_submit` | 282.000 | 3,033.564 | 5,000 | 284.000 | 2,780.558 | 5,000 | −253.007 |
| 13 | `try_request_admission_submit_fast` | 109.000 | 2,040.071 | 5,000 | 109.000 | 2,721.351 | 5,000 | +681.280 |
| 14 | `process_ready_completion_pipes` | 147.900 | 2,577.958 | 9,993 | 103.116 | 2,520.453 | 5,090 | −57.505 |
| 15 | `process_commands` | 635.513 | 2,313.276 | 19,971 | 623.167 | 2,267.724 | 20,078 | −45.552 |
| 16 | `drain_claimed_completion_pipe` | 55.966 | 2,202.214 | 4,997 | 58.000 | 2,286.555 | 5,000 | +84.341 |
| 17 | `drain_claimed...::lambda` | 59.964 | 2,047.305 | 4,997 | 65.000 | 2,129.555 | 5,000 | +82.250 |
| 18 | `pthread_mutex_lock` | 1,964.894 | 1,965.244 | 255,125 | 1,643.498 | 1,643.516 | 215,948 | −321.728 |
| 19 | `process_completion_pipe` | 132.964 | 1,885.402 | 4,997 | 137.000 | 1,962.555 | 5,000 | +77.153 |
| 20 | `send_public_router_reply_with_wait` | 269.000 | 1,642.564 | 5,000 | 189.000 | 1,312.558 | 5,000 | −330.007 |

### 감소 귀속

Function self 비용은 서로 배타적이므로 다음 subtotal은 합산할 수 있다.

| 귀속 | 확인된 self Δ Ir/msg | 근거 |
|---|---:|---|
| async post/dispatch·queue wait·그 동기화 제거 | **−1,848.611** | mutex lock/unlock −587.182, ASIO loop/handler −411.342, cond wait/signal/broadcast −516.223, scheduler/post/op/owner 보조 −333.864. Async handler 호출이 4,996→70, `pthread_cond_clockwait`가 4,980→0, mutex lock이 255,125→215,948이다. |
| 새 direct drain·copy·관찰 경로 | **+624.018** | `get_events` 0→5,000과 +90.000, staged-frame shallow copy/close/consume, public API sync, metadata read, direct completion drain의 식별 가능한 self 증감을 합한 값이다. 감소 원인이 아니라 절감의 약 29.8%를 상쇄한다. |
| 나머지 syscall·allocator·clock·LTO attribution 이동 | **−866.047** | raw libc/condition 내부, allocator, clock, reply submit 내부와 6 Ir/msg 미만 함수들을 포함한 정확한 잔차다. |
| 합계 | **−2,090.640** | 92,215,768 → 81,762,566 Ir / 5,000 |

따라서 감소는 새 direct drain이 일을 생략해서가 아니라, 같은 drain을 caller turn에서 실행해
거의 매 message 발생하던 async executor와 condition wait 왕복을 없앤 결과다.

정상 왕복 작업이 collection 밖으로 이동한 횟수는 **0회**다. Benchmark는 수집 구간 안에서
`request_reply_once`가 completion receive와 kind·ID·result·part count 검증까지 끝나야 다음
iteration으로 간다(`hotpath_bench.cpp:603-643,713-725`). 두 profile 모두 다음 호출 수를
유지한다.

| 보존 작업 | MP-7 calls | 최종 calls |
|---|---:|---:|
| 전체 request/reply/completion 왕복 | 5,000 | 5,000 |
| pending erase / `complete_reply_from_transport` | 5,000 / 5,000 | 5,000 / 5,000 |
| request correlation release | 5,000 | 5,000 |
| reply token checkout / commit erase / slot release | 5,000 / 5,000 / 5,000 | 5,000 / 5,000 / 5,000 |
| transport commit 검사 | 10,000 | 10,000 |

최종 `process_completion_pipe`와 `complete_reply_from_transport`도 정확히 5,000회다. MP-7의
`process_completion_pipe` edge가 4,997인 것은 LTO/inlining 및 async thread root attribution
차이이며, downstream `complete_reply_from_transport` 5,000회로 모든 record 처리를 확인했다.
Reply token commit 함수는 LTO로 inline됐지만 `checkout_router_reply_target`,
`erase_router_reply_target_locked`, publish guard 종료와 slot release가 양쪽 모두 5,000회라
checkout된 token의 정상 commit·회수도 보존됐다.
최종의 `consume_send_frame`은 5,000→15,000회로 늘어 shallow-copy attempt와 원본의 소비
책임도 생략되지 않았다.

`collect_scope_t` 뒤에 있는 caller-owned `zlink_completion_close`와 message cleanup
(`hotpath_bench.cpp:727-731`)은 두 버전 모두 측정 밖이다. 새로 collection 밖으로 옮긴
completion/token/correlation/slot 작업은 없으므로 이 경계는 감소 귀속이 아니다.

Callgrind 원문과 annotate 결과는 scratchpad의 다음 파일에 보존했다.

- `mp9-mp7-reqrep.callgrind`, `mp9-final-reqrep.callgrind`
- `mp9-mp7-reqrep-self.txt`, `mp9-mp7-reqrep-inclusive.txt`
- `mp9-final-reqrep-self.txt`, `mp9-final-reqrep-inclusive.txt`
- `mp9-reqrep-top20.md`

## 계약·계층 판정

- **소유 계층:** Core completion drain이 physical REPLY record의 pending lookup, public
completion export와 폐기 수명을 소유한다. Helper final admission 인계는 Core part-helper/public
send 경계가 소유한다.
- **Spec 조항:** main 미커밋
`core/doc/spec/core/socket/README.ko.md:1151-1209`의 completion pull·단일 직렬 drain과
`core/doc/spec/core/02-message.ko.md:41-57,458-470`의 정확히 한 번 close·zero-copy callback
수명이다. Payload export OOM은 README `:1190-1195`, late reply 단일 completion은 ZMP
`01-zmp.ko.md:426-433,579-585`와 일치한다. 어느 문장도 다른 동작이 되지 않았다.
- **교차언어 대조:** Framework runtime 변경은 없다. C++, .NET, Java, Node.js, Python, Go,
Rust가 모두 동일한 Core C `zlink_completion_recv` 경로를 호출하므로 언어별 completion
정책이나 보상 상태를 추가하지 않았다.
- **변경 분류:** **B — 기존 결함.** MP-8이 기존 poller drain을 무등록 NONE caller turn으로
확대한 뒤 드러난 잠금 안 payload 해제를 소유 Core 계층에서 수정했다.

남은 기능·sanitizer 실패는 없다. 공식 hotpath reference의 reqrep 하한만 의도된 개선 때문에
FAIL이며 D-B213 지시대로 reference는 갱신하지 않았다.
