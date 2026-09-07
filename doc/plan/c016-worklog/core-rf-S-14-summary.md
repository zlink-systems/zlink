# core-rf-S-14 요약 — `test_sl_flow_snapshot_accounts_dr_reply_as_application` 간헐 실패

worktree: `~/project/zlink-work/s14` (detached 3bb34d5459)

## 1. 결과 (수치)

| 구성 | before | after |
|---|---|---|
| solo 30회 (dev, 같은 빌드) | 28 PASS / **2 FAIL** | **30 PASS / 0 FAIL** |
| solo 20회 (계측 전 최초 확인) | 19 / **1 FAIL** | — |
| 계측 빌드 solo 40회 | 26 / **14 FAIL (35%)** | — |
| `ctest -j4 -R 'stream|pipe'` 부하 동시 10회 | — | **10 / 0** |
| `ctest -R 'lane|hwm|flow|snapshot|accounting|dealer|router'` 5회 (60 tests) | — | **5회 모두 100% pass** |
| `setarch -R` 10회 | — | **10 / 0** |

## 2. 증명된 원인 (진단서 가설은 **반증**)

진단서 §4의 유력 후보(`accounting_lane()`의 lane 경합)는 **틀렸다**. 계측으로 직접 반증했다.
실패·성공 양쪽 모두 스냅샷 시점에 `lane=1(application)`, `endpoint_refs=2`,
`application_writer/reader` 바인딩 정상, `sample_application_pipe_queue` 성공(`sampled=1`)이었다.
(`ctx_physical_queue_registry.cpp:184 accounting_lane()`은 completion/monitor가 아니면 항상
application을 돌려주고, `create_pipepair_queues`가 이미 lane을 확정해 넣으므로
`classify_pipepair_queues`는 이 경로에서 사실상 no-op다. 경합할 상태가 없다.)

실제 원인은 **reply를 물리 큐에서 먼저 빼 가는 비동기 드레인**이다. 계측 로그(성공/실패 대비):

```
FAIL: [DBGWR] tid=main  pipe=R q=Q more=0 fb=1088 written=0 incompl=2176   (ROUTER가 FINAL 기록)
      [DBGRD] tid=IO/1  pipe=D peer=R bytes=2176                          (← 스냅샷 전에 드레인)
      [DBGSMP] w=R r=D written=2176 consumed=2176 avail=0 → appcur=0  (5 s 내내 0)
PASS: [DBGWR] ... incompl=2176
      [DBGSMP] w=R r=D written=2176 consumed=0 avail=2176 → appcur=2176
      [DBGRD] tid=IO/1 pipe=D bytes=2176                                  (스냅샷 뒤에 드레인)
```

드레인 주체(backtrace로 확정):

```
pipe_t::account_inbound_frame            core/src/runtime/core/pipe.cpp:3750
pipe_t::read                             core/src/runtime/core/pipe.cpp:1080
socket_reqrep_internal::process_completion_pipe
                                         core/src/api/socket/socket_request_reply_dispatch.cpp:227
socket_base_t::drain_claimed_completion_pipe
                                         core/src/runtime/sockets/common/socket_base_api.cpp:1546
socket_base_t::process_ready_completion_pipes
                                         core/src/runtime/sockets/common/socket_base_api.cpp:1360
socket_base_t::process_async_mailbox      core/src/runtime/sockets/common/socket_base_lifecycle.cpp:1479
socket_base_t::async_mailbox_handler      core/src/runtime/sockets/common/socket_base_lifecycle.cpp:321
thread_routine (ZLINKbg/IO/0|1)
```

즉 ROUTER가 FINAL을 쓰면 DEALER 메일박스에 command가 실리고, 그 메일박스를 처리하러 깨어난
**백그라운드 IO 스레드**가 같은 턴에서 `process_ready_completion_pipes()`를 돌려 2176 B reply를
물리 큐에서 socket-local completion store로 옮긴다. 그 순간 pipe-local 원장의
`written - consumed`가 0이 되어 `core_queue_accounted_bytes`/`current_accounted_bytes`가 0이 된다.
테스트 main 스레드의 첫 폴이 이 드레인보다 빠르냐 느리냐가 그대로 2176 vs 0 이분법을 만든다.
(부분 드레인 관측도 있었다: `consumed=1088 → appcur=1088`. 즉 창(window) 문제가 맞고, lane 문제가 아니다.)

## 3. 소유자와 스펙 문장

- 전이 소유자: `socket_base_t::process_async_mailbox()`
  (`core/src/runtime/sockets/common/socket_base_lifecycle.cpp:1471-1481`).
  이 드레인은 **의도된 설계**이며 소유권 게이트가 명시돼 있다:
  `_completion_poller_refs == 0`일 때만 async executor가 completion을 드레인한다.
  `socket_base_dispatch.cpp:254-257` 주석: *"A public POLLCOMPLETION registration is the sole
  completion owner while it exists … the async executor already skips that drain while a public
  poller owns it."*
- 회계 스펙(무엇이 계상되는가):
  - `core/doc/spec/core/systems/06-auto-hwm.ko.md:587` — “**Controlled DEALER-ROUTER reply를
    queue에 남기면** 그 byte delta가 `core_queue_accounted_bytes`, `current_accounted_bytes` …에
    나타난다.” (en:465) — 전제가 “queue에 남기면”이다.
  - `core/doc/spec/core/systems/06-auto-hwm.en.md:459` — “After receiving a complete message,
    Core queue charge ends and the sender can send again.”
  - `core/doc/spec/core/systems/05-connection-memory.en.md:57-59` — “a charge is part of the
    connection's memory cost **only while the frame is in the queue**, and a binding or application
    holding the payload after dequeue does not add to it.”
  - `05-connection-memory.en.md:72-75` — pending-request work charge는 “neither actual allocator
    bytes nor retained payload bytes and is **not included in queue-HWM current or snapshot values**”.

