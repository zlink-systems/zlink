# q1695d — Issue #1695 진단: `ZLINK_OPT_EVENTS`가 `ZLINK_POLLIN`을 돌려주지 않는다

기준 checkout: `/root/zlink` HEAD `4bf652482f` (main). 코드·스펙·GitHub은 건드리지 않았고 빌드·시험도 돌리지 않았다.

## 1. 결론

- **main에서는 이미 고쳐져 있다.** 지목한 코드(`has_out () ? ZLINK_POLLOUT : 0`)는 commit
  `1ad2b0439c` "fix(core): report input readiness from ZLINK_OPT_EVENTS" (2026-10-14, Refs #1695)에서
  poller와 같은 판정 함수 `get_events_internal (ZLINK_POLLIN | ZLINK_POLLOUT, …)`를 부르는 형태로 바뀌었다.
  스펙도 그보다 먼저 `2c060ffdb5`·`2790a174b0`(같은 날)에서 EVENTS의 bit와 부작용을 정했다.
- **배포본에는 아직 없다.** `git tag --contains 1ad2b0439c`에 `core/*` tag가 없다(최신 `core/v1.19.0`).
  Issue 증상(1.19.0에서 poller는 POLLIN=1, EVENTS는 0)은 1.19.0 코드로 설명되며, 다음 Core release로 해소된다.
- main의 수정은 이 job이 요구한 설계(“poller가 이미 쓰는 판정 하나를 재사용, 새 상태·lock·분기 없음”)와 일치한다.
  추천안도 이 형태이며, 추가 구현은 필요 없다. 남은 일은 시험 보강 1건(§6의 FD 경로)과 별도 진단 후보 1건(§8)이다.
- 분류: **B (기존 결함)**. 근거는 §8.

## 2. 지목 위치 확인 (main 기준)

| 지목 | 1.19.0 시점 내용 | main 현재 |
|---|---|---|
| `core/src/runtime/sockets/common/socket_base_api.cpp:777–786` | `process_commands` 뒤 `has_out () ? ZLINK_POLLOUT : 0` 반환 | `:777–789`. `drain_primary_signaler ()`(:780) 뒤 `get_events_internal (ZLINK_POLLIN \| ZLINK_POLLOUT, &events, true)`(:781–783), 결과를 두 bit로 mask해 반환(:786–787) |
| `core/src/api/core/zlink_option_mapping.cpp:31` | `{ZLINK_OPT_EVENTS, ZLINK_INTERNAL_OPT_EVENTS}` | 같은 줄, 변경 없음 |

변경 diff는 `git show 1ad2b0439c -- core/src/runtime/sockets/common/socket_base_api.cpp`로 확인했다(+6/−5줄).

## 3. 결정 소유 표 — “이 socket에 지금 읽을 수 있는 record가 있는가(POLLIN)”

Core에는 thread-safe socket과 일반 socket의 구분이 따로 없다(`rg thread_safe core/src` 0건). 모든 socket이
같은 `socket_base_t` 경로를 쓴다.

### 3.1 판정 위치

| 층 | 위치 (file:line) | 하는 일 |
|---|---|---|
| 공통 판정 | `socket_base_api.cpp:1022–1040` `socket_base_t::has_in` | receive turn 안에서 BUFFER_TOO_SMALL로 남긴 part-helper record를 먼저 보고, 없으면 `xhas_in ()` |
| bit 조립 | `socket_base_api.cpp:865–927` `get_events_internal` (POLLIN은 :919) | `process_commands` 뒤 COMPLETION·POLLIN·POLLOUT·POLLROUTE bit를 조립한다. POLLIN을 조립하는 유일한 곳 |
| 호출자: poller | `socket_base_api.cpp:931–961` `get_events_for_poller` ← `core/src/runtime/core/socket_poller.cpp:555` | `zlink_poll`·`zlink_poller_wait`의 socket readiness |
| 호출자: EVENTS | `socket_base_api.cpp:777–789` | main에서 `get_events_internal` 재사용 |
| 호출자: 내부 대기 | `core/src/runtime/core/recv_internal.cpp:346–390` `wait_socket_events_internal` | POLLIN 단독이면 `socket_poller_t`, 아니면 :377에서 `get_events_internal` |
| type별 `xhas_in` | 기본 `socket_base.cpp:595` (false) | — |
| | PAIR `sockets/pair/pair.cpp:205` | `_pipe->check_read ()` |
| | DEALER `sockets/dealer/dealer.cpp:428` → `sockets/internal/fq.cpp:461` | fair-queue `check_read`, read miss면 pipe 비활성화 |
| | ROUTER `sockets/router/router_recv_path.cpp:474–498` | `recv_selected`로 record를 **prefetch**(`_prefetched=true`), token slot은 `probe_router_reply_token_admission`(:37–50)으로 확인만 한다 |
| | SUB·XSUB `sockets/pubsub/xsub.cpp:372–` (SUB는 `sub.hpp:15`에서 상속, `xhas_in`은 FINAL) | 구독에 맞는 message를 **prefetch**(`_has_message`), 맞지 않는 message는 버린다 |
| | XPUB `sockets/pubsub/xpub.cpp:428` | `_pending_data` 비었는지 |
| | PUB `sockets/pubsub/pub.cpp:36` | 항상 false |
| | STREAM `sockets/stream/stream.cpp:1087–1097` | PACKET mode는 `pump_packet_receive_queue`로 packet 조립, RAW mode는 notify 목록 또는 `fq.has_in` |

POLLIN을 다루지만 판정하지 않는 곳: `socket_base_api.cpp:803–863` `socket_base_t::get_events`는
COMPLETION·POLLOUT만 조립한다. `api/monitoring/poller_poll_once.cpp:128–131`과
`api/monitoring/poller_api.cpp:413–417`이 POLLIN을 빼고(`& ~ZLINK_POLLIN`) 부른다.

### 3.2 스펙 소유 조항

| 결정 | 소유 조항 |
|---|---|
| raw socket POLLIN의 의미 | `core/doc/spec/core/05-polling.ko.md:48–55` (§3 표, “complete record를 수신할 수 있음”) |
| EVENTS가 돌려주는 bit와 부작용 | `05-polling.ko.md:57–62` |
| type별 POLLIN 구체 의미 | DEALER `socket/06-dealer.ko.md:150`, ROUTER `socket/07-router.ko.md:243–245`, STREAM `socket/08-stream.ko.md:223`, 수신 함수 대응 `socket/README.ko.md:71–74` |
| flag 값 | `04-events.ko.md:48` |
| poll 경로의 hot path 범위 | `systems/10-hot-path.ko.md:41` |

### 3.3 하나로 모을 소유자

POLLIN 판정의 소유자는 `socket_base_t::has_in`(type별 `xhas_in`으로 위임)이고, event bit 조립의 소유자는
`get_events_internal`이다. main에서는 poller·EVENTS·내부 대기가 모두 이 하나를 부른다.

## 4. 스펙 rg 대조

| 검색어 (범위 `core/doc/spec`) | 위치 |
|---|---|
| `ZLINK_OPT_EVENTS\|OPT_EVENTS` | `core/05-polling.ko.md:57`, `core/05-polling.en.md:61`, `core/socket/README.ko.md:361`, `core/socket/README.en.md:384` |
| `has_in` | `core/systems/10-hot-path.ko.md:41`, `.en.md:45` (`get_events_internal → process_commands → xhas_in / xhas_out`) |
| `POLLIN` | `04-events.ko.md:48`; `05-polling.ko.md:22,30,48,57,105,109,181,350`; `socket/README.ko.md:71,72,74`; `socket/06-dealer.ko.md:150,155,385`; `socket/07-router.ko.md:243,285,394`; `socket/08-stream.ko.md:223,438` (en 대응본 포함 같은 분포) |
| `readiness\|readable` | 주요: `05-polling.ko.md`(38건), `04-events.ko.md`(8), `systems/11-synchronization-model.ko.md:30,124,126,188,192,332`, `systems/10-hot-path.ko.md`(2) |

어긋남 점검:

- `socket/README.ko.md:361`(“반환 bit는 05-polling §3”)과 `05-polling.ko.md:57–62`는 서로 맞고, ko·en(`05-polling.en.md:61–67`)도 같은 내용이다.
- `05-polling.ko.md:59–62`는 “조회가 poller 판정과 같은 내부 진행(prefetch, 구독 필터링, packet 조립)을 할 수 있지만 record를 소비·폐기하지 않고, 예약하지 않는다”고 정한다. §3.1의 ROUTER·XSUB·STREAM 부작용이 정확히 이 범위이고, ROUTER의 token probe(`router_recv_path.cpp:46`)는 slot을 예약하지 않으므로 “예약하지 않는다”와도 맞는다.
- 비차단 gap: `05-polling.ko.md:30`은 type별 POLLIN 의미의 소유 문서로 DEALER·ROUTER만 든다. PAIR·SUB·XSUB·XPUB·PUB 문서(`socket/01-pair`~`05-xsub`)에는 POLLIN 조항이 없어 §3 표의 일반 문장이 그 역할을 한다. XPUB의 POLLIN이 “구독 message 수신 가능”을 뜻한다는 점은 어디에도 명시돼 있지 않다. 이번 결함의 판단에는 영향이 없다.
- `ZLINK_OPT_FD`는 `socket/README.ko.md:360`의 한 줄 정의뿐이다. EVENTS와 FD의 edge 관계는 §8에서 다룬다.

## 5. 추천 수정안 (main에 이미 반영됨)

바꿀 곳: `socket_base_api.cpp` `socket_base_t::getsockopt`의 `ZLINK_INTERNAL_OPT_EVENTS` 분기 한 곳.

| 안 | 내용 | event bit 조립 위치 수 (수정 전 → 후) | 평가 |
|---|---|---|---|
| **A (추천, main 반영)** | EVENTS가 `get_events_internal (POLLIN\|POLLOUT, …, true)`을 부르고 두 bit로 mask | 3 → 2 (`get_events_internal`, `get_events`, EVENTS 자체 조립 → 앞의 둘). POLLIN 판정은 1(`has_in`) 그대로이고, EVENTS가 그것을 쓰지 않던 누락이 사라진다 | 새 상태·lock·분기 없음. 기존 `process_commands` 호출이 `get_events_internal` 안으로 들어가 줄이 줄었다 |
| B | EVENTS가 `get_events_for_poller`를 부른다 | 3 → 2 | admission scope를 한 번 더 잡고, poller 전용 인자(`transport_output_`, signaler 소비)를 EVENTS가 정해야 한다. 규칙이 하나 늘어 기각 |
| C | 기존 EVENTS 조립에 `has_in ()` 한 줄 추가 | 3 → 3 | POLLIN·POLLOUT 조립이 두 곳에 남는 중복이다. AGENTS.md §3 “원칙이 단순해지는 근본 수정”에 어긋나 기각 |

수정 전/후 규칙 수: event bit 조립 위치 3 → 2, POLLIN 판정 위치 1 → 1(EVENTS 누락 해소).
더한 상태·플래그·queue·lock: 0.

### 5.1 `has_in` 부작용과 recv 의미

- **lock**: `has_in`의 `socket_receive_entry_scope_t`는 `acquire_turn`(`socket_lifecycle_runtime.cpp:63–70`)에서 public API sync를 잡는다. 그런데 현재 thread가 이미 소유하면 다시 잡지 않는다. EVENTS는 `socket_public_api_lock_scope_t`를 이미 쥐고 있으므로 새 lock도, 새 lock 순서도 생기지 않는다. part-helper `state->mutex`(:1033)는 poller 경로에도 있던 기존 lock이다.
- **ROUTER prefetch**: `_prefetched_msg`·`_prefetched_id`·`_current_in`에 담기고, recv 경로가 먼저 소비한다(`router_recv_path.cpp:297–307, 366–387`). 받을 record와 순서는 바뀌지 않는다. token slot은 예약하지 않는다.
- **XSUB/SUB prefetch**: `_has_message`로 보존되고 `xsub.cpp:278–281`에서 다음 recv가 그 message를 먼저 돌려준다. 구독에 맞지 않아 버려지는 message는 application에 원래 전달되지 않는 것이다(`05-polling.ko.md:59`의 “구독 필터링”).
- **STREAM PACKET**: packet을 조립해 `_packet_receive_queue`에 보존한다.
- **DEALER/PAIR**: `check_read`가 빈 ypipe의 reader를 재워, 다음 write가 `activate_read`를 보내게 만든다. poller와 같은 비용이며 hot path §3-5의 대상은 message 경로이지 EVENTS 조회가 아니다.
- 결론: 부작용은 모두 poller가 이미 일으키던 것이고, 스펙 `05-polling.ko.md:59–62`가 허용한 범위 안에 있다. recv 의미(받을 record, 순서, RID, token)는 바뀌지 않는다.

POLLOUT 참고: EVENTS는 `has_out`(public 의미)을 쓰고, poller는 `transport_output_readiness` mode일 때만 `transport_has_out`을 더한다(`socket_base_api.cpp:956–959`). 이번 결함과는 관계없고, public poller가 어느 mode인지는 확인하지 않았다.

## 6. 시험 설계

대상 socket 종류(코드 근거):

| 종류 | 근거 | 주는 쪽 |
|---|---|---|
| PAIR | `pair.cpp:205` | PAIR |
| DEALER | `dealer.cpp:428` | DEALER |
| ROUTER | `router_recv_path.cpp:474` (prefetch) | DEALER |
| SUB, XSUB | `xsub.cpp:372` (prefetch·filter) | PUB/XPUB |
| XPUB | `xpub.cpp:428` (구독 message) | SUB/XSUB의 subscribe |
| STREAM RAW, STREAM PACKET | `stream.cpp:1087` | 외부 raw TCP 또는 STREAM |
| PUB (음성 대조) | `pub.cpp:36` 항상 false | — |

main의 `core/tests/integration/test_option_events.cpp`(278줄, `core/tests/CMakeLists.txt` label `contract`)가 위 9개 case를 모두 다룬다.
공개 C API만 쓰고 sleep 없이 `zlink_poll` timeout으로 동기화한다. 각 case의 흐름은 이렇다. `expect_empty`(EVENTS에 POLLIN이 없고 `zlink_poll` 0) → 송신 →
`expect_readable`(`zlink_poll`로 POLLIN을 확인한 뒤 EVENTS 2회 모두 POLLIN) → DONTWAIT 수신 후 payload 검증 → `expect_empty`.
`test_ready_empty_pipe_first_data.cpp`에는 EVENTS가 unread WRITABLE에 POLLOUT을 내되 completion bit를 노출하거나 record를 소비하지 않는다는 검증이 추가됐다.

보강 제안:

1. **EVENTS 단독 경로**. 현재 시험은 EVENTS를 읽기 전에 언제나 `zlink_poll`이 먼저 command를 적용한다. 그래서 EVENTS 자신이 `activate_read`를 적용하고 POLLIN을 내는지는 검증하지 않는다. 그런데 이 경로가 Issue의 실제 사용(ZLINK_OPT_FD + EVENTS 외부 event loop)이다. 결정적 설계는 다음과 같다. 송신 뒤 `ZLINK_OPT_FD`로 fd를 얻는다. `while (!(events (s) & ZLINK_POLLIN)) { zlink_pollitem_t it = {NULL, fd, ZLINK_POLLIN, 0}; TEST_ASSERT_EQUAL_INT (1, zlink_poll (&it, 1, wait_ms, NULL)); }`처럼, EVENTS가 POLLIN을 낼 때까지 FD readable만 기다린다. 각 대기는 `wait_ms`로 막혀 있다. 이 loop 안에서 socket 대상 `zlink_poll`은 부르지 않는다. 적용 대상은 위 표의 종류 전부다.
2. ROUTER case에서 `rid != NULL`과 송신 peer의 RID 일치를 확인한다. prefetch한 `_prefetched_id`가 recv까지 보존되는지 보는 검증이다. 현재는 payload와 token만 확인한다.

참고 시험: `core/tests/integration/test_wake_invariants.cpp`, `test_two_poller_wake.cpp`, `test_ready_empty_pipe_first_data.cpp`, `test_option_events.cpp`. 규칙 근거는 `core/tests/README.md:17–22`(Interface Boundary), `CONTRIBUTING.ko.md:90`(§5 테스트 규칙)이다.

## 7. 영향

- **hot path**: `10-hot-path.ko.md:33–43` §2 표에 EVENTS(getsockopt) 진입점은 없다. 수정은 표의 함수(`get_events_internal`·`has_in`)를 고치지 않고 호출자 하나를 바꿨다. §3 금지 1–6항(heap 할당, 문자열 identity, table 조회, 무조건 부가 작업, reader를 재우는 미리보기, sleep)은 해당하지 않는다. 7항(poller 밖에서 적용한 command의 신호)은 수정 전후가 같다. 수정 전에도 EVENTS는 `drain_primary_signaler`와 `process_commands (…, true)`를 했다(§8 참조).
- **hotpath_gate**: 측정하는 5개 cell(`10-hot-path.ko.md:117–123`)이 EVENTS를 부르지 않으므로 수치 영향은 없을 것으로 본다. §5.1 의무는 “표의 함수를 고치는 변경”이므로 엄밀히는 대상이 아니다. 다만 EVENTS가 primary signaler를 소비하므로 `test_wake_invariant*` 10회 반복은 실행할 가치가 있다(실행하지 않음).
- **binding**: 바뀔 것이 없다. Java `bindings/java/src/main/java/systems/zlink/contracts/sockets/SocketOptionFacades/CommonSocketOptions.java:375–377`은 int를 그대로 돌려준다. .NET `bindings/dotnet/src/Zlink/Runtime/Sockets/SocketOptionFacades.Runtime.cs:152`는 `PollEventFlags`로 cast만 한다. **C++ binding에는 `events()` accessor가 없다**(`bindings/cpp`에서 `ZLINK_OPT_EVENTS`는 `include/zlink_enum.h:112` enum뿐). 모두 Core에 포함된 값을 그대로 노출하므로, bundled Core가 수정 포함 버전으로 오르면 해소된다.

## 8. 변경 분류

**B (기존 결함)**. `05-polling.ko.md:57–58`은 EVENTS가 raw socket의 POLLIN·POLLOUT을 poller 표와 같은 판정으로 돌려준다고 정한다. 1.19.0 코드는 POLLIN을 빠뜨렸다. 이 조항은 `2c060ffdb5`(2026-10-14)에서 들어왔다. 그 전에는 `socket/README`의 “이벤트 상태 bitmask” 한 줄뿐이어서 처음에는 D(spec gap)였다. 하지만 스펙이 먼저 정해졌고, 같은 판정을 재사용해 맞췄으므로 현재 기준으로는 B다. 우회(C) 요소는 없다.

### 별도 진단 후보 (이번 수정 범위 밖, 가설·미검증)

`05-polling.ko.md:88–89`는 “public poller의 notification descriptor는 그 poller만 소비한다”고 정한다.
그런데 socket에 처음 등록한 `zlink_poller`는 mailbox의 primary signaler를 소유한다(`socket_poller.cpp:303–304`).
EVENTS는 그 signaler를 비우고(`socket_base_api.cpp:780`), `primary_poller_drain_=true`로 command를 적용한다.
이때 `mailbox->signal_pollers`(`socket_base_lifecycle.cpp:540`, `mailbox.cpp:474–479`)는 primary signaler에 다시 신호하지 않는다.
따라서 다른 thread가 EVENTS를 부르는 동안 같은 socket에서 `zlink_poller_wait`가 primary로 대기하면,
readiness 전이가 EVENTS에 흡수되어 poller가 timeout까지 잠들 수 있다(lost wake).
이 동작은 1.19.0부터 있던 것이고 `1ad2b0439c`가 만든 것이 아니다. FD+EVENTS 사용과 zlink_poller를 함께 쓰는 경우의 계약은 `ZLINK_OPT_FD`(`socket/README.ko.md:360`)에도 정해져 있지 않다.

## 9. BLOCKERS

#1695에는 없다. 비차단 spec gap 2건:

- `core/doc/spec/core/socket/README.ko.md:360` — `ZLINK_OPT_FD`가 어떤 descriptor인지, EVENTS와 edge를 어떻게 나누는지, `zlink_poller` 등록과 함께 쓸 때 누가 소비하는지 정하지 않는다(§8 후보와 연결).
- `core/doc/spec/core/05-polling.ko.md:30` — type별 POLLIN 의미의 소유 문서가 DEALER·ROUTER뿐이다. PAIR·SUB·XSUB·XPUB·PUB는 §3 표의 일반 문장에 기댄다.
