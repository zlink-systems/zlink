---
title: "Core 멀티파트 whole-message recv 공개 API 신설 + router recv 네이밍 정정 — draft(설계)"
---

# Core whole-message `recv()` 공개 API 신설 + router recv 네이밍 정정 — draft

> **설계 draft.** 무엇을·왜·어떤 시그니처로 만들지를 정한다. 실제 문서/코드/perf 반영 대상과 순서는 별도 **적용 plan**
> (`doc/plan/core-whole-message-recv-api-apply-plan.ko.md`)이 소유한다.

## 1. 배경·문제

- 현재 공개 recv는 **part 단위**만 있다: `zlink_recv_part`, `zlink_router_recv_part`, `zlink_subscribe_part`, `zlink_xpub_recv_part`
  (`core/include/zlink/socket/api.h:285~`). 호출당 `zlink_msg_t part_out_` 1개 + `has_more_out_`를 돌려준다.
- part 단위 설계 이유: **단건(single-part) 메시지에서 컬렉션 할당 비용을 피하려고**. 타당한 동기다.
- 그러나 **수신 메시지의 상당수가 멀티파트**라면 part 단위 API만 제공하는 것은:
  1. **호출측 코드를 복잡하게** 만든다(record마다 has_more 루프 + rid/token 첫 part 처리 + 재조립).
  2. Core 내부적으로 record를 part 단위로 쪼개 넘기는 **어댑터**(멀티파트 시퀀스 버퍼 + 커서 + per-socket `handle_state_t.recv` + `mutex`)를 강제한다
     (`router_recv_part_impl`의 `stage_recv_sequence`/`take_recv_part`).
  3. 바인딩(Node/.NET 등)은 어차피 `Received.parts` **컬렉션으로 다시 합쳐** 앱에 준다 → "record→part로 쪼갬→다시 합침"의 **양방향 낭비**이고,
     .NET/Node routed 진단에서 확인된 **메시지당 native 경계 다수·per-message 고정비**의 근원이다.
- Core 내부엔 이미 **record 전체를 한 번에 pull**하는 경로가 있다: `recv_router_message_direct`
  (`core/src/api/socket/socket_request_reply_runtime_io.cpp:951`) — 소켓 pipe에서 `recv_routed`로 프레임을 받아 rid/reply token/모든 part를
  한 번에 돌려준다. **내부 심볼**이라 공개돼 있지 않다.

## 2. 목표

1. **네이밍 정정**: `reqrep::recv_router_message_direct`는 DATA(`send`)·REQUEST를 **모두** 받는 일반 router recv이므로 `reqrep` 소속·이름이
   오해를 준다. 일반 router recv 이름/네임스페이스로 정정(내부 심볼, 호출부 소수 → contained·저위험, perf 영향 없음).
2. **whole-message `recv()` 공개 API 신설**: 멀티파트 메시지를 **컬렉션으로 한 번에** 받는 공개 함수를 추가한다. 기존 `*_recv_part`는
   **그대로 유지**(단건·zero-alloc 경로). 두 API 공존: part 단위(저할당) + whole-message(저복잡·저경계).

## 3. 신설 API 설계

### 3.1 공통 원칙
- 기존 recv 계열과 **동일 관용**: `zlink_recv_result_t` 반환, `zlink_recv_flags_t`(DONTWAIT 포함), source RID는 socket-owned borrowed view
  (다음 data-recv/close까지 유효). ROUTER는 `zlink_reply_token_t`도 함께.
- **한 번의 호출이 record 하나(모든 part)를 원자적으로 소비**한다. part 중간 상태(시퀀스 커서)가 남지 않는다 → 시퀀스 버퍼·mutex 불필요.
- **single-consumer pull** 계약 유지(동시 recv 직렬화는 caller 의무, Core는 직렬화하지 않음). thread-safety 모델은 기존과 동일하며 완화하지 않는다.

### 3.2 parts 컬렉션 소유권 — 선택지
- **(A·권장) caller-제공 배열(bounded, zero-alloc)**: caller가 `zlink_msg_t parts_out_[]`와 capacity를 준다. Core가 채우고 `count_out_` 기록.
  각 슬롯은 caller-소유 msg(각자 close). capacity 부족 시 `count_out_`에 필요한 수를 쓰고 truncated 결과 반환(프레임 상태 규칙은 §6). 바인딩은
  재사용 배열(pool)로 zero-alloc 달성 → 현재 per-message 컬렉션 할당 오버헤드 제거. `recvmmsg` 스타일.
- (B) Core-할당 배열 반환 + `zlink_free_parts()`: 유연하나 ownership·free 계약이 늘어 공개 표면 복잡 → 지양.
- **권장 (A)**: 기존 part_out(호출자 소유 슬롯) 관용과 일관되고 "컬렉션 할당 비용 회피" 취지를 whole-message에서도 지킨다.

