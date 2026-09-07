# ST-1 — STREAM packet pump 정체: 원인 특정·수정·회귀 테스트

- job: ST-1 (codex 1차 재현 → ST-1b Claude 인계·원인 특정·수정)
- worktree: `/home/hep7hep7/project/zlink-work/st1` (detached `1696ed55e5`, 0.17.2 포함), patch 미커밋
- 대상 결함: `doc/bug/perf/2026-09-08-core-stream-packet-pump-stall.ko.md` (§8 — 0.17.2에서도 재현)
- 변경 분류: **B (기존 결함)** — 공개 계약·옵션·스펙 변경 없음

---

## 1. 결론 (요약)

머신 A가 보고한 "TCP로 도착해 커널 buffer에서 빠져나간 마지막 frame이 packet API로 끝내
반환되지 않는다"의 원인은 pump 경계도 engine 입력 정체도 아니었다.

**한 STREAM socket에서 recv thread와 send thread가 서로 다른 배타 도메인 아래
동일한 `fq_t`(수신 fair-queue)를 동시에 변경한다.** recv thread는 `receive_once_guarded()`의
**lock-free public receive lease**만 쥔 채 `stream_t::xrecv`/packet pump → `_fq.recvpipe()`를
돌리고, send thread는 `socket_base_t::process_commands()`에서 `public_api_sync` +
`command_owner_sync` + `receive.sync`를 쥔 채 `activate_read` command를 적용
(`xread_activated` → `_fq.activated()`)한다. public receive 빠른 경로는 `receive.sync`를 잡지
않으므로 두 주체는 서로 배제되지 않는다.

결과로 `array_t<pipe_t, 1>`의 "배열 위치 ↔ `pipe_t::_array_index`" 불변식이 깨지고
`_fq._active`가 실제보다 작아져 **0에 도달**한다. 그 시점 pipe들은 `_in_active == true`이고
inbound ypipe에는 읽을 데이터가 남아 있는데, `pipe_t::process_activate_read()`는 `_in_active`가
이미 true면 아무 알림 없이 조기 return하므로 **fq를 다시 활성화할 사건이 영원히 오지 않는다**
(영구 wake 유실). 보고서 §5의 `fq.cpp:39 _pipes.empty()` assertion도 같은 손상의 결과다.

수정: **command 적용 구간이 public receive lease를 함께 획득**하도록 했다. hot path
(공개 receive 빠른 경로, stream_tcp send/recv)에는 원자 연산·lock을 하나도 추가하지 않았다.

---

## 2. 재현 조건

C 러너(같은 thread에서 recv→send)는 통과하고 C++ 러너(별도 dispatcher thread)만 실패한다는
보고서의 관찰이 그대로 성립한다.

| 항목 | 값 |
|---|---|
| socket | STREAM, `tcp://127.0.0.1:*`, `ZLINK_STREAM_RECV_MODE_PACKET` |
| thread A | poller(`POLLIN`) + `zlink_stream_recv_packet(DONTWAIT)` pull |
| thread B | A가 queue에 넣은 packet을 `zlink_send_part_rid`로 echo (별도 thread) |
| client | raw TCP N개, 각 M frame 왕복(길이 프레이밍 prefix 6 + header 11 + body 64) |
| RCVHWM/SNDHWM | 4096 (bounded), 0 (unbounded) |

가장 빨리 재현되는 조합(측정):

| clients | M | HWM | body | 결과 |
|---:|---:|---:|---:|---|
| 100 | 100 | 4096 | 64 B | 매회 재현 (recv=sent=echo가 expected보다 수백 개 부족한 상태로 정지) |
| **100** | **40** | **4096** | **64 B** | **매회 재현** — 회귀 테스트가 쓰는 조합 (수정 전 4/4 FAIL) |
| 40 | 40 | 0 | 64 B | 이 조합만으로는 재현되지 않음 (대조군으로 테스트에 유지) |

회귀 테스트: `core/tests/integration/test_stream_concurrent_pull_send.cpp`
(`core/tests/CMakeLists.txt`에 등록, `TIMEOUT 180`). sleep 기반 동기화는 쓰지 않고
timeout은 실패 상한(30 s)으로만 쓴다. 수정 트리에서 2 케이스 합쳐 **0.09 s**에 통과한다.