→ 스펙은 **dequeue 시점에 charge가 끝난다**고 못박고 있다. IO 스레드의 드레인은 dequeue다.
따라서 Core 회계는 스펙대로 동작했고, 회계 쪽에 결함이 없다. 결함은 **테스트가 스펙 전제
(“queue에 남기면”)를 성립시키지 않은 것**이다. DEALER를 RUNNING으로 되돌린 뒤 completion 소유자
없이 FINAL을 넣으면, reply가 큐에 남아 있을지 여부는 IO 스레드와의 경주로 결정된다.
(같은 이름의 unittest가 10/10 통과하는 이유도 이것이다 — pump 기반이라 IO 스레드 드레인이 없다.)

## 4. 수정

`core/tests/integration/test_dealer_router_single_lane_contract.cpp` 한 파일, +15줄.
스펙이 이미 정해 둔 소유권 규칙을 그대로 쓴다: 공개 API `zlink_poller_add(..., ZLINK_POLLCOMPLETION)`로
DEALER의 completion 소유자를 등록해 두면 async executor가 드레인을 건너뛰므로 reply가 실제로
**큐에 남는다**. queued 스냅샷 검증이 끝난 뒤 `zlink_poller_remove`/`zlink_poller_destroy`로
소유권을 반납하고, 이후 `receive_completion` 이하는 원본 그대로다.

- 새 플래그·옵션·상태 없음. Core 소스 무변경. `core/include/**`·`libzlink.vers` 무변경.
- **기대값(TEST_ASSERT_*의 값)은 한 줄도 바꾸지 않았다.** 추가된 것은 setup/teardown 3+2줄뿐이다.
- 같은 파일의 다른 케이스(`test_sl_request_reply_at_head_completion_only:1662`)가 이미 쓰는 패턴과 동일하다.

### 설계 비교

| 안 | 내용 | 판정 |
|---|---|---|
| A (채택) | 테스트가 completion 소유자를 잡아 스펙 전제(“queue에 남기면”)를 성립시킴 | 규칙 0개 추가, Core 무변경, 결정적. 채택 |
| B | reply가 completion store에 있는 동안에도 물리 큐 charge를 유지 | `05-connection-memory.en.md:57-59`·`06-auto-hwm.en.md:459`(dequeue에 charge 종료)와 정면 충돌. writer credit/HWM 해제 시점까지 바뀌므로 **계약 변경**. 금지 항목이라 채택 불가(§5 D 참고) |
| C | async mailbox의 opportunistic 드레인을 poller가 없을 때 끔 | 콜백 기반 completion 전달 liveness를 깨고, `socket_base_dispatch.cpp:254-257`이 명시한 소유권 모델을 뒤집는다. 규칙 추가. 기각 |

## 5. 실행한 테스트와 남은 실패

- solo 30회 (before 2 FAIL → after 0 FAIL), `stream|pipe` 부하 동시 10회 0 FAIL,
  `lane|hwm|flow|snapshot|accounting|dealer|router` 60 tests × 5회 전부 100% pass, `setarch -R` 10회 0 FAIL.
- 남은 실패: 없음.
- TSan: **돌리지 않았다.** 변경이 테스트 파일 한 곳(공개 API 호출 추가)뿐이라 Core 동시성 코드가
  전혀 바뀌지 않았고, `core/CMakeLists.txt:67-79`가 libzlink TSan 빌드는 상호운용 목적이며
  race 검출용이 아니라고 명시한다. before/after 델타가 정의되지 않아 생략했다.

## 6. 성능

측정 없음(테스트 전용 변경, 라이브러리 코드 무변경).

## 7. 재확인한 스펙 절

`06-auto-hwm.ko.md:191-194,483,587-597` / `06-auto-hwm.en.md:148-155,459,465-472`,
`05-connection-memory.en.md:53-75`, `core/doc/spec/core/socket/README.*`의 single-lane·completion
소유권 절, `socket_base_dispatch.cpp:222-300`의 poller 소유권 주석.
**어느 문장도 다른 동작이 되지 않았다** — Core 런타임 동작·완료 순서·POLLIN/POLLOUT/WRITABLE
계약은 그대로이고, 테스트는 스펙이 이미 정의한 소유권 상태를 명시적으로 만들었을 뿐이다.

## 8. 분류

**B (기존 결함)** — G-1/G-3 무관한 pre-existing 결함. 다만 결함 위치는 Core 회계가 아니라
**계약 테스트의 전제 설정**이다. 계측으로 lane 가설을 반증하고 드레인 주체를 backtrace로 특정했다.

## 9. 남는 관찰 (D 후보, 이번 job에서 손대지 않음)

DEALER가 reply를 completion store로 옮긴 뒤 `zlink_completion_recv` 전까지, Core가 실제로 들고 있는
payload 바이트는 **어떤 스냅샷 필드에도 나타나지 않는다**(`05-connection-memory.en.md:72-75`가
work charge는 payload 바이트가 아니라고 명시). 스펙상 의도된 공백이지만, “Core가 보유 중인 메모리”
관측 관점에서는 사각지대다. 필요하면 별도 spec 논의로.