### 3.3 제안 시그니처 (초안, (A) 기준)
```c
/* ROUTER: 한 record(모든 part)를 한 번에 수신. parts_out_는 caller-제공 배열(용량 parts_capacity_).
 * 성공 시 part_count_out_에 실제 part 수(각 슬롯 caller-소유 msg, 각자 close).
 * source_rid_out_는 borrowed view, reply_token_out_는 REQUEST일 때만 비-0. */
ZLINK_EXPORT zlink_recv_result_t
zlink_router_recv (void *router_,
                   const zlink_routing_id_t **source_rid_out_,
                   zlink_reply_token_t *reply_token_out_,
                   zlink_msg_t *parts_out_, size_t parts_capacity_, size_t *part_count_out_,
                   zlink_recv_flags_t flags_);

/* PAIR·DEALER: 한 record(모든 part)를 한 번에 수신. */
ZLINK_EXPORT zlink_recv_result_t
zlink_recv (void *s_,
            const zlink_routing_id_t **source_rid_out_,
            zlink_msg_t *parts_out_, size_t parts_capacity_, size_t *part_count_out_,
            zlink_recv_flags_t flags_);
```
- SUB/XSUB(topic)·XPUB(subscribe event)은 필요 시 같은 패턴으로 확장(2차). **우선 ROUTER·PAIR·DEALER data recv**(routed/perf 대상)부터.
- **확정 검토 항목**: 이름(`zlink_recv` vs `zlink_recv_message`), capacity 초과 정책(truncate+error vs 프레임 보존 후 재시도), DONTWAIT 시
  부분 record 처리(멀티파트 도중 데이터 없음 → record 원자성 규칙과 정합).

### 3.4 `recv_part`와의 관계
- `*_recv_part`는 **유지**. 단건·스트리밍(부분 소비)·초저할당 경로용. 신설 `recv`는 대부분-멀티파트·저복잡 경로용.
- 내부 구현: 신설 `recv`는 whole-record pull(`recv_router_message_direct` 계열)을 **직접** 사용(part 어댑터/시퀀스 버퍼 우회).
  `recv_part`는 계속 어댑터 경로. 즉 **시퀀스 mutex·버퍼는 recv_part 경로에만** 남고 whole-message 경로엔 그 오버헤드가 없다.
- 같은 소켓에서 두 API 혼용 시 규칙(진행 중 part 시퀀스가 있으면 whole `recv`는 EBUSY 등)은 spec에 명시.

### 3.5 바인딩 recv 시그니처 (이미 whole-message 형태 — 공개 표면 유지)
핵심: **바인딩 공개 API는 이미 `recv(Received)` whole-message 형태**로, `Received{ parts:[...], routingId, replyToken }` 컬렉션을 채운다.
따라서 이번 작업으로 **공개 시그니처는 바뀌지 않고**, 내부 채우기만 Core whole-message recv로 전환한다(part 루프 → 1회 호출).

| 바인딩 | 현재(=유지) whole-message recv 시그니처 | 채우는 컬렉션 |
|--------|------------------------------------------|---------------|
| **C** (Core) | `zlink_recv(s, rid_out, parts_out[], cap, count_out, flags)` / `zlink_router_recv(r, rid_out, token_out, parts_out[], cap, count_out, flags)` **(신설)** | caller-제공 `zlink_msg_t[]` |
| **C++** | `int recv(received_t& out, recv_flags_t = none)` (+ 값반환 `received_t recv()`) — `message_socket_contracts.hpp:46/69`, `routed_socket_contracts.hpp:21` | `received_t`(parts/rid/reply token) |
| **.NET** | `bool Recv(Received result, RecvFlags flags = None)` — `MessageSocketContracts.cs:25` | `Received` |
| **Java** | `boolean recv(Received result, RecvFlags flags)` — `PairSocket/DealerSocket/RouterSocket.java` | `Received` |
| **Node/TS** | `recv(result: Received, flags?: RecvFlags): boolean` — `pair/dealer/router_socket.ts` | `Received` |

- 반환 관용: 성공 `true`/`0`, non-blocking no-data는 `false`/no-data 결과(기존과 동일). rid/reply token은 `Received`(또는 값 객체)에 담김.
- **변경 지점은 내부뿐**: 각 바인딩이 `Received.parts`를 채울 때 Core `*_recv_part` 루프 대신 **신설 Core whole-message recv 1회 호출 + 재사용 배열**을
  사용한다(per-message 경계·할당 감소). 신규 공개 메서드 추가는 원칙적으로 없음(필요 시 언어별로 판단하고 문서화).

