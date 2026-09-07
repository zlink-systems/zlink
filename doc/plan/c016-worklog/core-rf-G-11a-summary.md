# G-11a — 명령 드레인이 항상 소켓 turn을 잡는다 (lane 모델 step 1)

> 2026-09-07. worktree `~/project/zlink-work/g11a` (detached `08da256f1e`). **커밋하지 않았다.**
> 원본 로그: `<scratchpad>/G11a/`(`hotpath-{before,after}.log`, `tsan-*.log`, `g11a.patch`, `hp_{before,after}`).
> G-1(`socket_runtime.hpp`·`socket_base_dispatch.cpp`·`ctx_physical_queue_registry.cpp`)과
> G-3(`pipe.hpp`/`pipe.cpp`)은 **한 줄도 건드리지 않았다**(diff 2파일).

## 0. 결론 한 줄

PAIR 예외는 **성능 사유로 도입됐고 지금은 성립하지 않는다**(측정으로 반박). 예외를 없애 명령 드레인 규칙이
2개(비-PAIR는 잡고, PAIR는 bind/pipe_term_ack만 probe 후 재시도)에서 **1개**("명령 배치는 항상 turn 아래에서 돈다")로 줄었고,
비용은 **PAIR 20만 메시지당 bit 획득 43 → 176회, 경합 0회**, pair_inproc Ir/msg **+0.02 %**(노이즈), 벽시계 차이 없음이다.

## 1. 왜 PAIR가 예외였고, 그 이유가 지금도 성립하는가 — **아니다**

도입 커밋 `d548675abe`(2026-08-27, "perf(core): restore hot-path throughput with safe lifecycle sync")의 주석이 근거를 그대로 적고 있다.

> "PAIR owns only one raw pipe pointer: bind publishes it and a termination acknowledgement clears/deallocates it.
> Guard those two mutation commands unconditionally rather than checking a multipart marker, which would leave a
> check-then-admit race. **The frequent PAIR activation/pending commands remain lock-free.**"

즉 예외의 근거는 두 가지였다.

- **(a) 정확성 근거** — PAIR socket 쪽 endpoint의 *raw pipe 포인터*를 `bind`/`pipe_term_ack`가 바꾸는 동안
  `xsend`가 그 포인터를 역참조하면 안 된다. 이 근거는 **예외가 아니라 fence를 요구한다**. 그래서 원본도 이 두 명령은
  turn을 잡았고(`is_pair_pipe_lifetime_command` probe → API→command-owner 순서로 재시도), 예외 대상은 나머지 명령뿐이었다.
  이 요구는 "항상 잡는다"에 **완전히 포함된다** — 강화이지 약화가 아니다.
- **(b) 성능 근거** — "frequent PAIR activation/pending commands remain lock-free". 후속 `8b6c2aa906`(2026-09-03,
  "cut PAIR per-message overhead")도 같은 방향이다. **이 근거가 유일한 실질 예외 사유이며, §3의 측정이 이것을 반박한다.**

측정으로 본 (b)의 실체(§3): PAIR inproc 20만 메시지 동안 `lock_public_api_sync` 호출은 **43 → 176회**(메시지당
0.00022 → 0.00088), 경합(CAS 실패 후 백오프 진입) **0 → 0회**다. "frequent"라는 전제 자체가 사실이 아니었다 —
PAIR의 명령 드레인은 거의 전부 **이미 turn을 소유한 공개 호출 스레드 안에서** 일어나므로
(`public_api_sync_owned_by_current_thread()`가 true → 획득 자체가 없다) 추가 CAS가 발생하지 않는다.
따라서 "예외를 유지할 이유가 지금도 성립하는가"의 답은 **아니오**이며, 여기서 멈추지 않았다.

### 1.1 reaper-close 예외

`reaper_owns_closed_socket`(공개 close 이후 reaper poller가 설치된 뒤) 예외의 주석 근거는
"A parked marker may still leave a stale raw bit, so the reaper must not wait on it"이다. 코드로 확인한 결과
raw bit를 콜 밖으로 들고 나가는 경로는 없다 — `suspend_public_multipart_send`(`socket_lifecycle_runtime.cpp:479`)와
`release_public_multipart_marker`(`:242-258`)가 파킹·해제 시 bit를 함께 내려놓고,
`socket_public_send_scope_t::release_sync_for_retry`(`:511`)가 블로킹 대기 전에 내려놓는다.
`start_reaping()` 시점에는 async quiesce가 끝나 있으므로(`socket_base_lifecycle.cpp:340-341`) 명령 owner도 bit를 들고 있지 않다.
`test_close_completion_poller_release` ×50과 close·release suite 5회로 reaper가 bit에서 막히지 않음을 확인했다(§4).

