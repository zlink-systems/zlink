# specgap-0174 — Core 0.17.4 착지와 스펙 정합 검사

검사 기준은 `main` `22e3d3cdb3`이며, 0.17.4 Core 착지 `eb6abfb995`와 그 뒤의
현재 코드다. 지정된 Core 스펙 한·영 22개 파일을 읽었고, 사용자가 적은
`core/doc/spec/core/protocol/02-message`는 저장소의 실제 위치인
`core/doc/spec/core/02-message.{ko,en}.md`로 검사했다. 빌드·실행·테스트·소스 및 스펙
수정은 하지 않았다.

판정 기준은 다음과 같다.

- **정합**: 스펙 문장이 현재 구현과 같은 소유자·순서·관찰 결과를 말한다.
- **불일치(수정 필요)**: 현재 구현이 명시된 문장을 반박하거나, 포괄 목록이라고 읽히는
  문장이 실제 접근자를 누락한다.
- **스펙 침묵**: 구현 세부를 스펙이 고정하지 않는다. 공개 API·wire·소유권·memory
  ordering을 바꾸지 않는 휴리스틱이면 문서화가 불필요하다고 판정했다.

## 요약 판정

| 번호 | 변경 | 판정 | 요약 |
|---:|---|---|---|
| 1 | ALL-1/2 — socket turn 통합 | **11·04 정합 / 10 불일치(수정 필요)** | `11-synchronization-model`의 C2 단일 turn, command drain, pipe 양 끝, wait 재획득은 코드와 일치한다. `04-thread-safety`의 socket/engine 소유권도 일치하고 `receive_owner` 전제 문장은 없다. 다만 `10-hot-path`가 존재하지 않는 `_in_sync`와 per-message에서 제거한 `_out_sync`를 허용 목록에 남겼다. |
| 2 | MAC-4 — 종료를 receive 매 turn 관측 | **불일치(수정 필요)** | 구현은 wait 여부와 무관하게 blocking data receive loop의 매 turn에서 `ETERM`을 반환한다. Socket README와 STREAM 문서는 “blocking wait/while blocked”만 말해 backlog drain 중의 새 보장을 빠뜨린다. |
| 3 | CCU-2~5 — 증분 plan과 deadline 수렴 | **불일치(수정 필요)** | 증분 성공, generation 증가, deadline 단일 소유, full pass의 단일 해제, deferred shrink 비재무장은 정합이다. 그러나 증분 확장 불가 시 attach가 동기 full recalculation으로 fallback하는 코드가 있는데 §2는 이를 절대적으로 부정한다. |
| 4 | SD B2 — 첫 full read에서 2배 성장 | **스펙 침묵** | 1회 full-hit·2배 성장은 Asio engine의 내부 성능 휴리스틱이며 공개 동작, wire, HWM 상한을 바꾸지 않는다. `08-stream`의 initial cap과 `03-io-thread`의 Proactor 소유권만으로 충분하다. |
| 5 | SD-5 — WS bounded 3-buffer gather | **불일치(수정 필요)** | bounded target/max와 Beast write 1회는 정합이다. 큰 body는 output buffer에 복사하지 않고 `[앞, body pointer, 뒤]` sequence로 제출하므로 `01-zmp`의 “output buffer에 모은다”는 틀렸다. `03-io-thread`도 write API를 `async_write_some()` 하나로만 적었다. |
| 6 | D-f — 죽은 STREAM env 3개 삭제 | **삭제 정합 / 목록 문장 불일치(수정 필요)** | 삭제한 3개 이름은 코드에서 더는 읽지 않는다. 현재 8개 항목은 STREAM 전용 접근자와 일치하지만, 제목이 “STREAM runtime variables” 전체 목록처럼 쓰여 generic Asio 접근자 5개와의 경계를 말하지 않는다. 제목을 “STREAM 전용”으로 좁혀야 한다. |
| 7 | RR-1 — 1,025-part reply | **스펙 침묵** | 1,024/1,025는 공개 part-count 제한이 아니라 private-head batch 경계다. 동적 part 배열과 일반 allocation/HWM 오류만 존재하므로 숫자 제한을 스펙에 추가하면 오히려 거짓 계약이 된다. |
| 8 | 한·영 parity | **불일치(수정 필요)** | 0.17.4 관련 변경은 모두 한·영 쌍에 같이 들어갔다. 별도로 `04-thread-safety.en.md` 한 문장이 한국어와 반대 의미가 되는 오기가 있다. |

