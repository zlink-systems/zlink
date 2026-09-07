# G-11 — 메시지당 잠금 15쌍의 출처와 계약 안에서의 축소 설계 (분석 전용)

> 2026-09-07. worktree `~/project/zlink-work/g11` (detached `238695fd8a`). **소스 수정·커밋 없음.**
> 수치는 G-1이 같은 축소 셀(STREAM zlink, CCU 20 / 1024 B / `--io-threads 4`, dev `-g` lib, callgrind)에서
> 잰 dev 덤프를 재사용한다(브리프가 허용). before 17.396 / after(G-1 두 건 제거) **15.122** `pthread_mutex_lock`/msg.
> 새 측정을 하지 않은 이유: 변경이 없으므로 같은 커널·같은 셀에서 같은 수가 나오고, 11 GB 머신에서
> callgrind 판 하나가 ~25 분을 먹는다. 대신 시간을 출처 추적(§1)과 lane 모형 설계(§4)에 썼다.
> libzmq 인용은 저장소 자체의 베이스 커밋 `e01e8afe31`(libzmq v4.3.5 원본 트리)에서 가져왔다 —
> `bindings/c/bench/with_zmq/libzmq`에는 바이너리 dist만 있고 소스가 없다.

## 1. 잠금별 출처 표 (15.12/msg, G-1 제거분 반영)