## 2. 변경 내용

`core/src/runtime/sockets/common/socket_base_lifecycle.cpp` (−58 줄):

- `process_commands`의 sync 결정이 **한 줄**이 됐다.
  `acquire_public_api_sync = !lifecycle.public_api_sync_owned_by_current_thread ()` — 소켓 타입도, 명령 종류도,
  close/reaper 상태도 더는 보지 않는다. `reaper_owns_closed_socket`·`command_path_needs_public_api_sync`·
  `pair_sync_for_this_claim` 세 지역 상수가 사라졌다.
- PAIR pipe-lifetime **probe/재시도 루프 전체 삭제**: `is_pair_pipe_lifetime_command()` helper,
  `probe_pair_lifetime_command`, `mailbox->probe_command(...)` 호출, `retry_pair_pipe_lifetime_with_sync`,
  `pair_pipe_lifetime_sync_required`, 그리고 그 재시도를 위해 바깥 `while(true)`로 되돌아가던 `continue` 분기.
  드레인이 이미 turn 아래에 있으므로 "먼저 훔쳐본 뒤 순서를 바꿔 다시 잡는다"는 프로토콜의 존재 이유가 없다.
- throttle 게이팅 두 곳의 `!pair_sync_for_this_claim &&` 접두 조건 제거(재시도 클레임이 없어져 항상 참).

`core/tests/unittest/unittest_receive_transaction.cpp` (내부 정책 unittest, 공개 계약 테스트 아님):

- `test_pair_commands_only_fence_pipe_lifetime_transitions` → `test_pair_commands_fence_every_command_drain` 개명.
- "PAIR activation command unnecessarily acquired the multipart fence"(FALSE 기대) →
  "PAIR activation command did not take the socket turn"(TRUE 기대). **이 테스트가 예외를 못박고 있던 유일한 지점이다.**
- 같은 테스트의 나머지 단언은 그대로 통과한다: xsend가 게이트를 쥐고 있는 동안 replacement `bind`가
  처리되지 않고, 송신이 끝난 뒤 turn을 소유한 채 처리된다(fence 요구 (a)가 그대로 유지됨을 증명).

`mailbox_t::probe_command`/`ypipe_t::probe`는 이제 호출자가 없다. 삭제는 `mailbox.*`를 만지게 되므로
게이트 중인 다른 job과의 충돌을 피해 **이 job에서는 남겨 뒀다**(후속 정리 항목).

## 3. 측정 — bit 경합과 PAIR 지연

`lock_public_api_sync`에 임시 카운터(calls / contended / spins / yields / sleeps)를 넣어 before·after를 같은
워크트리에서 재고 **제거했다**(최종 diff에 없다). `hotpath_bench <cell> 200000`, flock 아래.
`enter_public_send`의 admission CAS로 bit를 함께 가져가는 경우는 잠금 경로를 타지 않으므로 카운트 밖이다 —
즉 아래 숫자는 **"turn을 따로 잡아야 했던 횟수"**다.

| 셀 (200,000 msg) | before calls | after calls | before 경합 | after 경합 | after 백오프 |
|---|---:|---:|---:|---:|---|
| **pair_inproc** | 43 | **176** | 0 | **0** | spin 0 / yield 0 / sleep 0 |
| stream_tcp | 398,284 | 398,086 | 0 | 0 | — |
| router_router_tcp | 398 | 403 | 2 | 1 | spin 64 / yield 26 / sleep 0 |
| dealer_dealer_inproc | 274 | 277 | 0 | 0 | — |

- PAIR: +133회 / 200,000 msg = **+0.0007 CAS/msg**, 경합 0. 비-PAIR 3셀은 이미 turn을 잡고 있었으므로 불변(차이는 노이즈).
- **sleep(100 µs) 진입 0회** — 스핀락을 쥔 채 명령을 적용해도 어떤 셀도 백오프의 수면 단계에 닿지 않는다.

