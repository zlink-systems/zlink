---
title: "Protocol — ZMP v1.0"
---

[English](https://zlink-systems.github.io/zlink/spec/core/protocol/01-zmp/) | 한국어

<!-- zlink-nav:start -->
[프로토콜 목차](README.ko.md) | [이전: 프로토콜 개요](README.ko.md) | [다음: RAW (STREAM) 프로토콜 상세](02-raw.ko.md)
<!-- zlink-nav:end -->

# Protocol — ZMP v1.0

> **이 장이 정의하는 것** — ZMP wire protocol과 request-reply header metadata의 byte 단위 배치,
> handshake와 decode 검증 계약, 그리고 그 byte를 만드는 encode/decode 내부 구현.
> 소개는 [ZMP protocol 가이드](../../../guide/zmp-protocol.ko.md)가 다룬다.

## 1. ZMP 개요

ZMP(zlink Message Protocol)는 zlink 전용 wire protocol이다 — message를 주고받는
endpoint인 [socket](../glossary.ko.md#socket)들이 transport 위에서 교환하는 byte의
배치를 정한다. wire 위에서 전송되는 하나의 데이터 단위를 frame이라 한다.

ZMP는 raw socket handshake, request-reply와 connection control frame만 정의한다.
Application service topology나 stateful object protocol을 포함하지 않는다.

이 문서에서 frame header, handshake, request-reply metadata의 byte 배치와 decode 검증 규칙은 다른
구현이 상호운용을 위해 의존하는 **계약 서술**이다. 그 byte를 만들고 해석하는
encode/decode 경로와 pending 관리는 **구현 서술**이며 [§9 내부 구조](#9-내부-구조)에
모은다.

관련 문서는 다음과 같다.

| 관련 내용 | 문서 |
|---|---|
| ZMP protocol 소개와 사용 관점 | [ZMP protocol 가이드](../../../guide/zmp-protocol.ko.md) |
| ZMP framing 없이 연결하는 RAW wire format | [RAW (STREAM) 프로토콜 상세](02-raw.ko.md) |
| message lifecycle와 multipart 공개 계약 | [Message](../02-message.ko.md) |

## 2. 기본 방향

Request-reply 정보는 첫 application data frame의 ZMP header에 기록한다. Request,
reply와 error reply를 구분하는 kind와 0이 아닌 request sequence는 transport protocol이
소유하며 application payload part가 아니다. 따라서 같은 multipart를 ordinary send와
request에 넘기면 수신 application은 같은 part 수, 순서와 byte를 관찰한다.

전용 request·reply API는 Core가 소비하는 첫 application message에 내부 metadata를 붙인다.
Encoder는 이를 wire header로 옮기고 decoder는 다시 message 내부 값으로 복원한다. Socket
runtime은 request 대상이나 pending completion을 찾은 뒤 public receive와 callback에 payload를
넘기기 전에 metadata를 제거한다.

- **Public message API는 request-reply kind나 sequence를 만들거나 읽지 않는다.** Application은
  request·reply API에 payload만 전달하며 raw send는 항상 ordinary data를 만든다.
- **Application payload 앞에 protocol part를 붙이지 않는다.** Protocol id, version, message
  type과 sequence처럼 보이는 payload도 그대로 application data로 처리한다.
- **`VERSION == 0x01` peer는 이 문서의 header 배치만 request-reply 계약으로 해석한다.**
  Protocol id, version, message type과 sequence 모양의 네 payload part는 ordinary data이며
  request-reply metadata로 해석하지 않는다.

## 3. 공통 frame header

모든 ZMP frame은 다음 8 byte header로 시작한다.

### 3.1 Header 배치

```text
 Byte:   0         1         2         3         4    5    6    7
      +---------+---------+---------+---------+---------------------+
      |  MAGIC  | VERSION |  FLAGS  |  KIND   |   PAYLOAD SIZE      |
      |  (0x5A) |  (0x01) |         |         |   (32-bit BE)       |
      +---------+---------+---------+---------+---------------------+
```

| 필드 | 오프셋 | 크기 | 설명 |
|------|--------|------|------|
| MAGIC | 0 | 1 | `0x5A` |
| VERSION | 1 | 1 | `0x01` |
| FLAGS | 2 | 1 | frame flag |
| KIND | 3 | 1 | `0x00` data, `0x01` request, `0x02` reply, `0x03` error reply |
| PAYLOAD SIZE | 4-7 | 4 | Sequence extension을 제외한 application payload 크기, Big Endian |

Ordinary data frame은 이 8 byte header 바로 뒤에 payload가 온다. Request-reply kind인 첫
frame은 공통 header 뒤에 8 byte unsigned Big Endian request sequence를 붙여 16 byte
header를 만든다.

```text
 Byte:   0         1         2         3         4    5    6    7
      +---------+---------+---------+---------+---------------------+
      |  MAGIC  | VERSION |  FLAGS  |  KIND   |   PAYLOAD SIZE      |
      +---------+---------+---------+---------+---------------------+
      |                 REQUEST SEQUENCE (64-bit BE)                |
      +-------------------------------------------------------------+
      |                    APPLICATION PAYLOAD ...                  |
      +-------------------------------------------------------------+
```

Sequence extension은 `KIND`가 request, reply 또는 error reply일 때만 존재한다. `PAYLOAD
SIZE`와 message 크기 제한은 extension을 포함하지 않는다.

### 3.2 FLAGS bit

| 비트 | 이름 | 값 | 설명 |
|------|------|-----|------|
| 0 | MORE | `0x01` | multipart 계속 |
| 1 | CONTROL | `0x02` | control part |
| 2 | IDENTITY | `0x04` | routing id 관련 frame |
| 3 | SUBSCRIBE | `0x08` | 구독 요청 |
| 4 | CANCEL | `0x10` | 구독 취소 |

ZMP `CONTROL` bit는 HELLO/READY 같은 protocol control frame에만 쓴다. Request-reply
kind는 `CONTROL`, `IDENTITY`, `SUBSCRIBE` 또는 `CANCEL`과 함께 사용할 수 없다.

수신 decoder는 FLAGS의 bit 5~7이 설정된 frame을 `EPROTO`로 거부한다. 다음 FLAGS
조합도 `EPROTO`다.

- `CONTROL | IDENTITY`
- `CONTROL | MORE`
- `SUBSCRIBE | CANCEL`
- `SUBSCRIBE` 또는 `CANCEL`과 그 밖의 flag를 함께 설정한 조합

## 4. Handshake

연결이 만들어지면 active 쪽은 stream transport에서 HELLO와 READY frame을 한 outbound
buffer로 보낸다. Message 경계가 있는 WS·WSS에서는 두 frame을 각각 하나의 binary message로
보낸다. Paired DEALER·ROUTER transport의 passive 쪽은 HELLO만 먼저 보내고, peer READY를
수신한 뒤 자기 READY를 보낸다. Passive 쪽은 이 READY의 transport write가 성공적으로
완료된 뒤에만 local connection readiness와 pair admission을 공개한다. READY write가
실패하면 handshake를 실패로 끝내며 readiness를 공개하지 않는다. Active 쪽은 passive
READY를 수신한 뒤에만 data 교환을 시작한다.

```mermaid
sequenceDiagram
    participant A as Active peer
    participant P as Passive peer

    A->>P: HELLO + READY (stream은 한 buffer, WS·WSS는 message 둘)
    P->>A: HELLO
    Note over P: peer READY 수신·검증
    P->>A: READY
    Note over P: READY write 완료 후 local readiness 공개
    Note over A: passive READY 수신·검증 후 data 교환 시작
```

**HELLO frame**: control type(1 byte), socket type(1 byte),
routing ID 길이(1 byte), routing ID(0~255 byte) 순서로 구성한다. routing ID는
transport peer를 식별하는 byte 열이다.

**READY frame**: READY control type byte `0x04`는 항상 전송한다. Metadata를 사용하면
그 뒤에 property를 연속해서 붙이며, 각 property의 byte 배치는 다음과 같다.

```text
[name length:u8][name bytes][value length:u32 BE][value bytes]
```

`ZLINK_OPT_ZMP_METADATA`를 활성화하면 `Socket-Type`을 추가하고, socket type이
DEALER 또는 ROUTER일 때만 `Routing-Id`도 추가한다. Metadata를 사용하는 READY에는
`Zlink-Max-Message-Size`를 항상 추가하며, 값은 unsigned 64-bit big-endian 8 byte다.
이 option의 기본값은 비활성화다. 다만 paired DEALER·ROUTER transport는 이 option과
관계없이 metadata와 [§4.1](#41-request-reply-transport-pair)의 pair property를 항상
추가한다.

**ERROR frame**: ERROR control type은 `0x05`다. Body는 다음 byte 순서로 구성한다.

```text
[type:0x05][error code:u8][reason length:u8][reason bytes]
```

### 4.1 Request-reply transport pair

Request-reply를 사용하는 하나의 logical DEALER/ROUTER peer는 두 physical transport
connection을 사용한다.

| Lane | 전달하는 traffic |
|---|---|
| Application | 일반 application message와 request |
| Completion | 이미 보낸 request를 완료하는 reply와 receive-flow control |

두 connection의 READY frame에는 `Zlink-Pair-Id`, `Zlink-Pair-Generation`,
`Zlink-Lane`이 들어간다. Pair ID와 generation은 unsigned 64-bit big-endian 값이다.
Lane은 한 byte이며 Application은 `0`, Completion은 `1`이다. 세 property는 항상
함께 있어야 한다. 두 connection의 pair ID, generation과 peer routing identity가
모두 일치해야 한다.

READY의 `Routing-Id`는 두 connection이 같은 peer에 속하는지 검증하는 metadata다. Runtime은
ROUTER가 peer를 선택하는 synthetic routing-id preamble을 Application lane에만 제공한다.
Completion lane은 이 preamble과 ordinary `data` record를 전달하지 않는다. READY 뒤
Completion lane에 record가 들어오면 첫 record는 reply, error reply 또는 receive-flow
control이어야 한다.
Peer-weight advertisement는 Application lane scheduling만 제어하며 Application lane에만
보낸다. Peer-weight advertisement는 Completion-lane record가 아니다.

Application write는 두 lane의 검증이 끝날 때까지 대기한다. 이전 generation에서
수신한 data를 새 pair에 연결하지 않는다. 한 lane에서 protocol error, identity
mismatch, fence timeout 또는 terminal failure가 발생하면 pair 전체를 종료한다.
Reconnect는 새 generation을 만들고 두 lane을 다시 검증한 뒤 Application write를
재개한다.

FIFO 순서는 각 lane 안에서만 보장한다. 두 lane 사이의 순서는 보장하지 않는다.
Application ingress가 수신 처리가 밀려 추가 제출을 제한하는
[backpressure](../glossary.ko.md#backpressure)로 중단되어도 Completion reply를
처리할 수 있다. Relocation, session binding과 그 밖의 상위 protocol은 두
connection 사이의 순서에 의존하지 않고 자체 generation fence를 사용해야 한다.

## 5. Request-reply kind와 sequence

첫 application data frame의 `KIND`와 sequence extension이 request-reply record를
식별한다.

| Kind | 값 | Sequence | Application에서 관찰하는 결과 |
|---|---:|---|---|
| data | `0x00` | 없음 | Payload를 ordinary message로 받는다. |
| request | `0x01` | 0이 아닌 8 byte Big Endian 값 | Typed receive가 payload와 reply에 사용할 sequence 또는 local token을 반환한다. |
| reply | `0x02` | 원래 request와 같은 값 | Completion progress lane이 해당 pending request를 payload로 완료한다. |
| error reply | `0x03` | 원래 request와 같은 값 | Completion progress lane이 첫 payload part의 errno를 오류 completion으로 바꾼다. |

Multipart request-reply는 첫 application frame에만 request-reply kind와 sequence를
기록한다. 첫 frame의 `MORE`가 설정되면 둘째 frame부터 `KIND == 0x00`이고, 마지막
application frame에서 `MORE`가 해제된다.

```text
[REQUEST + SEQUENCE + MORE][payload part 0]
[DATA                    ][payload part 1]
...
```

`request_sequence == 0`은 유효하지 않다. Reply는 request에서 받은 wire sequence를
그대로 사용한다. Error reply는 receive-only kind이며 public sender는 없다. 첫
application payload part는 0이 아닌 errno를 4 byte Big Endian 값으로 담는다. Core C
completion callback은 errno를 `zlink_request_result_t`로 매핑하고 이 첫 part를 제외한 나머지
part를 callback payload로 받는다. 상위 language binding의 error 변환과 payload 처리는
[Binding 스펙](../../../../../bindings/doc/spec/README.ko.md#request-reply-오류-정책)이 소유한다.
첫 part가 없거나 크기가 4 byte가 아니거나 값이 `0`이면
result는 `ZLINK_REQUEST_PROTOCOL_ERROR`이고 callback payload part 수는 `0`이다.

### 5.1 Request-reply sequence (DEALER → ROUTER)

```mermaid
sequenceDiagram
    participant D as DEALER
    participant R as ROUTER

    D->>D: request_seq=N 할당
    D->>D: 첫 payload에 request kind와 sequence N 연결
    D->>R: [REQUEST + sequence N][application payload]
    R->>R: header metadata 복원 → (source_node_rid, request_seq=N, payload)
    R->>R: public 전달 전에 metadata 제거
    R->>R: router_handler로 dispatch
    R->>R: 첫 reply payload에 reply kind와 sequence N 연결
    R->>R: routing_id로 Completion pipe 선택 (local key)
    R->>D: [REPLY + sequence N][application reply payload]
    D->>D: pending[seq=N] 매칭 → reply_handler 호출
```

이 diagram에서 wire에 나타나는 header 배치는 위 kind와 sequence 규칙이 계약이다. `routing_id`는
reply wire part가 아니라 ROUTER가 대상 Completion pipe를 고르는 local 선택 key다.
Pending 매칭과 handler dispatch는 [§9 내부 구조](#9-내부-구조)의 구현 서술이다.

## 6. Transport routing_id와의 관계

transport `routing_id`와 request-reply 주소는 같은 값이 아니다.

- transport `routing_id`: 현재 연결된 peer를 선택하는 ROUTER local key이며 reply wire에는
  포함되지 않는 주소
- `request_seq`: request와 reply를 묶는 식별자

둘을 섞으면 reply 주소를 잘못 계산하게 된다.
문서와 구현 모두 이를 다른 계층으로 설명해야 한다.

## 7. Decode 유효성 검사

수신 peer는 공통 header와 sequence extension에 다음 조건을 적용한다.

- `KIND`가 data, request, reply 또는 error reply 중 하나다.
- Request-reply kind의 sequence가 `0`이 아니다.
- Request-reply kind에 `CONTROL`, `IDENTITY`, `SUBSCRIBE` 또는 `CANCEL`이 없다.
- Application multipart의 둘째 frame부터 request-reply kind나 special frame이 나타나지 않는다.
- Stream EOF 전에 base header, sequence extension과 선언한 payload가 모두 끝난다.

Base header나 sequence extension이 여러 transport read로 나뉘는 것은 오류가 아니다.

검증 실패는 `EPROTO`로 pair를 종료하고 해당 frame을 handler나 reply completion에 전달하지
않는다. 이 frame 자체가 pending request를 완료하지 않으며, pair 종료로 기존 pending request가
disconnect 결과를 받는 규칙은 유지한다.

## 8. Transport framing

TCP, IPC와 TLS는 같은 ZMP header와 payload byte를 연속된 stream으로 보낸다. Base header,
sequence extension과 payload는 여러 read로 나뉠 수 있다.

WS와 WSS는 RFC 6455 binary message 하나에 정확히 ZMP frame 하나를 넣는다. Request-reply
frame의 16 byte header와 application payload는 같은 binary message에 있어야 한다. Binary
message가 base header, sequence extension 또는 payload 중간에서 끝나거나 ZMP frame 둘을
포함하면 `EPROTO`다. HELLO와 READY도 message transport에서는 각각 하나의 binary message를
차지한다. Text message는 payload가 유효한 HELLO, READY 또는 data frame byte와 같아도
ZMP frame이 아니며 peer는 ZMP parsing 전에 `EPROTO`로 연결을 종료한다.

Inproc은 wire codec을 거치지 않지만 pipe와 queue가 같은 internal metadata를 보존한다. Public
receive와 completion에서 관찰하는 payload, sequence와 metadata 제거 결과는 network transport와
같다.

## 9. 내부 구조

> **이 절의 계약 소유** — 상호운용이 의존하는 byte 배치와 decode 검증은
> [§3](#3-공통-frame-header)~[§8](#8-transport-framing)이, 관찰 가능한 완료 동작은
> [§10 검증 요구](#10-구현-및-contract-test-검증-요구)가 소유한다. 이 절은 그 byte를
> 만들고 해석하는 현재 구현을 서술한다.

### Encode / decode 흐름 (socket request-reply)

송신 runtime은 Core가 소유한 첫 application message의 16 byte inline auxiliary 영역에 kind와
host-order sequence를 둔다. Group과 request-reply metadata는 같은 영역을 공유하므로 동시에
설정하지 않는다. `sizeof(msg_t) == 64`와 29 byte VSM 한계는 유지한다.

모든 ZMP encoder는 caller가 제공한 고정 storage에 최대 16 byte를 쓰는 같은 header builder를
사용한다. Ordinary data는 tag 확인 뒤 기존 8 byte header와 payload pointer를 그대로 사용하고,
request-reply 첫 frame만 sequence extension을 scatter/gather entry에 추가한다. Header 생성 때문에
heap buffer나 payload copy를 만들지 않는다.

Decoder는 application payload storage와 queue 수용 공간을 확보하기 전에 base header와 sequence
extension을 끝까지 모으고 검증한다. Header가 여러 transport read로 나뉘면 읽은 byte와 상태를
유지해 다음 read에서 계속한다. 검증 뒤 application payload 크기로 HWM admission을 수행하며,
backpressure 뒤 재시도할 때 header를 다시 읽지 않는다.

Decoder는 validation, admission, payload와 submission 상태를 차례로 진행한다. 첫 application
message에 metadata를 복원한 뒤 socket runtime이 다음처럼 처리한다.

1. Typed receive의 request는 source pipe와 wire sequence를 reply target으로 저장한다.
2. Completion progress lane의 reply와 error reply는 sequence로 pending request를 찾는다.
3. 필요한 값을 runtime state로 옮긴 뒤 public payload를 내보내기 전에 metadata를 제거한다.

Reply payload는 Completion pipe에서 등록 callback으로 바로 이동한다. 숨은 PAIR
receive queue나 두 번째 completion payload deque에 보관하지 않는다. Timeout,
shutdown과 같은 terminal callback에는 payload가 없는 작은 callback metadata queue만
유지한다. 이 queue는 transport lane이나 wire record가 아니다.

### Pending과 reply-target key

pending(응답 대기 항목) 소유권은 상위 API 계층에 있다. Outbound request의 pending map은
socket이 발급한 `request_seq`로 항목을 찾고, 별도 local cookie로 sequence 재사용을 fence한다.
Completion frame을 적용할 때는 frame을 받은 transport pair ID와 generation도 등록 당시 값과
같아야 한다.

Inbound request의 reply target은 public receive 역할에 따라 다르게 보관한다.

- `DEALER`: socket-local reply token → source pipe와 wire `request_seq`
- `ROUTER`: source routing ID와 wire `request_seq` → request를 전달한 source pipe

완료 규칙(첫 reply 완료, timeout, 중복 reply 무시, error reply 전달)은
[§10 검증 요구](#10-구현-및-contract-test-검증-요구)가 소유한다.

### WebSocket 구현

WebSocket framing 구현은 Beast가 보고하는 message 완료 상태와 최초 data frame의 binary
opcode를 transport adapter에서 decoder까지 함께 전달한다. Short read는 message 경계로
추측하지 않으며 text opcode는 HELLO parsing이나 data frame decode 전에 거부한다.

## 10. 구현 및 contract test 검증 요구

다른 구현과의 상호운용은 wire에서 관찰하는 byte로, request-reply 완료는 공개 API의
callback 결과로 확인한다. 각 항목은 test 하나로 이어진다.

**Frame header와 flags**
- Ordinary data를 보내면 wire에서 MAGIC `0x5A`, VERSION `0x01`, KIND `0x00`, 32-bit Big Endian application payload size를 담은 8 byte header가 관찰된다.
- Public request와 reply의 첫 frame은 KIND `0x01` 또는 `0x02`와 0이 아닌 8 byte Big Endian sequence를 담은 16 byte header를 사용하며, payload size는 sequence extension을 제외한다.
- Multipart request-reply의 둘째 frame부터 KIND는 `0x00`이고 `MORE`가 application part 경계를 그대로 나타낸다.
- FLAGS bit 5~7, `CONTROL | IDENTITY`, `CONTROL | MORE`, `SUBSCRIBE | CANCEL`, SUBSCRIBE/CANCEL과 다른 flag의 조합을 수신하면 decoder가 `EPROTO`로 거부한다.

**Handshake**
- Active 쪽은 stream transport에서 HELLO와 READY를 한 outbound buffer로 보내고, WS·WSS에서는 두 binary message로 보낸다. Paired DEALER·ROUTER transport의 passive 쪽은 HELLO를 먼저 보낸 뒤 peer READY 수신 후 자기 READY를 보낸다.
- Paired passive 쪽은 자기 READY의 transport write가 완료되기 전에 local readiness나 pair admission을 공개하지 않는다. Write가 실패하면 readiness 없이 handshake가 실패한다.
- READY control type `0x04` 뒤의 각 metadata property는 `[name length:u8][name bytes][value length:u32 BE][value bytes]` 배치를 따른다.
- `ZLINK_OPT_ZMP_METADATA`가 기본값(비활성)이면 metadata property가 없고, 활성화하면 `Socket-Type`과 8 byte big-endian `Zlink-Max-Message-Size`가 추가된다. `Routing-Id`는 DEALER·ROUTER READY에만 추가된다.
- paired DEALER·ROUTER transport의 READY에는 이 option과 관계없이 `Socket-Type`·`Routing-Id` metadata와 pair property가 항상 있다.
- ERROR control type은 `0x05`이며 body는 `[type][error code:u8][reason length:u8][reason bytes]` 배치를 따른다.

**Transport pair**
- 두 connection의 READY에 `Zlink-Pair-Id`(unsigned 64-bit big-endian), `Zlink-Pair-Generation`(unsigned 64-bit big-endian), `Zlink-Lane`(1 byte, Application `0` / Completion `1`) 세 property가 항상 함께 나타난다.
- Application write는 두 lane의 검증이 끝난 뒤에 전달된다.
- 이전 generation에서 수신한 data는 새 pair에 연결되지 않는다.
- 한 lane에서 protocol error, identity mismatch, fence timeout 또는 terminal failure가 발생하면 pair 전체가 종료된다.
- reconnect하면 새 generation이 만들어지고, 두 lane을 다시 검증한 뒤 Application write가 재개된다.
- FIFO 순서는 각 lane 안에서만 관찰되며, 두 lane 사이의 순서는 보장되지 않는다.
- Application ingress가 backpressure로 중단된 동안에도 Completion reply가 처리된다.
- Network paired connection에 첫 request를 보내기 전에 completion poller를 등록해도 pair가 종료되지
  않으며, 이어지는 request와 reply가 각각 한 번 전달된다.
- Inproc pair에서 application request를 받은 뒤 reply나 receive-flow control을 쓰기 전까지
  Completion pipe에는 읽을 수 있는 synthetic routing-id record가 없다.
- Inproc pair가 ready 상태가 된 뒤 peer weight를 변경해도 Completion-lane record가 추가되거나
  pair가 종료되지 않으며, 이어지는 request와 reply가 각각 한 번 전달된다.

**Request-reply와 decode 검증**
- Ordinary send와 request에 같은 multipart를 넘기면 수신 application은 같은 part 수, 순서와 byte를 관찰한다.
- Protocol id, version, message type과 sequence 모양의 네 payload part를 ordinary send로 보내도 전체 payload가 그대로 전달되고 request completion을 만들지 않는다.
- reply의 `request_seq`는 request에서 받은 값과 같다.
- ROUTER가 reply 대상을 고르는 `routing_id`는 local 선택 key이며 reply wire part가 아니다.
- `error reply`의 첫 payload part는 4 byte Big Endian errno다.
- 알 수 없는 kind, `request_seq == 0`, request-reply kind와 special flag의 조합, multipart 중간 kind 또는 special frame을 수신하면 `EPROTO`로 pair를 종료하고 payload를 handler나 completion에 전달하지 않는다.
- Base header, sequence extension 또는 payload를 끝내지 않고 stream을 닫으면 `EPROTO`이며 application receive나 completion에 부분 payload를 전달하지 않고 connection을 종료한다.

**완료**
- 첫 reply 1건으로 high-level request가 완료된다.
- 완료 후 같은 key로 추가 reply가 와도 다시 callback하지 않는다.
- reply보다 timeout이 먼저 오면 callback은 `ZLINK_REQUEST_TIMED_OUT`을 받는다.
- 유효한 `error reply`는 Core C callback에 매핑된 non-OK `zlink_request_result_t`와 errno part를
  제외한 나머지 payload로 전달된다.

**Transport와 frame 수**
- TCP, IPC, TLS, WS와 WSS에서 request-reply의 kind와 sequence가 같은 byte 위치에 나타난다.
- WS와 WSS의 RFC 6455 binary message 하나는 정확히 ZMP frame 하나를 포함하며, frame 중간에서 끝나거나 frame 둘을 포함한 message는 `EPROTO`다.
- 유효한 HELLO 또는 data frame byte를 WS·WSS text message로 보내면 payload가 공개되지 않고
  peer가 `EPROTO`로 연결을 종료한다. Fragmented text message도 최초 opcode를 유지해 같은
  결과를 낸다.
- Inproc request-reply는 network transport와 같은 application payload, sequence, reply 완료와 public metadata 제거 결과를 제공한다.
- Ordinary send와 request/reply에 같은 application multipart를 넘기면 방향별 ZMP data frame 수가 모두 application part 수와 같다.

<!-- zlink-nav:start -->
[프로토콜 목차](README.ko.md) | [이전: 프로토콜 개요](README.ko.md) | [다음: RAW (STREAM) 프로토콜 상세](02-raw.ko.md)
<!-- zlink-nav:end -->