## 4. 바인딩 매핑 — **바인딩 라이브러리 recv의 "내부 구현" 변경** (공개 시그니처 불변)
- 각 바인딩의 공개 `recv(Received)`는 이미 `Received{ parts:[...], routingId, replyToken }` 컬렉션을 반환한다. **공개 시그니처는 바뀌지 않는다.**
- **바뀌는 것은 그 함수의 내부 구현뿐**: 지금은 바인딩 라이브러리가 Core `*_recv_part`를 **`has_more==0`까지 while 루프**로 돌려 `Received.parts`를
  채운다(part마다 msg_init + P/Invoke/N-API 경계 + 컬렉션 append). 신설 후에는 **Core whole-message `recv`를 1회 호출**해 재사용 배열로 채운다
  (루프·part별 init/경계/append 제거). 단건 fast-path(예: .NET `AdoptNativeFromPool`)는 유지.
- **언어별 내부 변경 지점(=적용 plan §5 대상):**
  | 바인딩 | recv(Received) 내부 루프 위치(변경 대상) |
  |--------|------------------------------------------|
  | .NET | `bindings/dotnet/src/Zlink/Runtime/Sockets/SocketKernel.ReceiveCore.cs` — `ReceiveRouterParts`(:187, `while` :201, `zlink_router_recv_part[_nowait]` :217/220, `AppendNativePart` :250) |
  | C++ | `bindings/cpp/src/Runtime/...`의 `socket_t::recv(received_t&)` 내부 part 루프(구현 파일 확인) |
  | Node | addon 수신(`bindings/node/native/src/addon_core.cc`)의 router recv_part 루프 + `Received` 채우기 |
  | Java | `bindings/java/.../runtime/sockets/NativeRouterReceiveSupport`(또는 대응) recv_part 루프 |
- **공개 편의 메서드 추가는 원칙적으로 없음**(이미 `recv(Received)` 존재). 필요 시 언어별 판단·문서화(적용 plan).
- 이득: 바인딩 내부 per-message 경계·할당 감소(= .NET/Node routed 진단의 목표 지렛대). perf는 이미 `recv(Received)`를 호출하므로 **하네스 변경 없이
  자동 반영**. C 레퍼런스 perf는 신설 Core `recv`로 바꾸면 조립 제거·단순화.

## 5. 리스크·주의
> **record 원자성은 Core가 이미 보장한다** — 따라서 아래 "부분 수신/DONTWAIT 절반/혼용" 관련 우려는 새 설계 결정이 아니라 기존 계약으로 커버된다.
> 근거: `core/doc/spec/core/socket/README.ko.md:414`("수신을 시작한 record는 마지막 part까지 보존"), `recv_router_message_direct`가
> `collect_multipart_payload_parts`(`socket_request_reply_runtime_io.cpp:525`)로 **한 record의 모든 part를 모아서** 반환.
- **DONTWAIT 부분-record: 해당 없음.** record를 시작하면 마지막 part까지 받으므로 "절반 record" 상태가 존재하지 않는다 → whole-message recv는
  **전체 record 또는 no-data**만 반환. 별도 규칙 불필요.
- **capacity: 데이터 무결성 이슈 아님(구현 디테일).** record는 Core가 통째로 확보하며, 바인딩은 재사용 growable 배열(현 `MultipartMessageCollection`
  방식)로 채운다. C 공개 API 형태(caller 배열+capacity)만 "충분한 크기 제공 / count 반환"으로 정하면 되고, 부분 소비로 시퀀스가 남는 위험은 없다.
- **recv_part 혼용: record 원자성으로 커버.** 진행 중 반쪽 시퀀스가 존재하지 않으므로 새 규칙 신설 불필요. 필요 시 기존 `ZLINK_RECV_BUSY`(EBUSY,
  "another receive owner active", `zlink_enum` 202) 시맨틱을 그대로 재사용.
- 공개 C ABI 추가 → 되돌리기 어려움(시그니처만 spec에서 확정).
- thread-safety·single-consumer 계약은 기존과 동일(완화 금지).
- 공개 표면 단순성 유지: 편의를 늘리되 계약을 파편화하지 않는다.

## 5.5 이득 (기대 효과)
1. **native 호출 횟수 감소**: 멀티파트 record 수신이 part당 recv 호출(N회) → **1회**로. 바인딩은 per-part P/Invoke·N-API 경계·`msg_init`·컬렉션
   append가 사라진다.
2. **멀티파트 시퀀스 mutex/버퍼 우회**: `recv_part` 경로는 Core `part_helper`의 시퀀스 상태(`recv.active`/`owner_thread`)를 매번 `helper_state->mutex`
   아래에서 다루고, 남은 part를 버퍼(`stage_recv_sequence`)에 담았다가 `take_recv_part`로 꺼낸다. **whole-message recv는 한 record를 한 번에
   반환**하므로 시퀀스 커서·버퍼·그 mutex 경로를 **아예 타지 않는다**(단일 스레드 pull에서 uncontended라도 제거되는 고정비). 이 lock은 perf가 아니라
   Core에 있고 recv_part를 타는 **모든 언어**(C perf 직접 + cpp/dotnet/java/node 바인딩 내부 루프)가 통과하던 것이다.