**PAIR activation 지연**(A.5가 지정한 유일한 판정 기준): 정적 링크된 before/after 바이너리를 교차 실행(각 6회, 400,000 msg, flock).

| | 1 | 2 | 3 | 4 | 5 | 6 | 중앙값 |
|---|---:|---:|---:|---:|---:|---:|---:|
| before (s) | 0.331 | 0.324 | 0.350 | 0.336 | 0.341 | 0.325 | 0.3335 |
| after (s) | 0.330 | 0.344 | 0.315 | 0.328 | 0.333 | 0.351 | 0.3315 |

차이 없음(load average 2.7~3.5의 비-idle 머신, 분산 안). 결정적 지표인 Ir/msg는 아래 §3.1에서 +0.02 %다.
`test_wake_invariants`·`test_wake_invariant_*`·`test_two_poller_wake`·`test_stream_send_blocking_wakeup`
until-fail:20 전부 통과 — 활성화 wake가 늦거나 유실되지 않는다.

### 3.1 hotpath 5셀 (callgrind Ir/msg, 결정적, 주 판정)

같은 워크트리에서 pristine `08da256f1e` dev 빌드를 before로 삼았다(1 TU 증분 빌드).

| 셀 | before Ir/msg | after Ir/msg | 변화 | ratio(before→after) |
|---|---:|---:|---:|---|
| `dealer_dealer_inproc` | 4,241.10 | 4,240.76 | −0.3 (−0.01 %) | 1.2388 → 1.2387 |
| `dealer_router_reqrep_inproc` | 23,345.57 | 23,259.21 | **−86.4 (−0.37 %)** | 1.1861 → 1.1817 |
| **`pair_inproc`** (주 관찰 셀) | 3,082.05 | 3,082.73 | +0.7 (**+0.02 %**) | 1.3124 → 1.3127 |
| `router_router_tcp` | 3,636.39 | 3,654.47 | +18.1 (+0.50 %) | 1.2233 → 1.2294 |
| `stream_tcp` | 16,096.35 | 15,840.09 | **−256.3 (−1.59 %)** | 1.1007 → 1.0832 |

5셀 중 2셀 개선, 1셀 무변화, 2셀 +0.02~+0.50 %. 예상 밖의 개선(`stream_tcp` −1.59 %)은 잠금과 무관하다 —
드레인마다 돌던 `public_close_requested()`·`reaper_poller()` 두 원자 로드와 PAIR probe 분기가 사라졌기 때문이다.
`hotpath_gate` 자체는 before·after 모두 FAIL이며 이는 reference가 Release/LTO 기준이어서 생기는 **기존 실패**다
(G-2 §1.1·G-3 §1이 pristine에서 같은 FAIL을 확인했다).

`_out_sync`·`receive.sync`는 **하나도 제거하지 않았으므로** 축소셀 `pthread_mutex_lock`/msg는 15.12에서 변하지 않는다
(이 job의 목적은 step 2가 기댈 "turn이 배타를 준다"는 전제를 확정하는 것이다). 잠금 제거 효과 측정은 G-11b~d 소관이다.

## 4. 실행한 테스트

| 항목 | 결과 |
|---|---|
| `ctest -R 'pair\|wake\|poll\|stream\|pipe\|mailbox\|send\|recv\|receive\|router\|dealer\|close\|release\|transaction'` ×5 | **63/63 × 5 PASS** (560 s) |
| lost-wake `until-fail:20` (`test_wake_invariants`, `test_wake_invariant_hwm_lwm_shrink`, `test_wake_invariant_completion_owner`, `test_two_poller_wake`, `test_stream_send_blocking_wakeup`) | **5/5 × 20 PASS** (654 s) |
| `test_close_completion_poller_release` `until-fail:50` | **PASS** (31 s) |
| TSan (`setarch -R`, Release+`-fsanitize=thread`, 별도 `core/build-tsan`) | `unittest_receive_transaction` 0건, `test_close_completion_poller_release` 0건, `test_wake_invariants` **before 10건 = after 10건**(9× `ypipe_t<command_t>::check_read`, 1× `receive_once_guarded`) — **신규 0**, 모든 테스트 PASS |
| 남은 실패 | 없음(위 `hotpath_gate`의 기존 FAIL 제외) |

