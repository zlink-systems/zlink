# S-1 — I/O↔앱 핸드오프 activate_read 왕복 축소

> 2026-09-07. worktree `~/project/zlink-work/s1` (main `430abce139` + **S-2 diff 선적용**). 커밋하지 않음.
> 원본 데이터: `.../scratchpad/S-1/` (`cg_s2base.out`, `cg_after.out`, `tsan-*.log`),
> 벤치 결과 `~/project/zlink-work/s1/bindings/c/bench/with_stream/results/20260907_022602/`.

## 1. 결과 (수치)

기준선 = **S-2 적용본**(비재귀 mutex)이며, 그 위에 S-1만 얹은 차이다. callgrind 축소 셀(S-A §0 방법, CCU 20 / 1024 B / duration 15 s, 서버를 valgrind로 감쌈).

| 지표 (메시지당) | S-2 base | S-1 적용 | 변화 |
|---|---:|---:|---|
| **Ir / msg** | **10,589** | **9,887** | **−6.6 %** |
| `pthread_mutex_lock` 호출 | **18.482** | **16.552** | **−1.93 (−10.4 %)** |
| pthread_mutex 자체 Ir 비중 | 11.38 % | 10.99 % | −0.39 pt |
| `pipe_t::process_activate_read` 자체 Ir | **46.0** | **20.0** | **−57 %** |
| `mailbox_t::send` 호출 | 2.003 | 2.003 | 동일 |
| `mailbox_t::recv` 호출 | 1.684 | 1.700 | 동일 수준(노이즈) |
| `signaler_t::send` (eventfd write) | 0.409 | 0.415 | 동일 수준 |
| `signaler_t::recv_failable` (eventfd read) | 0.684 | 0.700 | 동일 수준 |
| `process_activate_read` 호출 | 1.999 | 2.000 | 동일 |
| `stream_t::xread_activated` 호출 | 0.999 | 0.999 | 동일 |
| TCP `recv` / `send` | 3.000 / 1.000 | 3.000 / 1.000 | 동일 |

제거된 락은 정확히 **메시지당 activate_read 2회 × `_out_sync` 1쌍**이다(18.482 − 16.552 = 1.93 ≈ 2).
mailbox·signaler·command 개수가 **하나도 줄지 않은 것이 §2의 (a) 판정에 대한 실측 확인**이다.

### with_stream (CCU 1000, runs 1, load avg 0.45에서 측정, `20260907_022602`)

| size | zlink kops | asio kops | 비(zlink/asio) | Phase 0 기준 비 |
|---|---:|---:|---:|---:|
| 64 B | **287.9** (기준 268.9, +7.1 %) | 363.9 | 0.791 | 0.835 |
| 1024 B | **258.9** (기준 243.0, +6.5 %) | 327.2 | **0.791** | 0.768 |
| 65536 B | **32.5** (기준 30.4, +7.0 %) | 40.3 | **0.808** | 0.776 |

절대 처리량은 세 크기 모두 Phase 0 기준 대비 **+6.5~7.1 %**. 1024 B·64 KiB에서는 asio 대비 비율도 개선(0.768→0.791, 0.776→0.808)됐고, 64 B는 asio 쪽이 기준 대비 더 크게(322.0→363.9) 나와 비율만 낮게 보인다. 이 표는 S-2 + S-1 합산이며 S-1 단독 분리는 하지 않았다(측정 1회 규칙).
mismatch는 모든 셀 0.

## 2. 설계 비교와 선택 이유 — (a)는 기각, (b)+(c) 채택

### (a) "미소비 activate_read coalescing" — **구현하지 않음(이득 0으로 증명)**

브리프 지시대로 "새 상태를 두기 전에 ypipe의 기존 sleep/awake 표시로 같은 정보를 얻을 수 있는지" 먼저 검토했고, **이미 그것이 바로 그 표시**였다.