| # | 잠금 (획득 지점) | 호출/msg | 지키는 불변식 | 도입 커밋·날짜 | 서브시스템 | thread-safe 계약 때문인가 | 두 번째 writer/reader는 누구인가 |
|---|---|---:|---|---|---|---|---|
| 1 | `mailbox_t::send` — `_sync` | 2.003 | 명령 큐(cpipe) + signaler 상태의 원자적 삽입 | libzmq 상속(`529c08df30` 이전) | 명령 핸드오프 | **y (그리고 MPSC 자체)** | 임의의 스레드가 producer, owner 하나가 consumer — 진짜 다중 producer. zmq도 같은 잠금(1.73) |
| 2 | `socket_base_t::process_commands` — `public_api_sync` + `command_owner_sync` (+명령당 `receive.sync`) | 1.467 | 명령 배치 단독 소유, 공개 API와 명령 적용의 배타 | `public_api_sync` `a819ea3ac6`(2026-03-15, thread-safe 계약)·`command_owner_sync` `3ef4d09a37`(2026-08-15, byte-HWM)·`receive.sync` `5d2bf1e84f`(2026-08-23) | API 직렬화 3겹 | **y이나 3겹은 아니다** — 계약이 요구하는 것은 "공개 연산 1개당 배타" 하나 | 다른 앱 스레드(계약) + 명령 owner 턴. **같은 불변식이 3개 잠금으로 쪼개져 있다** |
| 3 | `socket_base_t::read_activated` — `receive.sync` | 0.999 | 수신 파티션/활성 pipe 집합(POLLIN level의 근거) | `3ef4d09a37`(2026-08-15) | 수신 파티션·lane 재분류 | n — #2와 같은 상태 클러스터 | 명령 owner 턴 ↔ 앱 recv. #2와 **동일한 배타 영역** |
| 4 | `socket_base_t::has_in` — `receive.sync` | 0.278 | 같음 | 같음 | 같음 | n | 같음 (#2/#3와 한 덩어리) |
| 5 | `pipe_t::write` — `_out_sync` | 1.000 | 송신 상태 클러스터(`_out_pipe`,`_state`,`_out_active`,`_peers_msgs_read/bytes_read`) | `01a5c6639b`(2026-03-16 thread-safe) + byte credit 필드 `784e504384`(2026-07-30) | pipe 송신 | **부분적** | ① 다른 앱 스레드(계약) ② `process_activate_write`의 credit 회수(명령 owner) ③ `terminate`/`detach_peer_link` |
| 6 | `pipe_t::flush` — `_out_sync` | 1.001 | 같음 + activate_read 발신 판정 | 같음 | pipe 송신 | 같음 | 같음 |
| 7 | `pipe_t::write_single_message_and_flush_…` — `_out_sync` | 1.000 | 같음 | `f3be895b3f`(2026-09-03) | pipe 송신(STREAM) | 같음 | 같음 |
| 8 | `stream_t::xsend_routed` — `route_shard_t::sync`(64 샤드) | 1.000 | RID→pipe 표 | `2b090a2449`(2026-04-16) | route shard | n | 읽기는 앱 스레드 N개, 쓰기는 **토폴로지 변경 명령뿐**. 핫패스에 두 번째 writer 없음 |
| 9 | `mailbox_t::recv` / `reschedule_if_needed` / `signal_pollers` — `_sync` | 0.278/0.227/0.188 | mailbox 상태 | libzmq 상속 + `_sync` 재사용 | 명령 핸드오프 | y(#1과 같은 객체) | #1과 같음 |
| 10 | `zlink_poller_wait` / `poller_acquire` — 공개 poller `std::mutex` | 0.420/0.140 | poller 핸들 표(C1 조회 레지스트리) | poller 공개 API | 공개 poller | n | 앱 스레드 N개가 **읽기** 위주 |
| 11 | boost.asio scheduler·reactor 내부 | 4.033 | asio 내부 | 외부 라이브러리 | reactor 선택 | n | Core 밖 |
| 12 | (제거됨) `deferred_socket_msg_termination_sync` | 0 (was 1.004) | — | `88cd8557d7`(2026-08-30) | 지연 종료 | n | G-1이 제거 |
| 13 | (제거됨) `refresh_application_hwm_if_drained` — ctx `_sync` | 0 (was 0.499) | — | `f9328c7dec`(2026-08-22) | auto-HWM 회계 | n | G-1이 제거 |
| — | 벤치 하네스 자체 | 1.000 | — | — | — | — | — |

**Core 잠금 합계 = 10.09/msg**(위에서 asio 4.03과 하네스 1.0 제외), 그중 **계약이 실제로 요구하는 것은 #1의 2.0과
"공개 연산당 1개"의 1개뿐 — 대략 3쌍**이다. 나머지 ~7쌍은 2026-07-30 ~ 09-03 사이에 byte-HWM lane·flow-state·
completion·route shard가 각자 자기 잠금을 들고 핫패스에 올라온 결과다(도입 커밋 열이 그대로 증거다).
2026-03 thread-safe 계약 커밋이 도입한 잠금은 `_out_sync`와 `public_api_sync` **둘뿐**이었다 — D-B176(사용자 지적)이 맞다.

## 2. libzmq thread-safe 소켓이 연산당 1쌍으로 되는 메커니즘 (3줄)

1. **소켓 상태 전체가 하나의 잠금 뒤에 있다.** `socket_base.hpp:191`의 `mutex_t _sync` 하나를 모든 공개 연산이
   잡는다(`socket_base.cpp:1206` `send`, `:1294` `recv`, `:1138` `term_endpoint` … 전부 `scoped_optional_lock_t sync_lock (_thread_safe ? &_sync : NULL)`).
   명령 적용(`process_commands`)은 **그 잠금 안에서** 호출되므로 명령용 두 번째 잠금이 없다.
2. **pipe에는 뮤텍스가 아예 없다** — `src/pipe.cpp`에 `mutex`/`lock` 문자열이 0건이다. 양 끝이 각각 정확히 한
   스레드에 고정된 SPSC이고, 그 사이는 `ypipe_t::flush()`의 CAS 하나(`_c.cas(_w,_f)`)와 원자 카운터뿐이다.
3. 남는 유일한 공유 지점은 스레드 간 명령 전달용 `mailbox_t::_sync`(MPSC 삽입점)이고, 그것이 측정된 1.73/msg다.

## 3. 프레임워크 lane 모형으로 본 Core 소켓 (06-state-ownership §4·§5, 07 §6.3)

- **삽입점 잠금은 이미 있다.** `mailbox_t::_sync`가 07 §6.3의 "수용량 판정·순서 발급·삽입이 한 구간"이고,
  `mailbox.cpp`의 `schedule_if_needed_unlocked`가 §6.5의 "제출자가 배출 loop를 깨운다"다. **#1은 정당하다.**
- **lane도 이미 있다.** 명령 owner 턴(`process_commands`의 `command_owner_sync` 구간)이 소켓 state lane이다.
- **위반은 하나다: 소켓 상태가 lane 안에 다 들어가 있지 않다.** 공개 send/recv가 호출 스레드에서 같은 상태를
  직접 만지므로 두 번째 배타 장치(`public_api_sync`)가 필요해지고, 수신 파티션이 세 번째(`receive.sync`)로 갈라졌다.
  06 §4의 "한 컴포넌트에 C2가 섞이면 C2가 이긴다 / 부분적으로만 lane으로 옮기면 그 경계에서 교차 불변식 위반이
  다시 나타난다"에 정확히 걸린다. `_out_sync`도 같은 형태다 — pipe 송신 클러스터는 앱 쪽 writer와 lane 쪽
  credit 회수가 **같은 불변식을 두 실행 단위에서** 만진다.
- **분류.** route shard(#8)와 공개 poller 표(#10)는 **C1**(단일 map, 조회·추가·삭제, 걸친 불변식 없음) →
  뮤텍스가 아니라 lock-free 조회여야 한다. credit 회계(`_peers_msgs_read/_peers_bytes_read`)는 **C3**(단조 카운터) →
  release/acquire 원자. 소켓 수신·송신 상태와 `_out_active`/`_state` 전이는 **C2** → 하나의 lane이 소유.
- 06 §4의 "같은 불변식 → 하나의 lane"은 **현재 하나인 잠금을 쪼개는 것도 금지**한다. 아래 두 안은 모두
  합치는 방향이며, 새 잠금·새 플래그·새 옵션을 도입하지 않는다.

## 4. 설계 2안 (계약 불변, 새 옵션 없음)

### A. zmq형 — 공개 연산당 잠금 하나로 전부 흡수

`public_api_sync`+`command_owner_sync`+`receive.sync`를 **하나의 socket sync**로 합치고, 명령 적용을 그 잠금
안에서 수행한다. 소켓이 소유한 pipe의 `_out_sync`와 route shard 잠금도 그 아래로 흡수한다(= 잠금을 없앤다).

- 예상: Core **10.09 → 약 3.0/msg**(mailbox 2.0 + socket sync 1.0), 전체 15.12 → **≈ 8.0**. Ir −450 내외.
- **깨지는 것**: (i) 블로킹 recv/send가 잠금을 쥔 채 CV를 기다리게 되므로 잠금 해제-재획득 프로토콜을
  다시 설계해야 한다(zmq는 `_sync`를 놓고 signaler에서 기다린다 — Core는 `receive.progress_cv`·
  `submit_progress.cv`·`blocking_send_wait` 세 채널이 이미 이 잠금들에 묶여 있다).
  (ii) inproc은 한 스레드가 두 소켓의 pipe를 만진다(`stream.cpp:1260` `identify_peer` →
  `peer->set_router_socket_routing_id`, `pipe.cpp:431` `detach_peer_link`). `_out_sync`를 소켓 잠금으로
  대체하면 **소켓 A 잠금 → 소켓 B 잠금** 순서가 생겨 역전 데드락 후보가 된다(오늘은 `_out_sync`를
  놓고 peer를 잡아 회피한다). (iii) `ZLINK_BUILD_TESTS`의 `receive.sync.try_lock()` 경합 프로브 의미가 바뀐다.
- 위험: **높음**. 반경: `socket_base*.cpp`(≈6 파일), `socket_lifecycle_runtime.cpp`, `pipe.cpp`, `stream.cpp` — 수천 행.

### B. lane + SPSC — 공개 연산당 삽입점 잠금 1개, 앱↔I/O 사이는 전부 단일 writer (**권장**)

원칙: **"큐에 넣고 큐에서 뺀다"에는 잠금이 필요 없다. 잠금은 (a) 앱 스레드가 여럿인 삽입점과
(b) 한 lane이 소유하는 C2 상태에만 둔다.**

1. **소켓 잠금 3겹 → 1겹** (#2·#3·#4, −1.7/msg). `public_api_sync`·`command_owner_sync`·`receive.sync`는
   같은 C2 클러스터를 지키므로 하나로 합친다. A와 달리 **명령 적용을 잠금 안으로 끌어들이지 않는다** —
   지금의 획득 순서(API → command owner → receive)를 하나로 접기만 하므로 CV 프로토콜과 블로킹 경계가
   그대로 남는다. 이것이 A의 이득 대부분을 위험 없이 가져오는 부분이다.
2. **inbound pipe(session→socket)의 `_out_sync` 제거** (#5·#6 중 2.0/msg). 이 endpoint의 송신 클러스터
   writer는 engine I/O 스레드 **하나뿐**이다. 잠금이 남아 있는 이유는 `process_activate_write`가 같은 클러스터의
   `_peers_msgs_read`/`_peers_bytes_read`를 명령 owner 턴에서 쓰기 때문이다 → 이 둘을 **C3 단조 원자**로
   바꿔 reader 쪽이 release로 발행하고 writer 쪽이 acquire로 읽는다(ypipe가 데이터에 대해 이미 하는 것과 같은 형태).
   `_out_active` false→true 전이는 CAS 한 번으로 단독 승자를 정해 `write_activated` 발신이 정확히 1회로 남게 한다.
   `terminate`/`detach_peer_link`는 이미 명령 경로가 있으므로 lane으로 보낸다.
3. **outbound pipe(app→session)의 `_out_sync`** (#7, 1.0/msg)는 **삭제가 아니라 1번의 소켓 잠금으로 흡수**한다
   (앱 스레드가 여럿이라는 계약이 여기서만 진짜로 걸린다). 순증 0.
4. **route shard(#8, 1.0/msg)**: C1이므로 조회를 무잠금으로. 샤드마다 `std::atomic<const routes_snapshot_t*>`
   하나를 두고 토폴로지 변경(명령 경로) 때만 스냅샷을 교체한다. 핫패스 조회는 acquire 로드 1회.
5. **공개 poller 표(#10, 0.56/msg)**: 같은 C1 처리가 가능하지만 이득이 작아 후순위.

- **살아남는 잠금 쌍**: mailbox `_sync` 2.0(삽입점, zmq와 동일) + mailbox recv/signal 0.69 + 소켓 잠금 ~1.2
  + poller 0.56 ≈ **4.5/msg**(Core), asio 4.03 포함 전체 **≈ 8.5/msg**. 즉 **15.1 → 8.5, −6.6쌍**.
- Ir 예상: 쌍당 ≈ 65 Ir(G-A: lock 648 Ir/17.34 + unlock 490 Ir/17.34) → **−430 Ir/msg**, STREAM 9,808 대비 −4.4 %.
  다만 Ir은 `lock cmpxchg`의 캐시라인 왕복을 과소평가하므로 실제 처리량 이득은 4 %(Ir 기준)와
  13 %(§7.1의 "1쌍 ≈ 1.5~2 %" 기준) 사이로 본다.
- 반경: `socket_runtime.hpp`/`socket_lifecycle_runtime.cpp`/`socket_base_lifecycle.cpp`(1), `pipe.hpp`/`pipe.cpp`(2·3),
  `stream.hpp`/`stream.cpp`(4). 단계마다 독립적으로 되돌릴 수 있다.
- 위험: **중**. 잠금 순서 역전 없음(잠금이 늘지 않고 줄기만 한다), 블로킹 대기 프로토콜 불변.
  TSan 필수(2번은 잠금→원자 전환이다).

## 5. 재확인해야 할 스펙 문장 (그대로 유지되어야 함)

- **04-thread-safety §4.1** — "어떤 스레드든 소켓을 사용할 수 있고 공개 연산은 직렬화된다": B-1·B-3이
  공개 연산당 배타를 **하나의** 잠금으로 유지하므로 문장 그대로. A도 유지되나 블로킹 경계 재설계가 붙는다.
- **05-polling** — POLLIN level 유지 조건, WRITABLE wake의 순서·조건: B-2의 `_out_active` CAS가
  `write_activated`를 전이당 정확히 1회로 유지해야 한다. **여기가 B의 유일한 계약 위험 지점**이며
  lost-wake 테스트(`until-fail:10`)로 게이트해야 한다.
- **06-auto-hwm** — charge는 frame write에서 시작해 dequeue에서 끝난다(§`434-435`,`467`). B-2는 회계
  **값**을 바꾸지 않는다. 다만 writer가 drain을 관측하는 **시점**이 activate_write 명령 적용에서
  release/acquire 발행 시점으로 앞당겨진다. 스펙이 관측 시점을 명령 경계에 못박은 문장은 찾지 못했고
  오히려 "dequeue에서 종료"에 더 가까워진다 — 그러나 회계 시점을 고정한 문장이 발견되면 그 순간 **D**다.

## 6. 결론

"잠금 때문에 느리다"는 **부분적으로 맞다**. 잠금은 STREAM 셀에서 1,138 Ir/msg(전체의 11.6 %)이고
zlink−zmq 격차 2,823 Ir/msg의 **약 40 %**를 차지하는 단일 최대 공통 비용이다. 그러나 thread-safe 계약이
요구하는 것은 §1 표 기준 **약 3쌍**(mailbox 삽입점 2 + 공개 연산당 1)이고, 나머지 ~7쌍은 2026-07-30 이후
byte-HWM lane·flow-state·completion·route shard가 각자 잠금을 들고 들어온 결과다 — **계약을 한 글자도 바꾸지 않고
15.1 → 8.5쌍(Core 10.1 → 4.5)까지 줄일 수 있다.** 권장안은 **B**다. A는 이득이 1.5쌍 더 클 뿐인데
소켓 간 잠금 순서 역전과 블로킹 대기 재설계를 함께 요구한다.

**job 분할(예상 이득 순, 각각 독립 게이트):**

| job | 내용 | 예상 −쌍/msg | 위험 | 검증 |
|---|---|---:|---|---|
| G-11a | 소켓 3겹 잠금(`public_api_sync`+`command_owner_sync`+`receive.sync`) → 1겹 | −1.7 | 중 | `ctest -R socket_base\|stream\|pair\|router`, TSan 1회, 경합 프로브 기대값 확인 |
| G-11b | inbound pipe credit(`_peers_msgs_read/bytes_read`)를 C3 원자로, `_out_active` 전이 CAS화 → session 쪽 `_out_sync` 제거 | −2.0 | 중 | lost-wake `until-fail:10`, TSan 필수, `ctest -R pipe\|wake\|poll` |
| G-11c | route shard 조회 무잠금 스냅샷(C1) | −1.0 | 낮 | `ctest -R stream\|router`, with_stream 1회 |
| G-11d | app→session `_out_sync`를 G-11a의 소켓 잠금으로 흡수 | −1.0 | 중(G-11a 선행) | 위와 동일 |
| G-11e | 공개 poller 핸들 표 C1화 | −0.56 | 낮 | `ctest -R poller` |
| (별도 트랙) | boost.asio 4.03/msg — reactor 사용 방식 | −? | — | G-7과 합침 |

**변경 분류: B(기존 결함)** — 계약이 요구하지 않는 잠금이 서브시스템별로 누적된 것이다. A도 C도 아니며,
D 후보는 §5의 06-auto-hwm 관측 시점 한 건뿐이고 현재까지 그것을 고정한 스펙 문장은 찾지 못했다.

## 7. 멈춘 지점

- 재측정하지 않았다(§0 이유). G-11a~e 각 job은 자기 before/after를 같은 축소 셀로 1회씩 떠야 한다.
- 예상 −쌍/msg는 호출 횟수 산술이며, 잠금 제거가 만드는 캐시라인 트래픽 감소는 넣지 않았다(보수적).
- worktree `~/project/zlink-work/g11`은 만들었으나 빌드에 쓰지 않았다(소스 변경 없음). 정리해도 된다.

---

# 부록 A — 감독관 turn 모형 검증 (file:line 근거, 읽기 전용)

> 감독관 관찰 (1)(2)(3)을 코드로 검증했다. **한 곳을 정정**하고, 나머지는 확인됐으며 그 결과 turn 모형의
> 위험도가 §4-B에서 예상한 것보다 **낮다**(step 3이 이미 부분 구현돼 있다).

## A.0 정정 — "turn"은 pthread mutex가 아니다

`public_api_sync_bit`은 원자어 안의 CAS 스핀락이다(`socket_lifecycle_runtime.cpp:391-411` `lock_public_api_sync`,
`:23` 비트 정의, `:137-148` admission CAS에 비트를 함께 싣는 경로, `:364-373` thread_local 소유자 체인).
따라서 **§1의 15.12 `pthread_mutex_lock`/msg에 이 비트는 들어 있지 않다.** "항상 turn을 잡게 한다"는
잠금 카운트를 늘리지 않는다 — 늘어나는 것은 CAS 1회와 경합 시 백오프뿐이다.

또한 **공개 send는 이미 항상 비트를 잡는다**: `begin_public_send_scope`/`begin_complete_send_scope`
(`socket_base_msg.cpp:260-294`)가 둘 다 `needs_sync_=true`로 scope를 연다. `needs_sync_=false`
경로는 `socket_public_api_scope_t`(admission만)뿐이고 flow-state 조회·close 계열에서만 쓴다
(`socket_base_flow_state.cpp:37`, `socket_base_lifecycle.cpp:999,1051,1275`).
즉 감독관 관찰 (2)의 전반부는 **성립하지 않고**, 후반부(명령별 조건부)는 성립한다.

## A.1 (a) `_out_sync`가 지키는 필드 중 turn 보유자 외가 쓰는 것

| 필드 (pipe.hpp) | 이미 원자? | 쓰는 주체 | 판정 |
|---|---|---|---|
| `_peers_msgs_read` (`:818`), `_peers_bytes_read` (`:819`) | 아니오 | **오직** `pipe.cpp:967-970` (`process_activate_write` 안, 단조 max 갱신). 읽기는 `:824,846,1251,1268-1292,1413` 등 다수 | **C3 후보** — 단조 증가 2개. release/acquire 원자로 바꾸면 잠금 없이 같은 값 |
| `_out_active` (`:729`) | 아니오 | `process_activate_write`(`pipe.cpp:2812-2818`, false→true)와 송신 경로(true→false) | **CAS 1회로 전이 단독 승자 확정** — `write_activated` 발신이 전이당 1회로 유지되어야 한다 |
| `_state` (`:836`) | **예** (`std::atomic<lifecycle_state_t>`) | — | 이미 단일 원자 |
| `_in_active`, `_head_reclassify_wake`(`:746` 계열), `_transport_lane_count`, `_peer_weight`, `_transport_pair_application_ready` | **예** | — | S-11이 이미 원자화 |
| `_out_pipe`(`:717`), `_msgs_written`(`:791`), `_bytes_written`(`:793`), `_out_generation`(`:806`), `_out_incomplete_*`, `_pending_*`(`:920-927`), `_transport_pair_write_held`(`:751`) | 아니오 | **송신자 한 쪽만** 쓴다 | 단일 writer — turn(또는 I/O 스레드)이 배타를 준다 |

**핫패스 3회의 소속이 결정적이다.** `pipe_t::write`(1.000)와 `pipe_t::flush`(1.001)는 **session 쪽 endpoint**에서
불린다(`session_base_pipe_io.cpp:129` `_pipe->write`, `:141-142` `session_base_t::flush`, 호출자는
`asio_engine.cpp:1212,1500,1647,1782`) — 그 endpoint의 writer는 **asio I/O 스레드 하나뿐**이고,
그 endpoint로 오는 `activate_write` 명령도 같은 I/O 스레드가 처리한다. **두 번째 writer가 핫패스에 없다.**
`write_single_message_and_flush_no_recursive_hwm_check`(1.000)는 socket 쪽 endpoint이고
`stream_t::xsend_routed`(`stream.cpp:911`)에서 turn 안에서 불린다.

**핫패스 밖의 진짜 교차 접근**(이것 때문에 잠금이 남아 있었다): `pipe.cpp:431` `detach_peer_link`가 peer의
`_out_sync`를, `stream.cpp:1260` `identify_peer`→`peer->set_router_socket_routing_id`가 peer의 `_out_sync`를 잡는다.
둘 다 teardown/identity 경로다 — 명령으로 옮기거나, 그 두 지점 전용의 좁은 잠금으로 남긴다(핫패스 0회).

## A.2 (b) 비트 없이 도는 명령과 그것이 바꾸는 상태

`socket_base_lifecycle.cpp:404-420`:
```
reaper_owns_closed_socket = public_close_requested() && reaper_poller() != NULL
command_path_needs_public_api_sync = !reaper_owns_closed_socket
                                     && ((options.type != ZLINK_CORE_SOCKET_PAIR) || pair_sync_for_this_claim)
```
- **PAIR 소켓의 모든 명령**이 비트 없이 돈다. 예외는 `is_pair_pipe_lifetime_command`로 probe돼
  (`:487-497`) 재시도되는 pipe 생성/종료 명령뿐. 즉 PAIR의 `activate_write`(credit·`_out_active`),
  `activate_read`(수신 파티션), `flow_state`, `peer_weight`가 앱 send와 **동시에** 같은 상태를 만진다 →
  `_out_sync`·`receive.sync`가 그 동시성을 받아내고 있다.
- **reaper가 소유한 close 이후**의 모든 명령. 핫패스 아님.
- 비-PAIR(STREAM/ROUTER/DEALER/PUB/SUB)는 **이미 명령 드레인도 비트를 잡는다.** 따라서 이 소켓들에서
  socket 쪽 endpoint의 `_out_sync`와 `receive.sync`는 **이미 turn과 중복**이며, 남은 근거는 A.1의
  cross-endpoint 접근 2곳과 PAIR뿐이다.

## A.3 (c) 블로킹 대기가 turn/잠금을 쥐고 자는가 — **핵심 위험이 이미 해소돼 있다**

- **블로킹 send**: `wait_for_submit_progress`(`socket_base_lifecycle.cpp:784-791`)가 대기 전후로
  `send_scope_.release_sync_for_retry()` / `reacquire_sync_after_retry()`를 **이미 부른다**.
  즉 "turn을 놓고 잔다"는 프로토콜이 이미 존재한다. 실제 수면은 `progress.cv.wait(&progress.sync, …)`
  (`:766`)로, CV가 `progress.sync`를 놓는다.
- **블로킹 recv**: `socket_base_msg.cpp:735-760`의 루프는 api-sync scope를 **아예 열지 않는다**.
  수면은 `wait_receive_progress`(`socket_base_lifecycle.cpp:1548-1570`)의
  `receive.progress_cv.wait(&receive.sync, timeout)` — CV가 `receive.sync`를 놓는다.
  async 실행기가 없으면 대신 `process_commands(timeout)`으로 mailbox에서 잔다.
- **mailbox 대기**: `mailbox.cpp:276` `_command_wait_cv.wait(&_sync, …)` — 역시 CV가 `_sync`를 놓는다.

⇒ turn 모형에서 새로 만들어야 할 "대기 중 turn 반납"은 **recv 쪽뿐**이고, send 쪽 구현
(`release_sync_for_retry`/`reacquire_sync_after_retry`)을 그대로 재사용한다.

## A.4 (d) turn 모형에서 사라지는 잠금과 남는 잠금

| 사라짐 (근거) | /msg |
|---|---:|
| `process_commands`의 `command_owner_sync` + 명령당 `receive.sync` — turn이 배타를 준다 | 1.467 |
| `read_activated`의 `receive.sync` (`socket_base_api.cpp:1615,1680`) | 0.999 |
| `has_in`의 `receive.sync` | 0.278 |
| `pipe_t::write` `_out_sync` — session endpoint 단일 writer (A.1) | 1.000 |
| `pipe_t::flush` `_out_sync` — 같음 | 1.001 |
| `write_single_message_and_flush_…` `_out_sync` — turn 안 (A.1) | 1.000 |
| `route_shard_t::sync` — C1, 스냅샷 원자 교체 | 1.000 |
| **합계** | **−6.745** |

| 남음 | /msg | 이유 |
|---|---:|---|
| `mailbox_t::send` `_sync` | 2.003 | 07 §6.3 삽입점. 진짜 MPSC. zmq도 동일 |
| `mailbox_t::recv`/`reschedule_if_needed`/`signal_pollers` `_sync` | 0.693 | 같은 객체. 횟수 축소는 G-7 소관 |
| 공개 poller `std::mutex` | 0.560 | C1. 별도 job(G-11e) |
| boost.asio 내부 | 4.033 | Core 밖 |
| **turn 자체** | **0 (pthread 기준)** | 원자어 CAS. send 경로는 이미 지불 중 |
| **합계** | **7.29** | |

**15.12 → 8.29/msg** (Core 10.09 → **3.26**, zmq 1.73과 같은 자릿수).
쌍당 ≈65 Ir(G-A: lock 648/17.34 + unlock 490/17.34) → **−438 Ir/msg = STREAM 9,808의 −4.5 %**,
zlink−zmq 격차 2,823 Ir의 **−15.5 %**. 캐시라인 왕복 감소는 포함하지 않은 보수적 값이다.

## A.5 3단계 job 분할

| step | 내용 | 측정/게이트 | 그대로 유지돼야 하는 스펙 문장 |
|---|---|---|---|
| **1. 명령 드레인이 항상 turn을 잡는다** | `command_path_needs_public_api_sync`(`socket_base_lifecycle.cpp:404-420`)의 PAIR 예외와 reaper 예외를 없애 모든 명령이 비트를 잡게 한다. **잠금을 제거하지 않는다** — 순서와 비용만 확정한다 | 비트 경합률(백오프 진입 횟수)과 PAIR 처리량을 before/after 1회씩. `ctest -R pair\|wake\|poll`, PAIR lost-wake `until-fail:10`. 스핀락을 쥔 채 명령을 적용하므로 **PAIR 활성화 지연이 늘지 않는지**가 유일한 판정 기준 | 04-thread-safety §4.1(공개 연산 직렬화), 05-polling의 POLLIN level 유지, README의 recv wake 조건 |
| **2. 안쪽 잠금을 하나씩 제거** (S-1식 증명 첨부) | 2a `receive.sync`(read_activated·has_in·명령당) → turn. 2b socket endpoint `_out_sync` → turn(+ `detach_peer_link`·`identify_peer` 2곳은 명령화 또는 좁은 teardown 잠금). 2c session endpoint `_out_sync` → 제거하고 `_peers_msgs_read/_peers_bytes_read`를 C3 원자, `_out_active` 전이를 CAS. 2d route shard 스냅샷 | 각 항목마다 "이 잠금이 답하던 질문을 누가 답하는가"를 file:line으로 적고 TSan 1회 + lost-wake `until-fail:10` + `ctest -R pipe\|stream\|router\|dealer\|pair` 5회 | 05-polling: WRITABLE wake가 `_out_active` 전이당 **정확히 1회**. 06-auto-hwm: charge는 frame write에서 시작해 dequeue에서 끝난다(`06-auto-hwm.ko.md:434-435,467`) — 값은 불변, 관측 시점만 앞당겨진다. 시점을 명령 경계에 못박은 문장이 발견되면 그 순간 **D** |
| **3. 블로킹 대기가 turn을 반납** | recv 대기(`socket_base_msg.cpp:735-760` → `wait_receive_progress`)에 send 쪽과 같은 `release_sync_for_retry`/`reacquire_sync_after_retry` 패턴을 적용. step 2에서 recv가 turn 안으로 들어간 뒤에만 필요 | rcvtimeo·DONTWAIT 계약 테스트, 다중 스레드 recv 경합 테스트, lost-wake `until-fail:10` | 04-thread-safety §4.1, 05-polling의 level 규칙, recv wake 조건(README) |

**순서 근거**: step 1은 잠금을 하나도 건드리지 않으므로 되돌리기 쉽고, step 2의 모든 증명이
"turn이 배타를 준다"에 의존하므로 반드시 먼저 확정돼야 한다. step 3은 step 2c 이후에만 의미가 있다
(그 전까지 recv는 turn 밖이다). 세 step 모두 공개 헤더·`libzlink.vers`·계약 테스트 기대값을 건드리지 않는다.
