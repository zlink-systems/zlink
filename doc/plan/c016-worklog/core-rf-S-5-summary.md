# S-5 — blocking submit(FLAGS_NONE) 진입 비용 슬림화 — **채택 없음(측정으로 가설 반박)**

> 2026-09-07, worktree `~/project/zlink-work/s5` (detached `14396b53f4`). **diff 없음(전부 되돌림).**
> 원본 데이터: `<scratchpad>/S5/` (`cg_before.out`, `cg_after.out`, `cg_after2.out`, `ann_before.txt`, `b.py`, `edges.py`, `cg.sh`, `load_*.txt`)
> 측정: S-A §0 축소 셀(1024 B, CCU 20, warmup 2 s, duration 15 s, callgrind `--cache-sim=no`), worktree Release lib + worktree `bindings/c/build` 벤치. 전부 `flock PERF_LOCK` 아래. load 0.4~1.2.

## 1. 결과 요약

브리프의 두 가지 목표는 **이미 코드에 실현되어 있었고**, 강제로 인라인해서 얻는 이득은 **측정 잡음(±1 %) 이하**였다. 그래서 변경을 채택하지 않는다.

| 목표 | 판정 | 근거 |
|---|---|---|
| (a) `enter_public_send`의 CAS + sync 획득을 단일 상태 전이로 | **이미 그렇다** | `take_sync_in_admission`(`socket_lifecycle_runtime.cpp:133-137`)이 정상 상태에서 항상 성립 → CAS 1회로 in-flight+1, complete_unit, sync bit를 동시에 세운다. 실측: `lock_public_api_sync` 호출 **0.512/msg**(send 경로가 아니라 recv/poller 쪽), send admission은 1.000/msg인데 lock 호출이 없다 |
| (b) wait guard/컨텍스트 생성을 첫 admission 실패 이후로 지연 | **지연할 것이 없다** | `blocking_send_wait_guard_t` 생성자는 포인터 4개 store, `bind_target`은 store 1개, 소멸자는 `_acquired==false`면 즉시 return, `failure_errno()`도 즉시 0. `completion_submit_wait_context_t`는 `sndtimeo<0`이면 clock 호출조차 없고 `transport_pair_owner_progress_scope_t`는 store 2개. **할당·잠금·syscall 0** |
| (b') 첫 시도를 루프 밖 fast path로 분리 | **계약상 불가** | 실패 시 wait 등록은 **admission을 거절한 그 lifecycle sync 안에서** 이뤄져야 한다(`socket_send_submit.cpp:424-427` 주석). sync를 놓았다 다시 잡으면 그 사이 발행된 terminal handoff의 epoch를 guard가 놓친다(`blocking_send_wait_guard_t::acquire`가 epoch를 취득 시점에 캡처) |

## 2. 실측 — blocking submit 진입 경로 비용 분해 (1024 B, before)

전체 **10,117 Ir/msg**, `pthread_mutex_lock` **19.17회/msg**.

| 프레임 | self Ir/msg | 비고 |
|---|---|---|
| `wait_for_completion_submit_admission` | 144 (1.42 %) | scope 생성자 인라인 포함 |
| `try_admit_send_parts_scoped` | 127 (1.26 %) | |
| `send_direct_with_retry` | 113 (1.12 %) | +`msg_t::check` 9, `clear_send_recovery_pending` 4 |
| `enter_public_send` | 56 (0.47 %) | CAS 1회 |
| `~socket_public_send_scope_t` | 21 | +`leave_public_send` 47 |
| `process_submit_commands` | 16 | |
| **순수 계층 비용 합계** | **≈ 537 (5.3 %)** | |
| `process_commands`(0.044회/msg) | 222 | 실제 command drain — 계층 비용 아님 |
| `xsend_routed`(inclusive) | 889 | 실제 pipe write — 계층 비용 아님 |
| **submit 진입 inclusive 합계** | **1,648 (16.3 %)** | |

즉 **격차의 본체는 wait 스캐폴딩이 아니라 5중 프레임의 인자 마샬링·재검증**이다.

## 3. 시도한 변경과 그 결과 (되돌림)

`socket_runtime.hpp`로 lifecycle word 비트 상수를 올리고 (1) `enter_public_send`에 단일 CAS fast path 인라인, (2) `leave_public_send`·`mark/unmark_public_api_sync_owned`·`public_api_sync_owned_by_current_thread`를 헤더 인라인, (3) `~socket_public_send_scope_t`의 complete-record 분기 인라인, (4) `admission_errno = errno`를 실패 분기로 이동.

| 셀 | Ir/msg | lock/msg | submit 진입 inclusive Ir/msg |
|---|---|---|---|
| before (`14396b53f4`) | **10,117** | 19.171 | 1,648 |
| after (1)~(4) 전부 | 10,101 (−0.16 %) | 19.194 | 1,624 (−24) |
| after2 (1) 되돌리고 (2)~(4)만 | **10,216 (+0.98 %)** | 19.435 | 1,644 (−4) |

- 3회 런의 총 Ir/msg가 10,101 / 10,117 / 10,216으로 **±1 % 흔들린다**(메시지 수·epoll/read 횟수가 런마다 다르다). 목표했던 효과(≤0.3 %)가 잡음 아래다.
- after2에서 `leave_public_send` self가 47 → 56 Ir/msg로 **오히려 늘었다**. LTO가 이미 최적 배치를 고르고 있어서 수동 인라인이 역효과를 냈다.
- (1)은 admission 규칙(닫힘·multipart·카운터 포화 판정)을 헤더에 **복제**한다. 0.26 %를 위해 규칙을 이중화하는 것은 POSDDD(중복 금지)에 반한다 → 채택 불가.

## 4. `std::recursive_mutex _api_mutex` (review-S-2 지적) — **필요하다, 제거 불가**

- 이 잠금은 `socket_lifecycle_coordinator_t`가 아니라 **`stream_t`**의 것이고 획득 지점은 두 곳뿐이다: `stream.cpp:831`(`xsend`), `stream.cpp:245`(`xpipe_terminated`). `api_sync_mutex()`(`stream.cpp:1223`)는 `zlink_close` 경로에서만 쓰인다(`zlink.cpp:148`, `stream_api_lock_t`).
- **메시지당 잠금이 아니다**: `zlink_send_part_rid` → `xsend_routed`는 이 잠금을 잡지 않는다. 실측에서 `stream_t::xsend` 심볼 **호출 0**, `api_sync_mutex()` 전체 실행에서 **2회**. 벤치·perf 셀의 핫 경로에 없다. `_more_out`/`_current_out` 2-part 시퀀스를 쓰는 `zlink_send_part` 사용자에게만 메시지당 1쌍이다.
- **lease/CAS로 대체 불가**: 공개 send는 `public_api_sync_bit`(needs_sync=true)로 서로 배제되지만, `xpipe_terminated`는 **async executor(io 스레드)의 `process_async_mailbox`**에서 온다(`socket_base_lifecycle.cpp:315-323, 877-911`). 이 경로는 public API sync bit를 잡지 않는다. 따라서 `_current_out`/`_more_out`/route 제거를 공개 `xsend`와 배제하는 것은 오직 `_api_mutex`뿐이다(04-thread-safety). 제거하면 데이터 경쟁이다.
- 재귀성 자체는 필요하다: `zlink.cpp:155` 주석대로 async pipe termination이 같은 잠금에 재진입한다.

## 5. WRITABLE token 계약 재확인 — 어느 문장도 다른 동작이 되지 않았다

변경을 되돌렸으므로 자명하지만, 검토한 문장을 남긴다.

08-stream.en.md 122-135행:
> `NONE FINAL` snapshots `SNDTIMEO` and waits for local queue admission and reconnect of the same RID. A `DONTWAIT FINAL` makes exactly one admission attempt. If admitted immediately, it has ID `0` and no completion. If HWM or byte credit prevents admission, … it returns `ZLINK_SUBMIT_BACKPRESSURED` with `EAGAIN` and a nonzero wait token bound to that RID … When the same RID gains write credit …, Core produces exactly one `ZLINK_COMPLETION_WRITABLE` record for that token …

README.en.md "Part send and completion"(1527-1535행):
> A DONTWAIT `FINAL` makes one admission attempt. Immediate admission returns ID `0` and no completion. Backpressure or a target that is not ready yet returns `ZLINK_SUBMIT_BACKPRESSURED` with `EAGAIN` and a nonzero wait token … A NONE `FINAL` waits for admission to the same logical target within the snapshotted `SNDTIMEO` and returns ID `0` with no completion.

왜 불변인가: 이 job이 만진 대상은 (i) admission이 성공했을 때만 실행되는 상태 워드 갱신, (ii) `admission_rc != 0`에서만 읽히는 `errno` 캡처 시점뿐이었다. **token은 `FLAGS_NONE` 경로에서 애초에 만들어지지 않고**(NONE FINAL은 ID 0, completion 없음 — 위 두 문장), token 생성은 DONTWAIT 실패 시 `register_send_writable_wait_after_failure`(`socket_message_send_api.cpp:388-392`)에서만 일어난다. 이 job은 그 함수도, `blocking_send_wait_guard_t::acquire`의 등록 시점(=거절을 관측한 lifecycle sync 안)도 건드리지 않았다. 그리고 최종 상태는 diff 0이다.

## 6. 실행한 테스트

worktree diff가 0이므로 회귀 대상이 없다. Release lib 빌드 3회(baseline·after·after2)와 dev 트리 빌드 1회 모두 **경고·에러 없이 성공**했다(중간 변경본도 컴파일·링크·15 s 실부하 에코 통과: `cg_after*_server.log`의 `parse_error=0 protocol_error=0 send_error=0`). ctest 5회·TSan·with_stream·perf/c 셀은 **돌리지 않았다** — 채택할 변경이 없어 측정할 대상이 없다.

## 7. 변경 분류

**C(우회 아님) / 해당 없음 — 채택 없음.** S-B §4의 S-5 예상 이득 "2–5 %"는 실측으로 **반박**된다(≤0.3 %, 잡음 이하).

## 8. 후속 후보 (S-B에 없던 것)

**S-12(제안): routed 단일 part submit 진입 계층 축약.** `submit_public_send_record` → `send_completion_submit_blocking` → `wait_for_completion_submit_admission` → `try_admit_send_parts_scoped` → `send_direct_with_retry` → `xsend_routed`의 6프레임 중 가운데 3개는 STREAM/ROUTER 단일 part에서 인자만 옮긴다(14·19개 인자). 순수 계층 비용 **537 Ir/msg(5.3 %)**. 이득은 fast path 추가가 아니라 **프레임 병합**에서만 나온다 — 규모가 커서 별도 job이 필요하고, 계약 위험은 "실패 시 wait 등록은 거절을 관측한 lifecycle sync 안"이라는 불변식 하나에 집중된다.

## 9. 멈춘 지점

없음. 가설이 반박되어 조기 종료(약 1 h 20 m).