전체 ctest는 감독관 게이트 job 소관이라 돌리지 않았다.

## 5. 설계 비교와 선택 이유

- **선택안 — 조건을 없애고 "항상 잡는다"로 만든다.** 규칙 2개 → 1개, 지역 상수 3개·helper 1개·probe API 호출·
  재시도 루프 삭제. POSDDD의 "제어점을 추가하지 말고 합치거나 없애라"에 정확히 맞고, framework 06 §4의
  "컴포넌트를 부분적으로만 lane으로 옮기면 그 경계에서 교차 불변식 위반이 다시 나타난다"가 지목하는 경계 자체를 없앤다.
- **기각안 — PAIR 예외를 좁혀 activate_write/activate_read만 남긴다.** 잠금 카운트는 같고 규칙은 오히려
  "어떤 명령이 예외인가"라는 목록으로 늘어난다. §3이 예외의 이득을 0으로 측정했으므로 유지할 근거가 없다.
- **기각안 — reaper 예외만 남긴다.** §1.1에서 raw bit가 콜 밖으로 새는 경로가 없음을 확인했고, 예외를 남기면
  step 2의 모든 증명이 "close 이후에는 turn이 없다"는 두 번째 경우를 계속 끌고 다녀야 한다.

## 6. 재확인한 스펙 문장 — 어느 문장도 다른 동작이 되지 않았다

- **04-thread-safety §4.1** "어떤 스레드든 소켓을 사용할 수 있고 공개 연산은 직렬화된다" — 공개 연산당 배타는
  `public_api_sync_bit` 하나 그대로다. 이 job은 명령 드레인을 **그 배타 안으로 넣었을 뿐** 배타를 쪼개거나 늘리지 않았다.
- **05-polling** POLLIN level 유지 조건, "WRITABLE wake는 `_out_active` 전이당 정확히 1회" — `_out_active` 전이
  코드도 wake 발신 코드도 건드리지 않았다. 명령이 적용되는 **순서**는 mailbox FIFO 그대로이고, 달라진 것은
  적용 구간이 turn 아래라는 것뿐이다. lost-wake until-fail:20으로 게이트.
- **README recv wake 조건** — recv 경로는 이 diff에 없다(A.3대로 recv는 여전히 turn 밖이다).
- **06-auto-hwm 434-435·467** charge는 frame write에서 시작해 dequeue에서 끝난다 — 회계 코드 무변경, charge 값·시점 모두 불변.
- **D-079·D-099·S-12** — 완료·READY/DISCONNECTED·WRITABLE 순서 규칙에 닿는 코드 없음.
- 블로킹 send는 `wait_for_submit_progress`가 이미 sync를 놓고 기다린다(`socket_base_lifecycle.cpp:736,740`
  `release_sync_for_retry`/`reacquire_sync_after_retry`) — **변경 없음 확인**. 따라서 "명령 드레인이 turn을 요구한다"가
  블로킹 송신자와 데드락을 만들지 않는다.

## 7. 변경 분류

**B (기존 결함)** — 계약이 요구하지 않는 예외가 성능 가설 위에 도입됐고, 그 가설(PAIR 명령 드레인이 frequent하다)이
측정으로 반박됐다. 공개 헤더·`libzlink.vers`·공개 계약 테스트 기대값은 건드리지 않았다.

## 8. 멈춘 지점 / 남긴 것

- **with_stream(release lib) 측정은 하지 않았다** — 1.5 h 상한. 잠금을 하나도 제거하지 않았고 결정적 hotpath 5셀이
  −1.59 %~+0.50 %에 들어오므로 처리량 판정은 게이트 job의 5셀 재측정으로 충분하다고 봤다.
- `mailbox_t::probe_command` / `ypipe_t::probe`가 무호출로 남았다(§2). `mailbox.*`가 다른 게이트와 겹칠 수 있어
  이 job에서 삭제하지 않았다 — 게이트 뒤 정리 항목.
- 다음 단계(A.5 step 2)는 이 job이 확정한 "명령 배치는 항상 turn 아래에서 돈다"를 전제로 2a `receive.sync` →
  2b socket endpoint `_out_sync` → 2c session endpoint `_out_sync` + C3 원자 → 2d route shard 순으로 진행하면 된다.