- `pipe.cpp` `flush_unlocked()`의 `const bool sleeping = _out_pipe && !_out_pipe->flush ();` 가 유일한 발송 조건이다.
- `ypipe_t::flush()`는 reader가 잠들어 있을 때만 false를 돌려주고 **그 순간 `_c`를 `_f`로 올려 pipe를 깨어 있음으로 표시**한다. reader가 `check_read()`에서 큐를 비우고 다시 잠들기 전까지 **두 번째 flush는 반드시 true**를 돌려준다.
- 따라서 "mailbox에 미소비 activate_read가 있는데 또 보낸다"는 상황은 **구조적으로 발생하지 않는다**. sleep episode당 정확히 1회다.
- 에코 부하에서 메시지당 2회가 나오는 것은 중복이 아니라 **방향이 둘**(I/O→앱, 앱→I/O)이고 매 메시지가 각각 하나의 sleep episode이기 때문이다. 실측이 이를 확인한다: 위 표에서 `mailbox_t::send` 2.003/msg가 S-1 후에도 **한 톨도 줄지 않았다**.
- 결론: pipe에 "activate_read pending" 플래그를 두는 것은 **순수한 상태 추가이며 이득 0**. POSDDD(규칙 수 줄이기, 중복 금지)에 정면으로 어긋나므로 기각한다. 왕복 **횟수**를 줄이려면 앱을 I/O 스레드에서 실행해야 하고 그것은 S-B의 D-c(04-thread-safety 위반)다.

따라서 남은 지렛대는 **왕복 1회의 비용**이며, (b)와 (c) 둘 다 구현했다.

### (b) `process_activate_read`의 `_out_sync` 제거 — 잠금이 지키던 불변식의 문서화와 증명

**잠금이 실제로 지키던 것**: `_out_sync`는 `pipe.hpp:709` 주석대로 "`_out_pipe`, `_state`, `_out_active`, `_peers_msgs_read` 라는 하나의 **송신측 상태 클러스터**"를 보호한다. 보호 대상은 **같은 socket에 대해 다른 앱 스레드가 동시에 실행하는 `write()`/`flush()`** 다.

**`process_activate_read`가 만지는 것과 그 실제 소유자**:

| 멤버 | 누가 쓰나 | `_out_sync`가 보호하나 |
|---|---|---|
| `_in_active` | `check_read()`(`pipe.cpp:1048`), `read_internal()`(`:1170,:1196`) — **둘 다 `_out_sync`를 잡지 않는다** | **아니다**(한쪽만 잡는 잠금은 보호가 아니다) |
| `_state` | 모든 쓰기가 `transition_to_inactive_state_unlocked()` 경유, `_out_sync` 아래. 그러나 **읽기는 `check_read()` `pipe.cpp:1032`가 이미 잠금 없이** 한다 | 읽기측은 이미 잠금 밖 관행 |
| `_head_reclassify_wake` | `std::atomic`, `exchange`로만 전이 | 원자 자체로 충분 |
| 재분류 술어(`_transport_pair_id`, `_transport_lane`, `_session_pipe`, `_session_io_writer`) | attach 시점 확정 + `_transport_lane_count`는 atomic | 콜드 경로라 잠금 유지 |

즉 **핫 경로(`!_in_active` → true + wake)가 `_out_sync`에서 얻는 보호는 없다**. `_in_active`와 `_state`의 읽기·쓰기는 이 파일 안에서 이미 `_out_sync` 없이 하는 코드(`check_read`, `read_internal`)와 완전히 같은 노출도다. 그래서 핫 경로에서 잠금을 걷어내고, **재분류 콜드 분기에만** 잠금을 남겼다(동작 동일, 술어 재확인 포함). 덤으로 `_head_reclassify_wake`가 idle이면 원자 로드 1회로 조기 반환하므로, 콜드 분기 진입 자체가 사라졌다.

TSan이 이 판단을 확인해 준다(§4): `_in_active`에 대한 `process_activate_read` ↔ `read_internal` 경합은 **S-2 base에서 이미 그대로 보고된다**(base: `pipe.cpp:2744` 읽기 vs `:1196` 쓰기 / after: `pipe.cpp:2751` vs `:1196`). 즉 **새 경합을 만들지 않았고, `_out_sync`가 이 멤버를 지키고 있지 않았다는 것이 실증됐다.** 이 pre-existing 결함은 §7의 B 항목으로 남긴다.

대안 1(기각): `_state`를 `std::atomic<lifecycle_state_t>`로 바꾸고 기존 미러 `_state_active`를 삭제 — 중복 상태 1개를 없애는 더 깨끗한 방향이지만 `pipe.cpp`의 `_state` 사용처 63곳을 기계적으로 고쳐야 해 이번 job의 시간·위험 예산을 넘는다. §7에 후속으로 남긴다.
대안 2(기각): `_in_active`를 `atomic<bool>`로 승격 — 새 원자 1개를 추가하면서도 `read_internal` 쪽 접근 패턴은 그대로라 경합 해소가 안 되고, 상태만 늘어난다.

