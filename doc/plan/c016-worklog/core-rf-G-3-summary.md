# G-3 — pipe/ypipe layering 비용 축소

> 2026-09-07. worktree `~/project/zlink-work/g3` (detached `a12ef5c515`). **커밋하지 않았다.**
> 원본 데이터: `<scratchpad>/G3/` (`cg_{before,after}_{pair_inproc,router_router_tcp}.out`, `pp.py`, `set.py`),
> 로그 `<scratchpad>/g3-{hotpath-before,hotpath-after,ctest-1..5,lostwake,tsan-*}.log`.
> G-2(msg.* + pipe.cpp의 is_join/is_leave 3줄)와 G-1(socket_base_dispatch·socket_runtime.hpp·
> ctx_physical_queue_registry.cpp)은 **한 줄도 건드리지 않았다**(diff는 `pipe.cpp`·`pipe.hpp` 2파일뿐,
> `is_join`/`is_leave` 매치 0건).

## 1. 결과 — hotpath 5셀 (결정적 명령어 수, 주 판정)

같은 워크트리에서 pristine `a12ef5c515` dev 빌드를 재서 before로 삼았다.

| 셀 | before Ir/msg | after Ir/msg | 변화 | ratio(before→after) |
|---|---:|---:|---:|---|
| `dealer_dealer_inproc` | 4,339.45 | 4,190.70 | **−148.7 (−3.43 %)** | 1.2675 → 1.2241 |
| `dealer_router_reqrep_inproc` | 23,643.03 | 23,386.36 | **−256.7 (−1.09 %)** | 1.2012 → 1.1882 |
| `pair_inproc` | 3,287.59 | 3,177.40 | **−110.2 (−3.35 %)** | 1.3006 → 1.2570 |
| `router_router_tcp` | 3,730.94 | 3,662.87 | **−68.1 (−1.82 %)** | 1.2551 → 1.2322 |
| `stream_tcp` | 16,477.99 | 16,093.50 | **−384.5 (−2.33 %)** | 1.1268 → 1.1005 |

5셀 전부 감소. `hotpath_gate` 자체는 before·after 모두 FAIL이며 이는 기준값이 dev(LTO OFF) 트리 기준이
아니어서 생기는 **기존 실패**다(G-2 §1.1이 pristine에서 같은 FAIL을 확인했다). 이 job은 5셀 ratio를 모두 낮췄다.

## 2. 축소셀 — pipe/ypipe 계열 호출 횟수·self-Ir 표

`hotpath_bench`를 callgrind로 직접 감싼 결정적 측정(`--instr-atstart=no`, 20,000 iter, flock 아래, ninja 0).
G-A의 러너 대신 이 셀을 쓴 이유는 §5에 적었다.

### 2.1 사라진 프레임 (before 호출/msg → after 0)

| 심볼 | pair before 호출/msg · self-Ir/msg | RR-tcp before 호출/msg · self-Ir/msg | after |
|---|---:|---:|---|
| `write_state_admission_unlocked` | 1.00 · 17.0 | 1.00 · 17.0 | **0** |
| `admit_write_unlocked` | 1.00 · 15.0 | — | **0** |
| `hwm_credit_ready_unlocked` | 1.00 · 14.0 | — | **0** |
| `can_commit_bytes_with_peer_snapshot_unlocked` | 1.00 · 20.0 | 1.00 · 20.0 | **0** |
| `can_commit_bytes_unlocked` | 1.00 · 14.0 | 1.00 · 14.0 | **0** |
| `check_hwm_unlocked` | 1.00 · 13.0 | — | **0** |
| `check_hwm_with_peer_snapshot_unlocked` | 1.00 · 8.0 | — | **0** |
| `append_outbound_frame_bytes_unlocked` | 1.00 · 13.0 | — | **0** |
| `write_state_ready_unlocked` | 1.00 · 11.0 | 1.00 · 11.0 | **0** |
| `pending_peer_controls_unlocked` | 1.00 · 6.0 | 1.00 · 6.0 | **0** |
| `peer_uses_routed_protocol_unlocked` | 1.00 · 6.0 | 1.00 · 6.0 | **0** |
| `remote_flow_blocked_unlocked` | 1.00 · 5.0 | 1.00 · 5.0 | **0** |
| `publish_session_outbound_accounting_unlocked` | 1.00 · 4.0 | 1.00 · 4.0 | **0** |
| `get_peer` | 1.00 · 3.0 | 1.02 · 3.1 | **0** |
| **합** | **14.00회 · 149.0** | **9.02회 · 86.1** | **0회 · 0** |

### 2.2 남은 layering 집합 (S-13 정의: pipe_t read/write/flush + ypipe_t + accounting)