---

## 3. 덤프 근거 (정체 시점)

라이브러리와 **동일한 컴파일 플래그**(`-DNDEBUG` 포함 — 이걸 맞추기 전에는 내부 구조체
레이아웃이 어긋나 쓰레기 값이 나왔다)로 재현기를 빌드하고, 3 s 무진행 watchdog에서
클라이언트가 아직 붙어 있는 상태의 내부를 덤프했다.

```
STALL recv=8837 sent=8837 echoed=8837 expected=10000 no_data=612
DUMP fq pipes=20 active=0 current=0 packet_q=0 packet_bytes=0
     pending_valid=0 pending_off=0 recv_body=0 progress_epoch=8941 waiters=0 owner=0 drain=0
PIPEALL i=0  ptr=0x…4880 rid=95 idx=0  fq_active=0 sock_state=0 sock_in=1 ypipe_check=1
        sess_written=4930 sock_read=4785 queued=145 out=1 waiting=0
        eng_stop=0 insize=0 pending=0 partial_stage=0
… (같은 모양의 pipe 11개) …
PIPEALL i=17 ptr=0x…b2c0 rid=56 idx=2  ← 같은 pipe 포인터가 _pipes[2]와 _pipes[17]에 중복
DUMP_SUM socket_inactive=9 writer_stopped=0 writer_waiting=0
         engine_stopped=0 engine_buffered=0 engine_bytes=0 partial=0 credit_gap=11310
```

읽는 법과 가설 판정:

| 관측 | 값 | 의미 |
|---|---|---|
| `engine_stopped` / `engine_bytes` / `insize` / `pending` | 0 | engine 입력이 멈춰 있지도, buffer에 잔량이 있지도 않다 |
| `_packet_pending_input_valid` / `_packet_receive_queue` | 0 / 비어 있음 | stream pump가 붙들고 있는 잔여 raw chunk 없음 |
| `writer_stopped` / `writer_waiting` | 0 / 0 | pipe HWM으로 writer가 park된 상태가 아니다 |
| `_fq._active` | **0** | fq가 "활성 pipe 없음"으로 판단 |
| pipe별 `_in_active` / `_in_pipe->check_read()` | **1 / 1** (11개) | pipe는 "깨어 있음"이고 ypipe에는 읽을 데이터(각 145 B)가 있다 |
| `_pipes`의 같은 포인터 중복 | 있음 | `array_t` 색인 부기 손상 |

- **H1(engine 입력이 HWM으로 멈추고 read-resume wake가 다른 thread의 command drain에
  소비됨) 반증** — engine은 멈춰 있지 않고 buffer도 비어 있다. `_out_active=1`,
  `_waiting_for_byte_credit=0`이라 credit 대기도 아니다.
- **H2(pump가 recv thread 진행에만 의존) 반증** — DONTWAIT probe가 빈 결과인 것과
  `_fq._active==0`이 일치한다. pump는 정상이고, fq가 읽을 pipe를 하나도 못 고른다.
- **H3(종료 부근 `fq.cpp:39` assertion)은 별개가 아니라 같은 원인** — 수정 전 트리에서
  회귀 테스트를 돌리면 정체 실패와 함께 그 assertion이 같이 재현되고, 수정 후에는 둘 다
  사라진다.

### 3.1 동시 진입 증명

`fq_t`에 (a) 위치↔`_array_index` 검증기와 (b) 재진입 감지기를 임시로 넣고
(`ZLINK_FQ_DEBUG`, 진단 후 되돌림) 돌린 결과:

```
FQBAD read_miss i=79 idx=20 size=84 active=20        ← 위치↔색인 불변식 파손
FQCONCURRENT op=recvpipe   tid=A other_op=activated  tid=B
FQCONCURRENT op=activated  tid=B other_op=recvpipe   tid=A
```

| 동시 진입 쌍 | 1회 실행 관측 횟수 |
|---|---:|
| `recvpipe` ↔ `activated` | 217 + 182 |
| `deactivate_current_after_read_miss` ↔ `activated` | 33 + 15 |
| `recvpipe`/`read_miss` ↔ `pipe_terminated` | 5 |
| `attach` ↔ `recvpipe` | 2 |