3. **C 레퍼런스 perf·바인딩 내부 단순화**: C perf는 `zlink_router_recv_part`+has_more 수동 조립을 제거, 바인딩은 recv_part while 루프를 1회 호출로 교체.
4. **호출측 단순화**: 대부분-멀티파트 워크로드에서 "record→part로 쪼갬→다시 합침"의 양방향 낭비 제거. 공개 시그니처는 불변.
- 주의: thread-safety·single-consumer 계약은 그대로 유지된다(이득은 계약 완화가 아니라 경로 제거에서 온다).

## 6. 성공 기준(요약)
- 멀티파트 수신을 한 번의 호출·컬렉션으로 받는 공개 경로 제공, `recv_part` 병존.
- 바인딩 내부 per-message 경계·할당 감소 → routed 목표 갭 축소(§ perf 재측정), 비대상 회귀 없음.
- 공개 계약·thread-safety·측정 의미 보존.

---

## 7. 범위 확대 검토 — send도 whole-message로, part API는 제거 (2026-09-10)

### 7.1 왜 send까지 보게 됐나

Framework Java의 send 지연 수정 과정에서 **`BUSY`가 반복되는 현상**이 관측됐다. 원인 후보를 좁히며
확인한 것은 다음이다.

- Core 공개 send API는 전부 part 단위다 — `zlink_send_part`, `zlink_send_part_rid`, `zlink_request_part`
  (`core/include/zlink/socket/api.h:238,245,262`). 배열과 개수를 받는 형태가 없다.
- 그래서 바인딩이 파트마다 네이티브를 호출하는 루프를 돈다
  (`bindings/java/src/main/java/systems/zlink/runtime/sockets/SocketSendPlane.java:372-383`).
- 그 루프가 **"한 record의 첫 part부터 FINAL까지 같은 thread"** 라는 계약을 만든다
  (`core/doc/spec/core/socket/README.ko.md` §2 스레드 안전성). 이 조건을 어기면 `BUSY`다.

즉 recv에서 해결하려는 것과 **같은 뿌리**다. record 하나를 여러 번의 호출로 만드는 표면이
미완성 record 상태를 만들고, 그 상태가 스레드 계약·오류 경로·재시도 규칙을 낳는다.

### 7.2 조사 결과 — 제거해도 되는가

감독자 지시로 저장소 전체를 조사했다(2026-09-10). 세 질문과 답이다.

**Q1. send에서 part를 나중에 만드는 곳이 있는가 → 없다.**
모든 multipart 제출이 이미 만들어진 배열을 인덱스로 도는 루프다. 마지막 part 표시는 언제나
`index == count-1`이며 데이터에 따라 동적으로 정해지지 않는다.
- `bindings/c/perf/common/perf_zlink_part_helpers.hpp:57-64,76-83,121-127`
- `bindings/python/src/zlink/_native/_zlink_native.c:729-732`(part flag는 `:199`에서 `(index, count)`로 계산)
- `bindings/rust/src/runtime/messaging/operations/send_ops.rs:100,128-147,366-383`
- `bindings/dotnet/src/Zlink/Runtime/Messaging/RequestReplySupport.cs:241`
- relay/echo 전달 경로도 받은 parts를 **먼저 전부 담은 뒤** 루프를 돈다
  (`bindings/c/perf/multi/common/perf_multi_relay_server.hpp:206-243`).
- framework의 header-then-body도 lazy가 아니다. 헤더를 인코딩해 목록 앞에 붙인 **하나의 목록**을 제출한다
  (`framework/languages/java/.../ZLinkChannelRouteCalls.java:534,669`,
  `framework/languages/cpp/.../mesh_node_host_service.cpp:528`,
  `framework/languages/node/.../node-raw-binding-port.ts:384-392`).

예외 하나: `bindings/c/perf/single/common/perf_single_reqrep.hpp:635,647-664`가 payload를 `MORE`로 보낸 뒤
**빈 `FINAL`만 backpressure 루프로 재시도**한다. 이는 "실패한 FINAL은 staged prefix를 버린다"는 문서 규칙
(`core/doc/spec/core/socket/README.en.md:1078`)과도 어긋난다. whole-record 재시도로 바꿔야 하며,
**제품 코드가 아니라 perf 하네스**다.