| 셀 | before | after | 변화 |
|---|---:|---:|---:|
| `pair_inproc` | 537.1 | 503.1 | −34.0 |
| `router_router_tcp` | 526.0 | 468.5 | −57.5 |

부수 효과로 `frame_accounted_bytes`가 **40.0 → 22.0 Ir/call**로 줄었다(호출자가 인라인되면서 GCC가
상수전파된 사본을 쓴다). `pipe_t::read`(52)·`ypipe_t::read`(49)·`ypipe_t::write`(43)·`ypipe_t::flush`(13)는
**변하지 않았다** — 이들은 virtual(`ypipe_base_t`) 경계 뒤라 인라인 대상이 아니며, §6에 후속으로 남긴다.

## 3. 각 프레임이 소유한 규칙과 병합 판정

브리프 절차 (2)에 따라 각 함수가 소유한 규칙 하나를 적고 판정했다.

| 프레임 | 소유한 규칙 | 판정 |
|---|---|---|
| `get_peer` | peer 포인터의 acquire 로드 | 규칙 1개, 순수 접근자 → **헤더 인라인** |
| `remote_flow_blocked_unlocked` | "PAUSE는 다음 메시지 경계부터 막는다" | 규칙 1개 → 인라인 |
| `write_state_admission_unlocked` | 송신 상태 → admission 코드 매핑 | 규칙 1개 → 인라인 |
| `write_state_ready_unlocked` | **pass-through**(위를 부르고 out 파라미터에 복사, `== ready` 비교) | 자기 규칙 없음 → 인라인으로 프레임 소멸 |
| `admit_write_unlocked` | **pass-through**(`state_ready && credit_ready`) | 자기 규칙 없음 → 프레임 소멸 |
| `hwm_credit_ready_unlocked` | "credit 부족이면 waiter를 arm하고 한 번 더 본다" | 규칙 1개 → 인라인 |
| `check_hwm_unlocked` | in-flight ≥ hwm 이면 full | 규칙 1개 → 인라인 |
| `check_hwm_with_peer_snapshot_unlocked` | "실패하면 peer 스냅샷을 갱신하고 재판정"(= `can_commit_*_with_peer_snapshot`과 **같은 규칙의 두 번째 사본**) | 인라인. 두 사본은 술어가 다르므로 소스 통합은 하지 않았다(§4) |
| `can_commit_bytes_unlocked` | HWM/최대 메시지/빈 파이프 예외 | 규칙 1개 → 인라인 |
| `can_commit_bytes_with_peer_snapshot_unlocked` | 위와 동일한 재판정 규칙 | 인라인 |
| `append_outbound_frame_bytes_unlocked` | 미완결 바이트 누적 + 오버플로 EMSGSIZE | 규칙 1개 → 인라인 |
| `pending_peer_controls_unlocked` | "슬롯 2개가 존재를 소유한다" | 규칙 1개 → 인라인 |
| `peer_uses_routed_protocol_unlocked` | peer 타입이 D/R인가 | 규칙 1개 → 인라인 |
| `publish_session_outbound_accounting_unlocked` | 뜨거운 가드 `!_session_io_writer` + 차가운 publish | **냉·열 분리**: 가드만 인라인, publish는 `..._slow_unlocked`로 아웃라인 |

**왜 프레임이 남아 있었나(브리프 절차 4, G-2가 찾은 인라인 예산 패턴의 동일 사례).**
이 14개는 전부 `pipe.cpp` 안에서만 쓰이는 private 멤버이고 3~20 Ir짜리 술어다. 그런데도 아웃라인
사본이 호출된 이유는 **호출자**(`write_message_unlocked` 133 Ir, `write_single_message_and_flush_*` 127 Ir,
`flush_unlocked` 45 Ir, `read_internal` 82 Ir)가 GCC의 함수 성장 예산을 한참 넘겨 IPA-inline이
피호출자를 거절했기 때문이다. G-2의 `msg_t::size/data/check`와 원인이 같다(그쪽은 assert 매크로 전개,
이쪽은 호출자 크기). 헤더로 옮기자 14개 심볼이 **두 셀 모두에서 호출 0회로 사라졌다**.

## 4. 설계 비교와 선택 이유

**(A) 헤더 inline 이동(채택).** 본문을 `pipe.hpp` 클래스 뒤에 `inline`으로 그대로 옮긴다.
평가 순서·부작용·반환값이 바이트 단위로 같은 순수 코드 이동이고, 새 옵션·플래그·상태가 0개다.
`publish_session_outbound_accounting_unlocked`만 G-2식 냉·열 분리를 했다(뜨거운 가드 1로드).