### (c) `read_activated`의 STREAM raw 상수 분기 3개 → 1회 판정

`socket_base_t::read_activated`는 `pipe_->get_transport_pair_id () != 0`을 **세 블록에서 각각** 평가하고 lane 게터도 두 번 호출했다. STREAM raw는 pair id가 0이므로 세 번 다 실패한 뒤 마지막 블록으로 떨어진다.
브리프의 "pipe 생성 시 캐시된 불리언 1개" 대신 **함수 진입부 로컬 스냅샷 1회**(`const uint64_t pair_id`, `const transport_lane_t lane`)로 접었다. 이유: (1) 새 멤버·새 상태를 만들지 않는다(POSDDD), (2) `_transport_lane_count`는 pair readiness에서 바뀌므로 생성 시 캐시가 곧바로 틀린 값이 된다 — 캐시 불리언은 새 무효화 규칙을 요구하는데 그게 바로 "규칙 추가"다. 순수 코드 이동이며 분기 순서·판정 결과는 동일하다.

## 3. 변경 파일

| 파일 | 내용 |
|---|---|
| `core/src/runtime/core/pipe.cpp` | `pipe_t::process_activate_read()` 재작성 — 핫 경로 무잠금, `_head_reclassify_wake` idle 조기 반환, 재분류 콜드 분기에만 `_out_sync` |
| `core/src/runtime/sockets/common/socket_base_api.cpp` | `socket_base_t::read_activated()` — pair id/lane 스냅샷 1회로 3중 분기 축약 |

(그 밖의 diff는 전부 S-2가 만든 것이며 S-1의 변경이 아니다.)

## 4. 실행한 테스트

| 대상 | 결과 |
|---|---|
| `ctest -R 'wake\|poll\|stream\|pipe\|mailbox\|drain\|progress'` (59 tests) × **5회** | 전부 통과, 실패 0 |
| lost-wake 계열 `test_wake_invariants`·`test_two_poller_wake`·`test_wake_invariant_hwm_lwm_shrink`·`test_wake_invariant_completion_owner`·`test_stream_packet_progress`·`test_stream_send_blocking_wakeup` `--repeat until-fail:10` | **2차례 실행, 두 번 다 100 % 통과 (0 실패)** — 간헐 lost-wake 없음 |
| TSan(`core/build-tsan`, `setarch -R`) `test_wake_invariants`/`test_two_poller_wake`/`test_stream_packet_progress`/`test_stream_multiclient_delivery` 각 1회 | 아래 참조 |

TSan 경합은 **전부 pre-existing**이며 세 부류다.
1. `_in_active`: `process_activate_read` ↔ `pipe_t::read_internal` (`pipe.cpp:1196`). **S-2 base에서 동일하게 보고됨**(base 로그 `tsan-base-two_poller.log`). 내 변경이 만든 것이 아니다.
2. `ypipe_t<command_t,16>::check_read()` (`ypipe.hpp:104,111`) — mailbox cpipe의 의도된 lock-free 패턴, 이번 변경과 무관.
3. `notify_receive_progress_locked` ↔ `receive_once_guarded` (`socket_base_lifecycle.cpp:1543` / `socket_base_msg.cpp:68`) — 이번 변경과 무관.

## 5. 재확인한 스펙 문장 (문장 단위 대조)

### `core/doc/spec/core/05-polling.ko.md` §3 표 — raw socket 행
> "| raw socket | complete record를 수신할 수 있음 | submit 재시도 가치가 있음(socket 전체 집계). 읽지 않은 `ZLINK_COMPLETION_WRITABLE` record가 있는 동안 level로 유지 | socket별 receive mode 적용. close된 등록 socket은 `ZLINK_POLLERR` 1회 |"

**동일.** `POLLIN` level은 `get_events_internal` → `xhas_in()`이 결정하며 이 경로는 손대지 않았다. `read_activated`는 level을 만들지 않고 `xread_activated`로 pipe를 active partition에 넣을 뿐이며, 그 호출 조건(`pair_id`/lane 분류 결과)은 §2(c)의 순수 코드 이동으로 **비트 단위로 같다**. `POLLOUT`·`POLLERR`·`COMPLETION_WRITABLE` 경로는 변경 파일에 등장하지 않는다.