## 1. ALL-1/2/2b/3

### 1.1 `11-synchronization-model`과 `04-thread-safety`

**정합.** 핵심 스펙 문장은 다음과 같다.

- `11-synchronization-model.ko.md:71`: “socket의 C2 상태 전부. turn 안에서는 그 상태를
  잠그지 않는다.”
- 같은 파일 `:77-82`: application 연산과 command owner가 같은 turn을 사용하고, 모든 command를
  예외 없이 turn 안에서 적용한다.
- 같은 파일 `:101-121`: socket 쪽 pipe end는 turn 보유자가, session 쪽은 I/O thread가
  실행하며, SPSC queue에 별도 mutex를 두지 않고 hot-path write/read/flush가 cold lock을 잡지
  않는다.
- 같은 파일 `:133-152`, `:159-176`: mailbox drain owner handoff와 “waiter 등록은 turn 안,
  대기는 turn 밖, 복귀 뒤 재획득” 순서를 정의한다.
- `04-thread-safety.ko.md:45-54`: socket semantic layer가 routing state를 보호하고 connection
  engine state는 해당 I/O thread가 소유하며 receive mode는 single consumer라고 한다.

영문 대응 문장은 각각 `11-synchronization-model.en.md:76-106,110-142,146-169,177-196`과
`04-thread-safety.en.md:47-57`에 있다.

현재 코드는 `socket_runtime.hpp:282-318`에서 receive·readiness·command를 lifecycle turn 하나로
묶고, `socket_base_lifecycle.cpp:375-435`에서 모든 command batch를 같은 turn 아래 적용한다.
비동기 executor handoff도 `socket_base_lifecycle.cpp:911-943`에서 그 turn을 사용한다.
`socket_base_lifecycle.cpp:1594-1624`는 waiter를 turn 아래 등록하고 turn을 놓은 뒤 CV에서
대기한다. `pipe_write.cpp:453-462`와 `pipe_transport.cpp:44-60`의 steady-state write/flush에는
per-message `_out_sync`가 없다. 따라서 §3.1·§3.2·§3.3·§3.4는 현재 코드와 일치한다.

지정된 스펙 전체에서 `receive_owner`, `command_owner_sync`, receive partition lock을 전제하는
문장은 검색되지 않았다. §3.1의 “command owner”는 별도 lock/word 이름이 아니라 현재 turn을
잡는 실행 주체의 역할명이라서 잔존 0.17.3 전제가 아니다. `04-thread-safety.ko.md:56-60`의
public API guard는 handle pin과 close state만 관리한다는 문장도 맞다. 실제 turn은 별도
`socket_public_api_lock_scope_t`가 `public_api_state`의 turn bit를 조작한다
(`socket_runtime.hpp:709-775`).

ALL-2b/3의 WS masking·decoder recycle block은 allocator/transport 내부 선택이다.
`05-connection-memory.ko.md:93-104`와 영문 `:95-107`은 monitor가 allocator와 kernel overhead
전체를 측정하지 않는다고 명시하며, `01-zmp`의 wire byte·payload ownership은 바뀌지 않았다.
따라서 이 부분의 추가 문서화는 불필요하다.

### 1.2 수정 필요 문장 — `10-hot-path`

1. `core/doc/spec/core/systems/10-hot-path.ko.md:68-69`
   - 현재 문장: “허용되는 것: pipe 자신의 `_in_sync`·`_out_sync`, atomic load/store, 고정 크기
     스택 배열, 이미 잡고 있는 send/recv scope.”
   - 제안 문장: “허용되는 것: atomic load/store, 고정 크기 스택 배열, 이미 잡고 있는
     send/recv scope와 endpoint의 기존 C2 owner/turn. `_out_sync`는 §11의 cold path에서만
     사용하며 per-message write·read·flush는 이를 잡지 않는다.”