**Q2. recv에서 part 단위 수신에 의존하는 곳이 있는가 → 없다.**
결정적 근거: **Core는 첫 part를 내주기 전에 이미 물리 record 전체를 버퍼링한다**
(`core/src/runtime/sockets/common/socket_base.hpp:816`). 따라서 part 0만 먼저 보는 것으로 아끼는 비용이 없고,
배열로 받는 것은 복사가 아니라 소유권 이전이다.
- framework ingress 분류는 **이미 전부 materialize된 벡터** 위에서 돈다
  (`framework/languages/cpp/.../backend/raw_route_port.hpp:34-39`, 분류 `raw_mesh_node_owner.cpp:2976`,
  거절 `:3179`). `framework/languages`에는 part API 호출이 **0건**이다.
- Node의 단일 part fast path(`bindings/node/native/src/addon_core.cc:2848-2870`)는 의미상 필요가 아니라
  최적화다. capacity 기반 whole-recv에 작은 배열을 쓰면 같은 효과다.
- 나머지를 건너뛰거나 버리는 기능은 **지금도 없다.** owner는 `FINAL`까지 드레인해야 하고, 중간에 다른
  주체가 들어오면 `BUSY`/`EBUSY`다(`README.en.md:575-577`). 즉 whole-message recv는 기능을 없애는 것이
  아니라 **함정을 없앤다**.

**Q3. STREAM·XPUB은 다른가 → 둘 다 이미 part 단위가 아니다.**
- STREAM send는 계약상 단일 part다. `ZLINK_PART_MORE`는 `NOT_SUPPORTED`/`ENOTSUP`
  (`core/doc/spec/core/socket/08-stream.en.md:116-119`). **주의**: 유효한 RID로 보내는 길이 0 part는
  "그 peer를 끊는다"는 별도 의미다(`:151-153`). 1-element 배열이 이 의미를 보존해야 한다.
- STREAM RAW recv는 항상 `FINAL` 한 개(`08-stream.en.md:178-180`). STREAM PACKET recv는 이미 header+body를
  함께 돌려주는 별도 진입점(`core/include/zlink/socket/api.h:328-334`)이며 "record = parts 배열"이 아니라
  고정 2슬롯 framing이다(`08-stream.en.md:120-123`). **일반 배열 API로 접지 말고 별도 호출로 유지한다.**
- XPUB의 `zlink_xpub_recv_part`는 `zlink_msg_t`를 받지 않는다. 구독 이벤트(subscribed + topic bytes)를 읽는
  단일 프레임 API이며(`core/doc/spec/core/socket/04-xpub.en.md:40-43`) 이름만 `_part`다. 통합 대상이 아니다.
- 반면 **SUB/XSUB의 multipart는 실재한다**(`03-sub.en.md:211-212,303-304`,
  `core/src/runtime/sockets/pubsub/xsub.cpp:284-343`의 `_recv_part_index`). 따라서 `zlink_subscribe_part`도
  같은 처리가 필요하다 — **§3의 6개 심볼 목록에 빠져 있다.**

### 7.3 작업 규모 (호출 지점 수, `build/`·`dist/`·`target/`·`node_modules/` 제외)

| 영역 | 지정 6개 심볼 | part 계열 전체* |
|---|---:|---:|
| `core/src` | 13 (10은 api shim·`libzlink.vers`, **내부 호출자 0**) | 21 |
| `core/include` | 6 | 11 |
| `core/tests` | 411 | 566 |
| `core/doc` | 489 | 756 |
| `bindings/c` | 96 | 140 |
| `bindings/cpp` | 29 | 42 |
| `bindings/dotnet` | 37 | 63 |
| `bindings/java/src` | 28 | 45 |
| `bindings/node` | 14 | 19 |
| `bindings/go` | 33 | 54 |
| `bindings/python` | 35 | 54 |
| `bindings/rust` | 16 | 24 |
| `bindings/doc` | 53 | 68 |
| `framework/languages` | **0** | **0** |
| `framework/bench` | 18 | 20 |
| `framework/doc` | 1 | 7 |

\* `zlink_reply_part`, `zlink_publish_part`, `zlink_subscribe_part`, `zlink_stream_recv_packet` 포함.

비용은 거의 전부 기계적이다. `core/doc` 약 1,000줄과 `core/tests` 약 450줄, 바인딩 내부 약 180줄이다.
**Core 내부에는 part API 호출자가 없고, framework에는 0건이다.**

### 7.3.1 제거 대상 전체 목록 (현재 시그니처 그대로)

`core/include/zlink/socket/api.h` 기준. 아래 **8개를 제거**한다.