tid A는 recv thread, tid B는 send thread다. 즉 fq는 두 thread가 동시에 변경한다.

---

## 4. 원인 (파일:행)

동기화 경계 자체가 원인이므로 "한 줄"이 아니라 **두 경로가 만나는 지점**이 원인이다.

| # | 위치 | 역할 |
|---|---|---|
| 1 | `core/src/runtime/sockets/common/socket_base_msg.cpp:53-97` `receive_once_guarded()` | public receive는 `try_acquire_public_receive_lease()`(CAS)만 잡고 `receive.sync`는 잡지 않는다 — "Ordinary single-frame receive keeps the lock-free public fast path" |
| 2 | `core/src/runtime/sockets/common/socket_base_lifecycle.cpp:554-558` (수정 전) | command 적용은 `scoped_lock_t receive_owner (receive.sync)` 아래에서만 한다 — public receive lease는 보지 않는다 |
| 3 | `core/src/runtime/sockets/stream/stream.cpp:1024-1026`, `:623`, `:1045-1050` | recv/pump/`xhas_in`이 lease 아래에서 `_fq`를 변경 |
| 4 | `core/src/runtime/sockets/stream/stream.cpp:751` `xread_activated` → `core/src/runtime/sockets/internal/fq.cpp:174-190` `fq_t::activated()` | command 적용이 같은 `_fq`를 변경 |
| 5 | `core/src/utils/array.hpp:66-96` `push_back/erase/swap` | 위치와 `_array_index`를 비원자적으로 함께 갱신 — 동시 변경 시 파손 |
| 6 | `core/src/runtime/core/pipe.cpp:2838-2842` `process_activate_read()` | `_in_active`가 이미 true면 `read_activated()`를 보내지 않는다 → fq가 비활성인 채로 굳으면 회복 경로가 없다 |

스펙 대조 — `core/doc/spec/core/systems/11-synchronization-model.ko.md`:

- §3.1: "command 적용은 socket type이나 command 종류에 관계없이 turn 안에서 한다.
  **'자주 실행되니 lock 없이 둔다'는 예외는 두지 않는다.**",
  내부 확인 조건 "socket의 C2 상태를 읽고 쓰는 코드는 turn을 쥔 실행 주체 하나뿐이다."
- §5 3행: "socket type이나 빈도를 이유로 command를 turn 밖에서 적용한다 → 공개 연산과
  command가 같은 상태를 동시에 만진다."

수정 전 코드는 **공개 receive 쪽이 turn 밖에 있었다**는 점만 다를 뿐 같은 위반이다.
`/tmp/mp2-tsan.supp`에 `race:receive_once_guarded`·`race:check_read` suppression이 이미
있었다는 사실도 이 race가 전부터 관측됐음을 뒷받침한다.

---

## 5. 수정

변경 파일 2개(+테스트 등록).

### 5.1 `core/src/runtime/sockets/common/socket_runtime.hpp`

`socket_receive_runtime_t`에 `try_acquire_receive_owner_for_commands()`를 추가했다
(비공개 내부 헤더). 이미 있는 `receive_owner` 상태어(available / public / async)를 그대로
쓰고, 한 번만 CAS를 시도해 결과를 3가지(획득 / 이미 async 실행자가 보유 / public 시도가
보유 중)로 알린다. 새 상태·플래그·옵션은 없다.

### 5.2 `core/src/runtime/sockets/common/socket_base_lifecycle.cpp`

command 1건 적용 구간을

```c++
scoped_lock_t receive_owner (receive.sync);      // 이전
socket_command_receive_turn_t receive_owner (receive);   // 이후
```

로 바꿨다. 새 scope는 **`sync` → (실패 시 놓고 재시도) receive lease** 순서로 잡는다.

- lease를 먼저 잡고 `sync`를 나중에 잡으면, 같은 순서로 잡는 whole-record public receive
  (`socket_receive_record_scope_t`)와 교착한다.
- `sync`를 잡은 채 lease를 spin으로 기다리면 같은 교착이 된다.
- 그래서 `sync`를 잡고 lease 획득을 1회 시도해, public 시도가 lease를 쥐고 있으면 `sync`를
  **놓고** yield 후 재시도한다. lease만 쥔 lock-free receive 시도는 `sync`를 절대 요구하지
  않으므로 이 재시도는 짧고, 오래 열려 있는 record transaction에 대해서는 예전처럼 `sync`
  에서 잠들어 기다린다(CPU 소모 없음).