2. `core/doc/spec/core/systems/10-hot-path.en.md:75-76`
   - 현재 문장: “Allowed: the pipe's own `_in_sync` / `_out_sync`, atomic loads and stores,
     fixed-size stack arrays, and the send/recv scope already held.”
   - 제안 문장: “Allowed: atomic loads and stores, fixed-size stack arrays, the send/recv
     scope already held, and the endpoint's established C2 owner/turn. `_out_sync` is used
     only on the cold paths defined by §11; per-message write, read, and flush do not take it.”

근거는 `pipe.hpp:735-765,957`(현재 C2 owner와 남은 cold lock), `pipe_write.cpp:453-462`,
`pipe_transport.cpp:44-60`, 그리고 이를 금지하는
`11-synchronization-model.ko.md:118-121`/영문 `:129-133`이다. `_in_sync`는 현재 `pipe_t`에
존재하지 않는다.

## 2. MAC-4 — 매 receive turn의 context termination

**불일치(수정 필요).** `ctx_termination.cpp:82-110`은 모든 socket에 termination을 먼저
publish한 뒤 monitor teardown과 `stop()`을 수행한다. 세 blocking data-receive loop는
`socket_base_msg.cpp:701-709,830-838,980-988`에서 매 turn 시작마다 `_ctx_terminated`를 보고
`ETERM`을 반환하며, 실제 CV 복귀 경로도 `socket_base_lifecycle.cpp:1619-1624`에서 이를 확인한다.
따라서 “wait 중”뿐 아니라 이미 준비된 backlog를 계속 소비하는 non-sleeping turn 사이에서도
종료가 우선한다.

수정 필요 문장은 네 개다.

1. `core/doc/spec/core/socket/README.ko.md:546-551`
   - 현재 문장: “Blocking wait 중 context termination은 `ZLINK_RECV_TERMINATED`,
     `errno == ETERM`, socket shutdown은 `ZLINK_RECV_INVALID_STATE`,
     `errno == ESHUTDOWN`이다.”
   - 제안 문장: “`NONE` data receive는 각 receive turn 시작에서 context termination을
     관측한다. Blocking wait 중이거나 이미 준비된 backlog를 연속 drain 중이더라도 종료를
     관측하면 `ZLINK_RECV_TERMINATED`, `errno == ETERM`으로 끝나며, socket shutdown은
     `ZLINK_RECV_INVALID_STATE`, `errno == ESHUTDOWN`이다.”

2. `core/doc/spec/core/socket/README.en.md:584-591`
   - 현재 문장: “Context termination during a blocking wait returns
     `ZLINK_RECV_TERMINATED` with `errno == ETERM`; socket shutdown returns
     `ZLINK_RECV_INVALID_STATE` with `errno == ESHUTDOWN`.”
   - 제안 문장: “A `NONE` data receive observes context termination at the start of every
     receive turn. Whether it is in a blocking wait or continuously draining an already-ready
     backlog, observing termination returns `ZLINK_RECV_TERMINATED` with `errno == ETERM`;
     socket shutdown returns `ZLINK_RECV_INVALID_STATE` with `errno == ESHUTDOWN`.”

3. `core/doc/spec/core/socket/08-stream.ko.md:221-224`
   - 현재 문장: “Blocking 중 context termination은 `ZLINK_RECV_TERMINATED`+`ETERM`,
     socket shutdown은 `ZLINK_RECV_INVALID_STATE`+`ESHUTDOWN`이다.”
   - 제안 문장: “Blocking PACKET receive는 각 receive turn 시작에서 context termination을
     관측한다. Wait 중이거나 준비된 packet backlog를 drain 중이더라도 종료를 관측하면
     `ZLINK_RECV_TERMINATED`+`ETERM`이며, socket shutdown은
     `ZLINK_RECV_INVALID_STATE`+`ESHUTDOWN`이다.”

4. `core/doc/spec/core/socket/08-stream.en.md:232-234`
   - 현재 문장: “Context termination while blocked returns `ZLINK_RECV_TERMINATED` with
     `ETERM`; socket shutdown returns `ZLINK_RECV_INVALID_STATE` with `ESHUTDOWN`.”
   - 제안 문장: “A blocking PACKET receive observes context termination at the start of
     every receive turn. Whether waiting or draining a ready packet backlog, observing
     termination returns `ZLINK_RECV_TERMINATED` with `ETERM`; socket shutdown returns
     `ZLINK_RECV_INVALID_STATE` with `ESHUTDOWN`.”