```c
/* ── 제출(send) 계열 5개 ───────────────────────────────────────────── */

/* :238 */
ZLINK_EXPORT zlink_submit_result_t zlink_send_part (void *s_,
                                                    zlink_msg_t *part_,
                                                    zlink_send_flags_t flags_,
                                                    zlink_part_flag_t part_flag_,
                                                    void *user_context_,
                                                    zlink_completion_id_t *completion_id_out_);

/* :245 */
ZLINK_EXPORT zlink_submit_result_t zlink_send_part_rid (void *s_,
                                                        const zlink_routing_id_t *target_rid_,
                                                        zlink_msg_t *part_,
                                                        zlink_send_flags_t flags_,
                                                        zlink_part_flag_t part_flag_,
                                                        void *user_context_,
                                                        zlink_completion_id_t *completion_id_out_);

/* :262 — MORE는 timeout_ms_ == 0, user_context_ == NULL 제약이 있었다. 신설 API에서는 사라진다. */
ZLINK_EXPORT zlink_submit_result_t zlink_request_part (
  void *s_,
  const zlink_routing_id_t *target_router_rid_or_null_,
  zlink_msg_t *part_,
  zlink_send_flags_t flags_,
  zlink_part_flag_t part_flag_,
  uint32_t timeout_ms_,
  void *user_context_,
  zlink_completion_id_t *completion_id_out_);

/* :274 */
ZLINK_EXPORT zlink_submit_result_t zlink_reply_part (
  void *router_,
  const zlink_routing_id_t *source_rid_,
  zlink_reply_token_t reply_token_,
  zlink_msg_t *part_,
  zlink_part_flag_t part_flag_);

/* :297 */
ZLINK_EXPORT zlink_submit_result_t zlink_publish_part (void *subject_,
                                                       const char *topic_id_,
                                                       zlink_msg_t *part_,
                                                       zlink_send_flags_t flags_,
                                                       zlink_part_flag_t part_flag_);

/* ── 수신(recv) 계열 3개 ───────────────────────────────────────────── */

/* :286 */
ZLINK_EXPORT zlink_recv_result_t
zlink_router_recv_part (void *router_,
                        const zlink_routing_id_t **source_rid_out_,
                        zlink_reply_token_t *reply_token_out_,
                        zlink_msg_t *part_out_,
                        zlink_part_flag_t *has_more_out_,
                        zlink_recv_flags_t flags_);

/* :292 */
ZLINK_EXPORT zlink_recv_result_t zlink_recv_part (void *s_,
                                                  const zlink_routing_id_t **source_rid_out_,
                                                  zlink_msg_t *part_out_,
                                                  zlink_part_flag_t *has_more_out_,
                                                  zlink_recv_flags_t flags_);

/* :309 */
ZLINK_EXPORT zlink_recv_result_t zlink_subscribe_part (void *sub_,
                                                       const zlink_routing_id_t **source_rid_out_,
                                                       char *topic_id_buf_,
                                                       size_t topic_id_capacity_,
                                                       size_t *topic_id_len_out_,
                                                       zlink_msg_t *part_out_,
                                                       zlink_part_flag_t *has_more_out_,
                                                       zlink_recv_flags_t flags_);
```

**대체 관계**

| 제거 | 대체 |
|---|---|
| `zlink_send_part` | `zlink_send` |
| `zlink_send_part_rid` | `zlink_send_rid` |
| `zlink_request_part` | `zlink_request` |
| `zlink_reply_part` | `zlink_reply` |
| `zlink_publish_part` | `zlink_publish` |
| `zlink_router_recv_part` | `zlink_router_recv` |
| `zlink_recv_part` | `zlink_recv` |
| `zlink_subscribe_part` | `zlink_subscribe` |
| `zlink_xpub_recv_part` | `zlink_xpub_recv` (**이름만** 변경, 계약 불변) |

**계약은 유지하되 이름만 바꾸는 것 1개**

```c
/* :317 — zlink_msg_t를 받지 않는다. 구독 이벤트(subscribed + topic bytes) 리더이며
 * 단일 프레임이다. 통합할 multipart가 없다. 계약과 인자는 그대로 두고
 * 이름만 zlink_xpub_recv로 바꾼다. */
ZLINK_EXPORT zlink_recv_result_t zlink_xpub_recv_part (void *xpub_,   /* → zlink_xpub_recv */
                                                       const zlink_routing_id_t **source_rid_out_,
                                                       int *subscribed_out_,
                                                       char *topic_id_buf_,
                                                       size_t topic_id_capacity_,
                                                       size_t *topic_id_len_out_,
                                                       zlink_recv_flags_t flags_);
```

이름에 `_part`가 남으면 "part 단위 API"라는 오해를 부른다. 실제로는 part 개념이 없는 함수이고,
다른 심볼이 모두 `_part` 없는 이름이 되는 상황에서 이것만 남으면 왜 예외인지 매번 설명해야 한다.
**계약·인자·동작은 전혀 바뀌지 않는다. 이름만 바꾼다.**