**(B) 소스 수준 프레임 통합(부분 기각).** `write_state_ready_unlocked`/`admit_write_unlocked`를
호출자에 손으로 접고, `check_hwm_with_peer_snapshot_unlocked`와
`can_commit_bytes_with_peer_snapshot_unlocked`를 "재판정" 규칙 하나로 합치는 방향도 검토했다.
기각한 이유: (1) 기계어 수준의 이득은 (A)와 같다(측정으로 확인 — 14개 전부 소멸), (2) 두
`*_with_peer_snapshot_*`는 **술어가 서로 다르다**(하나는 바이트 HWM, 하나는 payload/빈 파이프 예외까지).
공통 템플릿으로 묶으면 호출부마다 어느 술어인지 알기 어려워져 "규칙 수"는 그대로인데 읽는 비용만 오른다.
(3) `admit_write_unlocked`를 지우면 호출부 여러 곳이 두 술어의 **순서**(state 먼저, credit 나중 —
`_waiting_for_flow_resume` arm이 credit arm보다 앞서야 한다)를 각자 재현해야 한다. 규칙을 없애는 게
아니라 **중복시키는** 방향이라 POSDDD에 어긋난다.

**(C) `ypipe_base_t` virtual 제거(이번 예산 밖, §6).**

## 5. 측정 방법 — G-A 러너 대신 `hotpath_bench`를 쓴 이유

G-2 §10의 권고를 따랐다. G-A의 축소 perf 셀은 같은 구성 2판 편차가 ±170 Ir/msg라 이 크기(−68~−385)의
변경을 판정할 분해능이 없고, 하네스 `getenv` 오염(G-5)도 섞인다. `hotpath_bench`는 공개 API를
`CALLGRIND_START_INSTRUMENTATION` 구간에서만 세는 결정적 계측기이므로 before/after 차이가 곧 신호다.
심볼 표(§2)도 같은 바이너리의 callgrind 덤프에서 뽑아 §1과 완전히 같은 실행을 본다.
측정은 전부 `flock <PERF_LOCK>` 아래, `pgrep -x ninja` 0을 확인하고 실행했다(load avg 4.77 / 4.72).

## 6. 변경 파일

| 파일 | 내용 |
|---|---|
| `core/src/runtime/core/pipe.hpp` | 14개 1규칙 술어의 정의를 클래스 뒤 `inline`으로 이동(+171줄); `pending_peer_weight_unset` 상수를 `pipe.cpp` 익명 namespace에서 헤더 namespace로 이동; `publish_session_outbound_accounting_slow_unlocked` 선언 추가 |
| `core/src/runtime/core/pipe.cpp` | 위 14개 정의 삭제(−169줄); `publish_session_outbound_accounting_slow_unlocked` 정의 추가(차가운 publish 본문) |

`core/include/**`·`libzlink.vers`·공개 계약 테스트 **무변경**. `ypipe.hpp`·`yqueue.hpp` **무변경**.

## 7. ypipe sleep/awake 진리표 — 전후 동일 (브리프 절차 3)

S-1 보고서 §2(a)가 확정한 진리표를 그대로 인용한다:

> `pipe.cpp` `flush_unlocked()`의 `const bool sleeping = _out_pipe && !_out_pipe->flush ();` 가
> **유일한 발송 조건**이다. `ypipe_t::flush()`는 reader가 잠들어 있을 때만 false를 돌려주고 그 순간
> `_c`를 `_f`로 올려 pipe를 깨어 있음으로 표시한다. reader가 `check_read()`에서 큐를 비우고 다시
> 잠들기 전까지 두 번째 flush는 반드시 true를 돌려준다. → sleep episode당 정확히 1회.

**증명(전후 동일).** 이 진리표를 구성하는 코드는 세 곳이며 diff가 그 어느 줄에도 닿지 않았다:
1. `ypipe.hpp`의 `flush()`/`check_read()` — 파일 전체가 diff에 없다.
2. `pipe.cpp::flush_unlocked()`의 `sleeping`·`reclassify_candidate`·`send_activate_read (peer)` —
   `git diff`에서 `sleep|flush ()|activate_read|_c.cas` 패턴에 걸리는 `+`/`-` 줄이 **0건**이다.
3. `pipe.cpp::process_activate_read()`/`check_read()`/`read_internal()`의 `_in_active` 전이 — 삭제·이동 대상이 아니다.
`flush_unlocked`가 부르는 `pending_peer_controls_unlocked`·`peer_uses_routed_protocol_unlocked`·`get_peer`는
인라인만 됐을 뿐 술어·평가 순서가 같으므로 `sleeping`/`explicit_reclassify`의 값과 `send_activate_read`
호출 횟수는 모든 입력에서 동일하다. 실측도 같다: 두 셀 모두 `send_activate_read` 호출/msg가 before·after
0.000(정상 상태에서 reader가 잠들지 않음)으로 변하지 않았다.

## 8. 실행한 테스트와 남은 실패