Socket README의 completion receive 문장(`README.ko.md:1213-1219`, 영문 `:1346-1354`)은
별도 completion pull 계약이다. MAC-4가 수정한 세 data receive loop와 혼동하지 않았으며 이번
수정 수에 넣지 않았다. `04-thread-safety`와 `11-synchronization-model`은 종료 결과를 소유하지
않고 turn/wait ordering만 소유하므로 그 침묵은 적절하다.

## 3. CCU-2~5 — 증분 Auto-HWM과 debounce 수렴

§3의 “`budget_generation`은 새 plan 기록마다 증가”
(`06-auto-hwm.ko.md:238-249`, 영문 `:192-203`)는
`ctx_auto_hwm_state.cpp:298-328`과 일치한다. §4의 deferred shrink
(`06-auto-hwm.ko.md:475-480`, 영문 `:366-368`)도 full pass 뒤 높은 applied 값을 deadline
재무장 근거로 쓰지 않는 `ctx_auto_hwm_state.cpp:331-339`와 일치한다. 첫 attach가 세운
deadline만 timer를 깨우고(`ctx_auto_hwm_recalc.cpp:119-158`), 증분 record는 이를 보존하며
full pass 한 곳만 해제한다(`ctx_auto_hwm_recalc.cpp:228-235,303-310`). §5의 같은 입력에서
effective budget이 결정적이라는 요구(`06-auto-hwm.ko.md:562-568`, 영문 `:441-447`)도 이
스케줄 순서를 값의 입력으로 삼지 않으므로 정합이다.

그러나 증분 확장 실패 시 attach는 실제로 `auto_hwm_recalculate_now()`를 동기 호출한다
(`socket_base_api.cpp:316-326,635-649`). 확장 함수도 invalid/빈 입력, stopped state, 정확한
extension 불가 또는 예외에 `false`를 반환한다(`ctx_auto_hwm_recalc.cpp:161-226`). 따라서
§2의 괄호 문장은 절대 명제로는 거짓이다. Public
`zlink_ctx_auto_hwm_recalculate()`의 명시적 즉시 재계산(`06-auto-hwm.ko.md:201-220`, 영문
`:159-174`)은 정상적인 별도 API이므로 충돌이 아니다.

1. `core/doc/spec/core/systems/06-auto-hwm.ko.md:160`
   - 현재 문장: “이미 붙어 있는 방향의 목표 인하는 option 변경과 같은 debounce 재계산
     경로가 기록한다(attach는 전체 재계산을 동기로 돌리지 않는다).”
   - 제안 문장: “이미 붙어 있는 방향의 목표 인하는 option 변경과 같은 debounce 재계산
     경로가 기록한다. Attach는 마지막 plan을 정확히 증분 확장할 수 있으면 전체 재계산을
     동기로 돌리지 않으며, 증분 확장이 불가능하면 같은 attach 경로에서 동기 full
     recalculation으로 fallback한다.”

2. `core/doc/spec/core/systems/06-auto-hwm.en.md:126`
   - 현재 문장: “The lowered target of directions already attached is recorded by the same
     debounced recalculation path that option changes use (an attach does not run a synchronous
     full recalculation).”
   - 제안 문장: “The lowered target of directions already attached is recorded by the same
     debounced recalculation path that option changes use. When the last plan can be extended
     exactly, attach does not run a synchronous full recalculation; if incremental extension is
     not possible, that attach path falls back to a synchronous full recalculation.”

Deadline을 별도 공개 상태로 문서화할 필요는 없다. §2가 관찰 가능한 즉시/지연 적용을, §3이
generation을, §4가 shrink를, §5가 결정성을 이미 소유하며 deadline은 그 계약을 만족시키는 내부
coalescing 장치다.

## 4. SD B2 — 첫 full-hit 성장

**스펙 침묵, 문서화 불필요.** 현재 정책은 full request 한 번을 확인하면
`current * 2`를 기존 max로 clamp한다
(`asio_stream_fastpath_policy.hpp:332-376`). Encoder도 full batch 한 번에 같은 규칙을 쓰며
short WS batch는 initial target으로 줄인다(`:379-399`). Engine 적용 위치는
`asio_engine.cpp:866-884,904-916`이다.