`zlink_stream_recv_packet`은 이름을 바꾸지 않는다. `_part`가 없고 `_packet`이 header/body 2슬롯이라는
실제 의미를 정확히 담고 있다.

`zlink_stream_recv_packet`(:328)도 유지한다. header/body 고정 2슬롯 framing이라 "record = parts 배열"과
의미가 다르다(§7.2 Q3). STREAM send는 단일 part 계약이므로 신설 API에서 `part_count_ == 1`만 허용하거나
기존 단일 msg 시그니처를 유지한다.

**함께 사라지는 타입**: `zlink_part_flag_t`(MORE/FINAL). send에서는 배열 순서가, recv에서는
`part_count_out_`이 그 역할을 대신한다. 공개 헤더에서 이 타입이 없어지는지, 아니면 내부에만 남는지는
구현 단계에서 확정한다.

### 7.3.2 이미 whole-message인 선례 — `zlink_completion_t`

`core/include/zlink/socket/api.h:62-65`:

```c
    /* Core-owned contiguous REQUEST reply array; release with
       zlink_multipart_close. */
    zlink_msg_t *reply_parts;
    size_t reply_part_count;
```

REQUEST 응답은 **지금도 파트 배열로 한 번에** 돌아온다. 해제 헬퍼 `zlink_multipart_close`도 이미 공개돼
있다. 그러므로 신설 API는 새 관용을 도입하는 것이 아니라 **이미 공개된 관용을 send와 나머지 recv에
맞추는 것**이다. 지금은 응답만 배열이고 요청은 파트 단위라 방향이 어긋나 있다.

### 7.4 신설 send 시그니처 (초안)

recv와 같은 관용을 따른다 — caller-제공 배열, `parts_capacity_`, `part_count_out_`. 한 번의 호출이 record
하나를 원자적으로 제출하며 part 중간 상태가 남지 않는다. **모든 슬롯의 msg는 호출이 소비한다**(현재
`zlink_send_part`의 "Every call consumes part_"와 같다). 부분 소비는 없다 — 실패하면 전부 소비되지 않았거나
전부 소비된 것 중 하나이며, 어느 쪽인지는 결과 코드가 정한다(§7.4의 계약 3).

```c
/* PAIR·DEALER: record 하나(모든 part)를 한 번에 제출. parts_는 caller-제공 배열(길이 part_count_).
 * 성공·backpressure 판정은 record 단위다. 부분 제출 상태는 남지 않는다. */
ZLINK_EXPORT zlink_submit_result_t
zlink_send (void *s_,
            zlink_msg_t *parts_, size_t part_count_,
            zlink_send_flags_t flags_,
            void *user_context_,
            zlink_completion_id_t *completion_id_out_);

/* ROUTER: 대상 RID를 지정해 record 하나를 제출. */
ZLINK_EXPORT zlink_submit_result_t
zlink_send_rid (void *s_,
                const zlink_routing_id_t *target_rid_,
                zlink_msg_t *parts_, size_t part_count_,
                zlink_send_flags_t flags_,
                void *user_context_,
                zlink_completion_id_t *completion_id_out_);

/* REQUEST: record 하나를 제출하고 reply를 기다리는 완료를 등록한다.
 * 현재 zlink_request_part의 "MORE는 timeout_ms_ == 0, user_context_ == NULL" 제약이 사라진다 —
 * record가 한 번에 제출되므로 timeout·context를 나눠 줄 이유가 없다. */
ZLINK_EXPORT zlink_submit_result_t
zlink_request (void *s_,
               const zlink_routing_id_t *target_router_rid_or_null_,
               zlink_msg_t *parts_, size_t part_count_,
               zlink_send_flags_t flags_,
               uint32_t timeout_ms_,
               void *user_context_,
               zlink_completion_id_t *completion_id_out_);

/* REPLY: 받은 record의 reply token으로 응답 record 하나를 제출. */
ZLINK_EXPORT zlink_submit_result_t
zlink_reply (void *s_,
             const zlink_routing_id_t *source_rid_,
             zlink_reply_token_t reply_token_,
             zlink_msg_t *parts_, size_t part_count_);

/* PUBLISH: topic으로 record 하나를 발행. */
ZLINK_EXPORT zlink_submit_result_t
zlink_publish (void *subject_,
               const char *topic_id_,
               zlink_msg_t *parts_, size_t part_count_,
               zlink_send_flags_t flags_);
```

대응하는 recv 쪽 추가분(§3.3의 두 개에 더한다):