| 대상 | 결과 |
|---|---|
| `ctest -R 'pipe\|wake\|poll\|stream\|hwm\|flow\|router\|dealer\|pair' -E hotpath` (71 tests) × **5회** | **5회 전부 100 % 통과, 실패 0** |
| lost-wake 6종 `--repeat until-fail:10` (`test_wake_invariants`, `test_two_poller_wake`, `test_wake_invariant_hwm_lwm_shrink`, `test_wake_invariant_completion_owner`, `test_stream_packet_progress`, `test_stream_send_blocking_wakeup`) | **100 % 통과, 실패 0** |
| TSan(`core/build-tsan` 신규 구성, `setarch -R`) `test_wake_invariants`/`test_two_poller_wake`/`test_stream_packet_progress` | **신규 경고 0** — 아래 |
| `hotpath_gate` | FAIL이지만 pristine에서도 같은 FAIL(§1) |

TSan이 보고한 race 지점은 두 종류뿐이고 **둘 다 S-1 §4가 pre-existing으로 기록한 것**이다:
`ypipe_t<command_t,16>::check_read()` (`ypipe.hpp:104,111` — mailbox cpipe의 의도된 lock-free 패턴),
`notify_receive_progress_locked` ↔ `receive_once_guarded` (`socket_base_lifecycle.cpp:1543` /
`socket_base_msg.cpp:68`). **`pipe.cpp`·`pipe.hpp`를 최상단 프레임으로 갖는 경고는 0건**이다.
전체 ctest는 공통 규칙대로 돌리지 않았다.

## 9. 재확인한 스펙 절 — 어느 문장도 다른 동작이 되지 않았다

- `05-polling.ko.md` §3 표(raw socket 행), "`ZLINK_POLLOUT`은 socket 전체의 집계 readiness다" —
  POLLIN/POLLOUT level은 `get_events_internal`→`xhas_in`/`xhas_out`이 결정하며 이 diff에 없다.
  wake 발송 조건은 §7에서 줄 단위로 동일함을 증명했다.
- `08-stream.ko.md` §5 "raw part receive"의 소유권·`ZLINK_PART_FINAL` — `read_internal`의 반환 형태
  무변경(삭제·이동 대상 아님).
- `socket/README.ko.md` §3 "Core는 application notification callback을 호출하지 않는다" — 알림 경로의
  개수·종류 무변경.
- HWM/LWM 계약(`06-auto-hwm`): `check_hwm_unlocked`·`can_commit_bytes_unlocked`의 부등식과 빈 파이프
  예외 조건이 문자 단위로 같다. 브리프가 지목한 `account_inbound_frame`의 published store 빈도는
  **06-auto-hwm §7.5 D-d 스냅샷 정의에 걸리므로 손대지 않았다**(그 함수는 diff에 없다).
- ABI: 공개 헤더·`libzlink.vers` 무변경. `pipe_t`의 멤버 레이아웃 무변경(멤버를 추가·삭제하지 않았다).

## 10. 변경 분류

**B(기존 결함) — 인라인 예산 초과로 1규칙 술어 14개가 메시지당 9~14개의 호출 프레임을 만들고 있었다.**
계약 적응(A)도, 우회(C)도, spec gap(D)도 없다.

## 11. 멈춘 지점 · 후속 후보

멈춘 지점은 없다(1.5 h 안에 완료).

- **후속 1 (가장 큰 남은 layering)**: `ypipe_base_t`가 `write/flush/check_read/read/read_if/probe_if_published`를
  **virtual**로 두기 때문에 `pipe_t`↔`ypipe_t` 경계는 인라인이 원천 차단된다(pair 셀 105 Ir/msg:
  `read` 49 + `write` 43 + `flush` 13). virtual이 존재하는 유일한 이유는 conflate 파이프 1종이며,
  일반 파이프는 granularity만 다른 `ypipe_t<msg_t,64>`/`<msg_t,256>` 2종이다. "conflate 여부 분기 1개 +
  구체 타입 호출"로 접으면 규칙이 하나 늘지만 프레임 3개가 사라진다 — 이득/규칙 교환이라 별도 job의 판단이 필요하다.
- **후속 2**: `read_internal`이 `_registry_accounting` 경로에서 `frame_accounted_bytes(msg_)`를 계산해
  `release_committed_frame`에 넘긴 직후 `account_inbound_frame`이 **같은 값을 다시 계산**한다
  (registry 계정 파이프에서 프레임당 1회 중복). 값을 인자로 내려보내면 사라진다. 이번 셀들은
  `_registry_accounting == false`라 측정에 잡히지 않아 손대지 않았다.
- **후속 3**: `pipe_t::write_message_unlocked`(121 Ir) / `socket_base_t::try_admit_send_parts_scoped`(131 Ir) /
  `send_direct_with_retry`(110 Ir)가 이제 pair 셀 pipe 계열의 최상위다. G-1 범위와 겹친다.