`08-stream.ko.md:374-409`/영문 `:400-437`은 공개 옵션이 아닌 runtime 기본값과
`ZLINK_ASIO_STREAM_INITIAL_TARGET_CAP=4096`만 고정한다. “연속 2회 full hit”를 요구하는
문장은 지정 스펙에 없고, max는 `rcvbuf`/`sndbuf`/`maxmsgsize`로 계속 제한된다
(`asio_stream_fastpath_policy.hpp:264-329`). 이 변화는 byte 순서, message 경계, public 오류,
buffer 상한을 바꾸지 않는 내부 성능 휴리스틱이므로 1회/2회 기준을 스펙 계약으로 추가하지 않는
편이 맞다.

## 5. SD-5 — WS/WSS bounded 3-buffer gather

코드는 큰 body를 heap-owned `msg_t`로 옮기고 body pointer를 보존하면서, 뒤의 준비된 frame을
target까지 계속 encode한다(`asio_engine.cpp:1310-1427`). 제출 시 최대 3개의 const buffer를
`[앞, body, 뒤]` 순서로 만들고 `async_writev()` 한 번 호출한다(`:794-837`). WS adapter는 그
sequence 전체를 Beast `async_write()` 한 번에 넘긴다
(`ws_transport_common_internal.hpp:236-265`). Target/max admission은
`ws_batch_policy.hpp:12-48`이 소유한다.

### 5.1 `01-zmp` 수정 필요

1. `core/doc/spec/core/protocol/01-zmp.ko.md:489-492`
   - 현재 문장: “송신 encoder는 현재 준비된 ZMP byte를 기존 `out_batch_size` 상한까지
     output buffer에 모으고, 그 bounded batch를 Beast binary write 한 번으로 제출한다.”
   - 제안 문장: “송신 encoder는 현재 준비된 ZMP byte를 기존 `out_batch_size` 상한까지
     bounded batch로 모은다. 큰 body 하나는 payload를 복사하지 않고
     `[앞 output buffer, body pointer, 뒤 output buffer]` sequence로 구성하며, WebSocket
     adapter는 그 batch를 Beast binary write 한 번으로 제출한다.”

2. `core/doc/spec/core/protocol/01-zmp.en.md:524-528`
   - 현재 문장: “The sending encoder collects currently available ZMP bytes in its output
     buffer up to the existing `out_batch_size` bound and submits that bounded batch with one
     Beast binary write.”
   - 제안 문장: “The sending encoder collects currently available ZMP bytes into a bounded
     batch up to the existing `out_batch_size` limit. One large body may remain un-copied as
     the middle element of a `[prefix output buffer, body pointer, suffix output buffer]`
     sequence; the WebSocket adapter submits that batch with one Beast binary write.”

기존 다음 문장의 “미래 traffic을 기다리지 않음, frame byte·순서·multipart 경계 불변”은
`asio_engine.cpp:1320-1327,1402-1427`과 일치한다. `01-zmp.ko.md:411-414`/영문
`:436-439`의 payload-copy 금지 문장도 새 pointer body와 일치한다.

### 5.2 `03-io-thread` 수정 필요

3. `core/doc/spec/core/systems/03-io-thread.ko.md:81-85`
   - 현재 문장: “engine(`asio_engine_t`)이 transport에 `async_read_some()` /
     `async_write_some()`을 요청하면 I/O thread의 `io_context`가 OS 비동기 I/O 완료를
     기다렸다가 completion callback을 부른다.”
   - 제안 문장: “engine(`asio_engine_t`)이 transport에 `async_read_some()`,
     `async_write_some()` 또는 buffer-sequence `async_writev()`를 요청하면 I/O thread의
     `io_context`가 비동기 I/O 완료를 기다렸다가 completion callback을 부른다.”

4. `core/doc/spec/core/systems/03-io-thread.ko.md:102-103`
   - 현재 문장: “Write 완료 → send pipe에서 꺼낸 message를 encode해 보낸 뒤, 남은 data가
     있으면 다음 `async_write_some()`을 건다.”
   - 제안 문장: “Write 완료 → send pipe에서 꺼낸 message를 encode해 보낸 뒤, 남은 data가
     있으면 다음 비동기 write operation(`async_write_some()` 또는 `async_writev()`)을 건다.”