```c
/* SUB·XSUB: topic과 record 하나(모든 part)를 함께 수신. zlink_subscribe_part를 대체한다.
 * topic 버퍼 관용은 기존과 같다(capacity + len_out). */
ZLINK_EXPORT zlink_recv_result_t
zlink_subscribe (void *sub_,
                 const zlink_routing_id_t **source_rid_out_,
                 char *topic_id_buf_, size_t topic_id_capacity_, size_t *topic_id_len_out_,
                 zlink_msg_t *parts_out_, size_t parts_capacity_, size_t *part_count_out_,
                 zlink_recv_flags_t flags_);
```

**이름**: 기존 이름에서 `_part`만 떼는 형태로 맞춘다 — `send_part`→`send`, `send_part_rid`→`send_rid`,
`request_part`→`request`, `reply_part`→`reply`, `publish_part`→`publish`, `recv_part`→`recv`,
`router_recv_part`→`router_recv`, `subscribe_part`→`subscribe`. §3.3의 "이름 확정 검토"는 이 규칙으로 닫는다.

**STREAM**: `zlink_stream_send`는 단일 part 계약이므로(§7.2 Q3) `part_count_ == 1`만 허용하거나 기존 단일 msg
시그니처를 유지한다. 유효한 RID로 보내는 길이 0 part의 "peer 끊기" 의미를 보존해야 한다.

**바뀌는 것 하나 더**: `zlink_part_flag_t`(MORE/FINAL)는 send 경로에서 사라진다. recv 쪽 `has_more_out_`도
`part_count_out_`로 대체된다. 즉 공개 표면에서 part flag 개념 자체가 없어진다.

### 7.4.1 바인딩 매핑 — send도 "내부 구현"만 바뀐다 (공개 시그니처 불변)

§4가 recv에 대해 정한 것과 **완전히 같다.**

- 각 바인딩의 공개 send·request·reply·publish는 이미 **parts 컬렉션을 받는 형태**다. 사용자는 파트 목록을
  builder에 얹고 한 번 `submit()`한다(`.message(a).message(b).submit()`). **공개 시그니처는 바뀌지 않는다.**
- **바뀌는 것은 그 함수의 내부 구현뿐**이다. 지금은 바인딩이 파트마다 `part_flag_`를 계산해 Core를
  `part_count_`번 호출하는 루프를 돈다. 신설 후에는 **Core whole-message send를 1회 호출**한다
  (루프·파트별 경계 왕복·flag 계산 제거).
- **언어별 내부 변경 지점**(적용 plan §10.3의 6단계 대상):

  | 바인딩 | send 내부 루프 위치(변경 대상) |
  |---|---|
  | Java | `bindings/java/src/main/java/systems/zlink/runtime/sockets/SocketSendPlane.java:372-383`(`submitBlockingParts`), `:398-412`(`submitNoWaitPartsAttempt`), `:430-440` |
  | .NET | `bindings/dotnet/src/Zlink/Runtime/Messaging/RequestReplySupport.cs:241` 및 같은 파일의 part 루프 |
  | Rust | `bindings/rust/src/runtime/messaging/operations/send_ops.rs:100,128-147,366-383`(`submit_part_sequence`) |
  | Python | `bindings/python/src/zlink/_native/_zlink_native.c:729-732,875,1114,1237,1293,1333`(part flag 계산 `:199`) |
  | C++·Node·Go | 각 언어 send 경로의 동일 루프(적용 plan에서 파일:줄 확정) |

- **없어지는 것**: 파트마다 하는 `part_flag_` 계산, 파트별 네이티브 경계 왕복, 파트 도중 실패 시의 부분 상태
  처리. 재시도는 record 단위 한 번이 된다.
- **유지되는 것**: 각 파트 msg의 소유권 이전 의미(제출이 소비한다), 단건 fast path, 공개 오류 분류.
- 이득의 크기: .NET 진단에서 메시지당 네이티브 경계가 14회로 집계됐다. send·recv를 모두 1회로 줄이면
  그 대부분이 사라진다. perf 하네스는 이미 공개 API를 쓰므로 **재작성 없이 자동 반영**된다.

### 7.5 결정해야 할 것 셋

1. **제거 범위**: `zlink_reply_part`·`zlink_publish_part`·`zlink_subscribe_part`까지 포함한다(§3의 6개 목록은
   불완전하다).
2. **`zlink_stream_recv_packet`은 유지**한다. header/body 고정 2슬롯이라 일반 배열 API와 의미가 다르다.
3. **`count > capacity` 계약을 정한다.** 지금의 드레인 루프에는 이 실패가 없다. record를 잃지 않으면서
   필요한 개수를 알려주는 방식이어야 한다(§3의 capacity 초과 완료 조건과 같은 규칙).

### 7.6 왜 지금인가

1.0 전이라 제거가 가능하다. 이후에는 호환성 문제가 된다. 그리고 표면이 하나가 되면
"한 record는 같은 thread" 제약과 미완성 record 상태, 그로 인한 `BUSY`·재시도 규칙이 함께 사라진다.
