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
- 공개 C ABI 추가 → 되돌리기 어려움. 시그니처·capacity·원자성 규칙을 spec에서 먼저 확정한다.
- **record 원자성**: whole-message는 한 record를 한 번에 소비. capacity 부족·DONTWAIT 도중 데이터 없음 시 **프레임 상태(보존/폐기)** 규칙을
  명확히(부분 소비로 시퀀스가 어정쩡하게 남지 않도록).
- `recv_part`와의 상태 상호작용 규칙 명시(혼용 시 EBUSY 등).
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