5. `core/doc/spec/core/systems/03-io-thread.en.md:82-87`
   - 현재 문장: “When the engine (`asio_engine_t`) calls `async_read_some()` /
     `async_write_some()` on the transport, the I/O thread's `io_context` waits for the OS
     asynchronous I/O operation to complete and then invokes the completion callback.”
   - 제안 문장: “When the engine (`asio_engine_t`) calls `async_read_some()`,
     `async_write_some()`, or the buffer-sequence `async_writev()` on the transport, the I/O
     thread's `io_context` waits for the asynchronous I/O operation to complete and then invokes
     the completion callback.”

6. `core/doc/spec/core/systems/03-io-thread.en.md:104-105`
   - 현재 문장: “Write completion → Encode and send the message pulled from the send pipe,
     then issue the next `async_write_some()` if data remains.”
   - 제안 문장: “Write completion → Encode and send the message pulled from the send pipe,
     then issue the next asynchronous write operation (`async_write_some()` or
     `async_writev()`) if data remains.”

`10-hot-path`의 public API→pipe write/read 규범 범위(`10-hot-path.ko.md:30-43`, 영문
`:33-47`)는 transport engine의 이후 batching 단계까지 포함하지 않는다. 따라서 SD-5의 큰-body
소유용 `new msg_t`(`asio_engine.cpp:1344-1357`)를 그 문서의 public hot-path heap 금지와
충돌한다고 판정하지 않았다.

## 6. D-f — STREAM environment variables

삭제된 `ZLINK_ASIO_STREAM_DISABLE_GATHER`,
`ZLINK_ASIO_STREAM_GATHER_THRESHOLD`,
`ZLINK_ASIO_STREAM_TINY_GATHER_THRESHOLD`는 현재 `core/`에서 changelog 외 접근이 0건이다.

`asio_stream_fastpath_policy.hpp:14-31,49-76,221-224`가 실제 읽는 변수는 다음과 같다.

| 변수 | STREAM 전용인가 | 현재 STREAM 목록 처리 |
|---|---|---|
| `ZLINK_ASIO_GATHER_WRITE` | 아니오. ZMP gather용이며 RAW STREAM은 header를 만들지 않는다(`:129-143`). | 제외 타당 |
| `ZLINK_ASIO_SINGLE_WRITE` | 아니오. Asio 공통 diagnostic이며 STREAM speculative loop에도 영향을 준다(`asio_engine.cpp:90,1536-1539`). | “전용” 목록이면 제외 타당 |
| `ZLINK_ASIO_GATHER_THRESHOLD` | 아니오. ZMP/WS gather threshold다(`asio_engine.cpp:92,1330-1343`). | 제외 타당 |
| `ZLINK_ASIO_TRACE` | 아니오. Asio 공통 진단이다(`asio_engine.cpp:108`). | 제외 타당 |
| `ZLINK_ASIO_LEGACY_SYNC_WRITE` | 아니오. 주석과 predicate가 non-STREAM 전용이다(`:59-68,118-127`). | 제외 타당 |
| `ZLINK_ASIO_STREAM_ENABLE_NON_TCP_SPEC_READ` | 예 | 문서에 있음 |
| `ZLINK_ASIO_STREAM_ASYNC_WRITE` | 예 | 문서에 있음 |
| `ZLINK_ASIO_STREAM_INITIAL_TARGET_CAP` | 예 | 문서에 있음 |

나머지 STREAM 전용 5개 접근자도 문서와 일치한다:
accept concurrency(`asio_listener_accept_policy.hpp`), session scheduler
(`ctx_io_thread_registry.cpp:22-24`), batch size/headroom(`stream_batch_policy.hpp`), pipe LWM hint
(`session_base.cpp:34`). 문제는 현재 제목이 generic Asio 변수까지 포함하는 전체 목록처럼 읽히는
점이다. 항목을 중복 추가하기보다 소유 범위를 “STREAM 전용”으로 명시하는 것이 맞다.