> "여러 peer를 가진 raw socket의 `ZLINK_POLLOUT`은 socket 전체의 집계 readiness다. 이 event는 writable해진 routing ID나 transport pair를 식별하지 않으며, …"

**동일.** 송신 방향(`write_activated`)은 전혀 건드리지 않았다.

### `core/doc/spec/core/socket/README.ko.md` §3 "Pull 수신과 completion 모델"
> "Core가 application에 처리할 항목이 있음을 알리는 경로는 poller readiness와 pull receive다. Core는 application notification callback을 호출하지 않는다."

**동일.** 알림 경로의 개수·종류를 바꾸지 않았다. `_sink->read_activated(this)` 호출 여부의 진리표는 (b) 전후가 같다: `_state ∈ {active, waiting_for_delimiter}` 이고 (`!_in_active` 였거나, `_in_active` 이면서 재분류 marker가 armed→idle 전이에 성공)일 때 정확히 1회.

> "| 일반 DATA | `ZLINK_POLLIN` | socket 종류에 맞는 `*_recv_part()` |"

**동일.** 꺼내는 함수와 readiness의 짝을 바꾸지 않았다.

### `core/doc/spec/core/socket/README.ko.md` §6 `zlink_recv_part`
> "`ZLINK_RECV_FLAGS_DONTWAIT` 호출에 데이터가 없으면 `ZLINK_RECV_NO_DATA`와 `EAGAIN`을 반환한다."(08-stream §5) / "STREAM RAW 수신은 성공 시 part 하나와 `ZLINK_PART_FINAL`을 반환한다."

**동일.** recv 경로(`socket_base_msg.cpp`, `stream.cpp`)는 변경 파일이 아니다. 반환 형태·소유권 이전·`has_more_out_` 값 어느 것도 이 diff가 닿지 않는다.

### `core/doc/spec/core/socket/08-stream.ko.md` §5 "Raw part receive"
> "성공하면 수신 part의 소유권이 호출자에게 이전되며 호출자는 `zlink_msg_close(part_out_)`를 정확히 한 번 호출해야 한다."
> "RAW 수신 record는 단일 part이며, 성공 시 `*has_more_out_`은 `ZLINK_PART_FINAL`이다."

**둘 다 동일.** 이 diff는 wake를 **보낼지 말지**만 다루고, 무엇을 언제 반환하는지는 다루지 않는다.

### D-099 drain 경계 (`7738b8fd41`)
**동일.** drain 경계는 `socket_base_t::drain()`/`process_commands`가 잡으며, `read_activated`의 completion lane 블록(`completion_drain_permitted()`, `process_ready_completion_pipes()`, `drain_claimed_completion_pipe`)은 (c)에서 **한 줄도 바뀌지 않았고** 진입 조건도 같다(`pair_id != 0 && lane == transport_lane_completion`).

**어느 문장도 다른 동작이 되지 않았다.** 깨어나는 시점(같은 batch 안에서 한 번), 순서, POLLIN level 결정 주체(`xhas_in`) 모두 불변이다.

## 6. 변경 분류

**C(우회) 없음 / 본 변경은 B(기존 결함이 아닌 순수 비용 제거)에 가장 가깝다** — 계약 문장을 하나도 건드리지 않고, 보호하지 않던 잠금과 중복 평가만 제거했다. 부수적으로 **B 항목 1건을 발견해 보고**한다(§7).

## 7. 멈춘 지점·후속

- **B(기존 결함, 이번 job 범위 밖)**: `_in_active`는 `process_activate_read`(command 소유 스레드)와 `pipe_t::read_internal`/`check_read`(receive 스레드)에서 **서로 다른 스레드가 동기화 없이** 읽고 쓴다. `_out_sync`는 한쪽만 잡아 보호가 되지 않았고, S-2 base에서도 TSan이 동일하게 보고한다. 소유권을 receive lease 쪽으로 일원화하거나 `_in_active`를 receive 상태 클러스터로 옮기는 별도 job이 필요하다.
- **후속 후보**: `_state`를 `std::atomic<lifecycle_state_t>`로 승격하고 미러 `_state_active`를 삭제(상태 1개 감소). 사용처 63곳의 기계적 변환이라 이번 예산에 넣지 않았다.
- with_stream 표는 S-2 + S-1 합산이다. S-1 단독 분리는 측정 1회 규칙에 따라 하지 않았고, 분리 수치는 callgrind 표(§1, S-2 base 대비)가 대신한다.