- async mailbox 실행자가 이미 `receive_owner_async`를 보유 중이면 획득하지 않고 해제도
  하지 않는다.

### 5.3 설계 비교

| 안 | 내용 | 판단 |
|---|---|---|
| A | 공개 receive 빠른 경로가 `receive.sync`를 잡는다 | **기각** — message마다 lock. §4·§5 1행 위반, hot path 회귀 |
| B (채택) | command 적용이 이미 존재하는 receive lease를 함께 잡는다 | 새 상태·규칙 0개, hot path 비용 0. §3.1의 "turn 하나" 그대로 |
| C | fq만 별도 lock으로 보호 | **기각** — 같은 불변식에 lock을 하나 더 나눔(§5 2행), 다른 socket type의 같은 경계는 그대로 남음 |
| D | `process_activate_read`가 `_in_active`가 true여도 항상 `read_activated`를 보낸다 | **기각** — 손상된 색인을 우회할 뿐 근본 race가 남고, hot path에 불필요한 wake를 늘린다(C 우회) |

---

## 6. hot path 영향 (stream_tcp send/recv)

성능 측정은 하지 않았다(attrib-0172 job과 충돌). 코드로 서술한다.

| 경로 | 추가된 것 |
|---|---|
| public recv (`zlink_stream_recv_packet` → `receive_once_guarded` → `xrecv`/pump → `_fq.recvpipe`) | **없음.** lease CAS는 이전에도 있었고 그대로다. `receive.sync`를 새로 잡지 않는다 |
| public send (`zlink_send_part_rid` → `xsend_routed`) | **없음.** 송신 자체 경로는 그대로다 |
| `process_commands()`의 command **없는** 통과(throttle·skip 경로) | **없음.** 새 코드는 command 1건을 실제로 적용하는 블록 안에만 있다 |
| `process_commands()`의 command **있는** 적용 1건 | CAS 1회(획득) + release store 1회(해제)가 기존 `receive.sync` lock/unlock 옆에 붙는다. 경합 시 `sync` unlock + `yield` + 재시도 |
| pipe/engine/session I/O thread | **없음** |

즉 message당 추가 원자 연산은 **command가 실제로 적용될 때만** 2회이고, 그 자리는 이미
mutex 2개(`command_owner_sync`, `receive.sync`)를 잡는 자리다.

---

## 7. 검증

| 항목 | 명령 | 결과 |
|---|---|---|
| dev 빌드 | `JOBS=4 scripts/build-core.sh dev` | 성공 (RelWithDebInfo, LTO OFF, tests ON) |
| 재현기(수정 전) | `./repro 100 100 4096 64` ×3 | 3/3 정체 (recv=sent=echo < 10,000) |
| 재현기(수정 후) | 같음 ×3 | 3/3 10,000/10,000 |
| 새 테스트(수정 전) | `bin/test_stream_concurrent_pull_send` ×4 | **4/4 FAIL** (3회는 `fq.cpp:39 _pipes.empty()` assertion 동반) |
| 새 테스트(수정 후) | 같음 ×4 | 4/4 PASS |
| 새 테스트 반복 | `ctest -R '^test_stream_concurrent_pull_send$' --repeat until-fail:20` | PASS (총 2.32 s) |
| 관련 suite | `ctest -R 'stream\|pipe\|wake\|hwm\|flow\|credit\|poll' --repeat until-fail:5` (57개) | **100% passed, 0 failed / 57**, 315.6 s |
| lost-wake (wake-invariant label 4개 포함) | 위 실행에 포함(`test_wake_invariants`, `test_two_poller_wake`, `test_wake_invariant_hwm_lwm_shrink`, `test_wake_invariant_completion_owner` 각 5회) | 전부 Passed |
| TSan — 새 테스트 | `TSAN_OPTIONS=suppressions=/tmp/mp2-tsan.supp setarch x86_64 -R ./bin/test_stream_concurrent_pull_send` | 2/2 PASS, 경고 0 |
| TSan — 새 테스트(suppression **없이**) | 같음, `TSAN_OPTIONS=halt_on_error=0` | 2/2 PASS, **ThreadSanitizer 경고 0** |
| TSan — stream suite | `test_stream_socket` / `test_stream_threadsafe` / `test_stream_packet_progress` | 11/11 OK · 5/5 OK · 8 중 1 FAIL(아래) |
| ASan | 새 테스트 | **미완**(§7.2) |
| 공개 인터페이스 | `git diff --stat -- core/include core/src/libzlink.vers` | **비어 있음** |