1. `core/doc/spec/core/socket/08-stream.ko.md:398`
   - 현재 문장: “현재 유지되는 STREAM 런타임 환경변수는 다음과 같다.”
   - 제안 문장: “현재 유지되는 STREAM 전용 런타임 환경변수는 다음과 같다. 여러 socket
     type에 공통인 Asio 진단·gather 변수는 이 목록에 포함하지 않는다.”

2. `core/doc/spec/core/socket/08-stream.en.md:426`
   - 현재 문장: “STREAM retains the following runtime environment variables.”
   - 제안 문장: “STREAM retains the following STREAM-specific runtime environment variables.
     Generic Asio diagnostic and gather variables shared by multiple socket types are outside
     this list.”

## 7. RR-1 — 1,025-part reply

**스펙 침묵, 문서화 불필요.** `02-message.ko.md:95-110`/영문 `:97-114`는 multipart의
`MORE`→`FINAL`, ownership, thread 규칙만 정의하며 part-count 상한을 두지 않는다. 현재 reply
경로는 `total_part_count`만큼 동적 buffer를 reserve하고 모든 part를 순회하며, 실패는 allocation
`ENOMEM` 또는 기존 admission 오류다(`socket_request_reply_runtime_io.cpp:1407-1432,1449-1517`).
`request_reply_internal.hpp:33-37`의 `65536`은 reply target slot 수, `64`는 whole-record drain
budget이지 reply part 수 제한이 아니다.

회귀 테스트도 1,023·1,024·1,025·2,048 parts를 모두 같은 completion으로 검증한다
(`test_request_reply_part_count_boundary.cpp:232-257`). 그러므로 1,024/1,025는 private-head batch
경계일 뿐 public 계약 숫자가 아니다. 스펙에 “1,025까지” 같은 문장을 추가하면 그보다 큰 정상
multipart를 제한하는 잘못된 계약이 된다.

## 8. 한·영 쌍 정합

0.17.4 관련 spec commit은 모두 한·영 파일을 함께 갱신했다.

- Auto-HWM D-H1: `c55da9c6c0`, ko/en 동시 변경
- STREAM D-f: `19fcc2fcb2`, ko/en 동시 변경
- 나머지 지정 파일도 각 pair의 마지막 변경 commit이 동일하다.

MAC, CCU, SD-5, D-f에서 위에 지적한 오류는 한쪽만 갱신된 것이 아니라 한·영 양쪽에 같은
형태로 존재한다. 한쪽만 의미가 어긋난 문장은 다음 하나다.

1. `core/doc/spec/core/systems/04-thread-safety.en.md:59-64`
   - 현재 문장: “The guard never waits: a new entry after close is accepted, and a close while
     an API call is in flight, are both rejected immediately, and the result the caller observes
     (`ESHUTDOWN`, `EBUSY`) is defined by Socket Common §2 Thread safety.”
   - 제안 문장: “The guard never waits: a new entry after close has been accepted fails
     immediately, as does close while an API call is in flight; the result the caller observes
     (`ESHUTDOWN`, `EBUSY`) is defined by Socket Common §2 Thread safety.”

한국어 대응 문장 `04-thread-safety.ko.md:56-60`은 “close가 accepted된 뒤의 새 진입”이
거부된다고 정확히 적었다. 현재 영문은 `is accepted` 때문에 그 반대 의미가 되며,
`socket_runtime.hpp:735-754`의 public entry guard 및 Socket README §2와도 맞지 않는다.

## 지정 스펙 나머지 범위

`socket/07-router`, `05-connection-memory`에는 위 변경을 반박하는 문장이 없었다. ROUTER route
snapshot은 public RID/mandatory/HWM 결과를 바꾸지 않는 C1 구현이며, allocator recycle과 B2 buffer
성장은 monitor가 allocator overhead를 완전 측정하지 않는다는 memory 문장과 정합이다.
`10-hot-path`는 위 `_in_sync`/`_out_sync` 허용 문장 외에는 현재 direct-pipe·snapshot·turn 경로와
일치한다. `11-synchronization-model`의 한·영 pair에는 0.17.3 `receive_owner` word를 전제한 문장이
없다.

수정 필요 문장 수 / 스펙 정합 여부: **17 / 불일치(수정 필요)**