### 7.1 TSan `test_stream_packet_progress` — 이번 변경과 무관(대조 확인)

`test_shutdown_during_drain`이 `transport did not queue all fragments`로 FAIL하고 TSan 경고
8건이 함께 나온다. 경고 8건은 전부 같은 쌍이다.

```
zlink::mailbox_t::activate_if_command_pending(bool)   ← main thread, process_commands ← zlink_bind
zlink::mailbox_t::reschedule_if_needed()              ← async mailbox executor (T3/T9)
```

즉 mailbox 스케줄 상태이며, 이번 수정이 만지는 receive 소유권과 무관하다.
**확인 방법**: 같은 TSan 트리에서 `core/src` 변경만 되돌려(patch는 scratch에 보관, `git stash`
미사용) 다시 빌드해 같은 테스트를 돌렸다. 결과가 **완전히 동일**하다 —
`test_shutdown_during_drain:FAIL: transport did not queue all fragments`,
`8 Tests 1 Failures`, `ThreadSanitizer: reported 8 warnings`. 따라서 기존 항목이다.

### 7.2 ASan — 상한 초과로 미완

`core/core/build-asan`을 `-fsanitize=address,undefined`로 구성했고 라이브러리 155/162 TU까지
컴파일한 상태에서 시간 상한(3 h를 크게 초과, 실제 8 h 경과) 때문에 중단했다. 이어서 하려면
worktree에서 아래를 그대로 실행하면 된다(구성은 이미 끝나 있다).

```bash
cd ~/project/zlink-work/st1/core
make -C build-asan -j2 libzlink test_stream_concurrent_pull_send
ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=print_stacktrace=1 \
  ./build-asan/bin/test_stream_concurrent_pull_send
```

TSan이 같은 테스트에서 suppression 없이도 경고 0으로 통과했으므로 이 항목은 남은 확인이지
알려진 실패가 아니다.

### 7.3 재현기(진단용, 저장소 밖)

`/tmp/st1_stream_repro.cpp`(codex 작성)에 라이브러리와 같은 플래그로 빌드하는 스크립트와
정체 시점 덤프·watchdog을 더한 사본이
`<scratch>/st1/repro.cpp`·`build.sh`에 있다. 진단용이며 저장소에 넣지 않았다 — 저장소가
가져야 할 회귀 검사는 §2의 통합 테스트다.

---

## 8. 남은 위험

1. **다른 socket type의 같은 경계** — 이 race는 STREAM 전용이 아니다. `fq_t`를 쓰는
   ROUTER·DEALER·PULL 등도 "한 thread가 recv, 다른 thread가 send"이면 같은 손상이
   가능했다. 수정은 `process_commands()`의 공통 지점에 있으므로 모두 함께 닫힌다.
   반대로 말하면 이 변경의 영향 범위는 모든 socket type이다.
2. **record transaction 중 command 지연** — whole-record receive가 lease+`sync`를 오래
   쥐면 command 적용이 그동안 `sync`에서 대기한다. 수정 전과 같은 대기 형태이므로
   회귀는 아니지만, 그 창이 길어질수록 command 적용이 밀린다.
3. **`array_t` 자체는 여전히 비원자적** — 이번 수정은 "동시에 만지지 않는다"로 해결했다.
   `array_t`에 방어 코드를 넣지 않았으므로, 앞으로 turn 밖에서 fq를 만지는 코드가
   생기면 같은 증상이 다시 난다. §5의 위반표가 그 방지 규칙이다.
4. **TSan suppression** — `/tmp/mp2-tsan.supp`의 `race:receive_once_guarded`·
   `race:check_read`는 이번 결함을 가리고 있었다. 이 수정 이후 그 억제를 걷어낼 수 있는지
   별도로 확인할 가치가 있다(이번 job 범위 밖).
