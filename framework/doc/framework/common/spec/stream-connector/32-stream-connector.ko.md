# Stream Connector — 공통 스펙

[스펙 목차](../README.ko.md) | [이전: Session Actor Dispatch](../20-session-actor-dispatch.ko.md) | [다음: Location Runtime](../21-location-runtime.ko.md)

> 이 문서는 **client stream connector의 언어 중립 정본**이다. 대상 실행 환경, transport,
> wire 계약, packet(header 정보와 payload를 결합한 전송 단위) 모델, 연결 생명주기,
> 오류 의미, 배포 산출물을 소유한다.
>
> 언어별 public 타입과 시그니처는 [`languages/<lang>/`](README.ko.md)가 고정한다 —
> [cpp](languages/cpp/03-stream-connector.ko.md) ·
> [dotnet](languages/dotnet/03-stream-connector.ko.md) ·
> [java](languages/java/03-stream-connector.ko.md) ·
> [typescript](languages/typescript/README.ko.md). 이 문서는
> **무엇을 보장하는가**를 정의하고, 언어별 스펙은 **그 의미가 그 언어에서 어떤 모양인가**를
> 정의한다([공개 계약 관리](../00-public-contract-governance.ko.md)).

## 1. 목적과 범위

Stream Connector는 서버 framework의 **STREAM 모델에 접속하는 client 쪽 라이브러리**다.
서버 session callback이 받는 것과 같은
[packet](../01-glossary.ko.md#stream-packet)(header + payload)을 client에서도 동일하게
주고받게 한다.

[Connector](../01-glossary.ko.md#stream-connector)는 도메인을 포함하지 않는다. 사용자가 그 위에 채팅·게임·장비 제어·알림 같은 자기
protocol을 구성한다.

**의존성 경계:**

- connector package는 **서버 framework package에 의존하지 않는다**(ASP.NET Core adapter,
  SPOT, Stage wrapper 등).
- 의존성은 transport·codec·compression처럼 connector 실행에 필요한 client-side runtime으로만
  한정한다.
- 반대 방향도 같다. 서버 framework package는 connector를 참조하지 않는다.

## 2. 대상 실행 환경

**이 절이 이 스펙의 출발점이다.** 실행 환경의 제약이 계약을 결정하기 때문이다. 어떤
connector를 쓰는지는 **언어가 아니라 "엔진 × 빌드 타깃"** 으로 정해진다.

### 2.1 엔진·환경별 담당 connector

| 대상 | 네이티브 빌드 | 웹 빌드(브라우저·WASM) |
|---|---|---|
| **Unity** | `.NET` connector | **TypeScript** connector — C#이 jslib interop으로 JS 계층을 호출한다 |
| **Godot** | C++ connector(GDExtension) 또는 `.NET` connector(Godot C#) | **TypeScript** connector |
| **Cocos** | C++ connector(Axmol 어댑터) | **TypeScript** connector(Cocos Creator web) |
| **Unreal** | C++ connector(plugin) | (해당 없음) |
| **브라우저 웹 client** | — | **TypeScript** connector |
| **데스크톱·서버 애플리케이션** | `.NET` / Java / C++ connector | — |

**규칙 하나로 요약하면 — 웹(브라우저·WASM)으로 빌드하는 순간 언어와 무관하게 TypeScript
connector를 사용한다.** 브라우저 샌드박스에서 OS 소켓을 열 수 있는 언어가 없기 때문이다.

### 2.2 환경 제약이 계약에 미치는 영향

| 환경 | 제약 | 계약 |
|---|---|---|
| 게임 엔진(공통) | 엔진 객체를 main thread 밖에서 다룰 수 없다 | 수신 callback 실행 문맥을 정하는 dispatch mode의 기본값은 **`Manual`**이다. Main thread에서 명시적으로 pump한다(§7). |
| 게임 엔진(C++) | 예외·coroutine이 비활성인 빌드가 있다 | C++ connector core는 **no-exception·no-coroutine**. public header가 `<coroutine>`을 노출하지 않는다 |
| **브라우저·WASM** | **OS 소켓을 열 수 없다**(보안 샌드박스) | **`tcp`·`tls` 사용 불가.** `ws`·`wss`만 사용하며 플랫폼의 네이티브 WebSocket API 위에서 동작한다(§3.2) |
| Node.js | TypeScript connector의 제품 실행 환경이 아니다 | 서버 process와 browser test runner만 담당한다 |

## 3. Transport

### 3.1 endpoint scheme → transport

| URI scheme | transport |
|---|---|
| `tcp://` | TCP |
| `tls://` | TLS over TCP |
| `ws://` | WebSocket |
| `wss://` | WebSocket over TLS |

transport를 명시했는데 endpoint scheme과 어긋나면 **구성 오류**로 처리한다.

### 3.2 환경별 transport 가용성

| 환경 | 사용 가능한 transport |
|---|---|
| **브라우저 계열**(웹, Cocos web, Unity WebGL, Godot Web) | **`ws`, `wss`만** |
| 네이티브(`.NET`·C++·Java) | `tcp`, `tls`, `ws`, `wss` |

**브라우저 계열이 `tcp://`·`tls://` endpoint를 받으면 구성 오류로 즉시 실패한다.** 런타임에
조용히 실패하지 않는다. 이는 구현 제약이 아니라 플랫폼 제약이다.

브라우저 계열에서 `ws`·`wss`는 **플랫폼의 네이티브 WebSocket API**로 구현한다. 핸드셰이크와
프레이밍을 플랫폼이 수행하므로 connector가 직접 구현하지 않는다.

## 4. Wire 계약

### 4.1 frame

STREAM frame의 앞쪽 2바이트는 `header_size`다.

```text
+----------------+----------------+----------------+----------------+
| u16 header_len | u32 payload_sz | header bytes   | payload bytes  |
+----------------+----------------+----------------+----------------+
```

### 4.2 header

```text
+----------------+---------+----------+----------+------------------+
| format_marker  | kind u8 | codec u8 | flags u8 | request_seq u64? |
| u8 = 0xF2      |         |          |          |                  |
+----------------+---------+----------+----------+------------------+
| name u8+n | meta u16+n? | corr u8+n? | flow_id 36B + origin u8?   |
+-----------+-------------+------------+----------------------------+
```

- **header의 첫 바이트는 `format_marker = 0xF2`다.** 값이 다르면 decode error다.
- `kind`·`codec`은 문자열이 아니라 **1바이트 enum**으로 인코딩한다.
- packet name은 `u8 name_len + UTF-8 bytes`이며 **최대 255바이트**다. **`Response`와 `Error`는
  [packet name](../01-glossary.ko.md#packet-name)을 담지 않는다** — `name_len = 0`으로 인코딩한다. 응답은 handler를 고르지 않고
  상관관계는 `request_seq`가 이미 정하므로 이 필드가 쓰이지 않는다
  ([03 message model](../04-message-model.ko.md)의 "reply 상관관계").
- metadata는 `u16 meta_len + metadata bytes`, correlation id는 `u8 len + bytes`로 이어진다.
- flow 필드는 **36바이트 `flow_id`와 1바이트 `flow_origin`이 항상 함께** 존재하거나 함께 없다.
  의미는 [메시지 흐름 상관관계 §3](../27-flow-correlation.ko.md#3-형식과-소유권)이 소유한다.
- **모든 multi-byte 정수는 network byte order**다.

application code는 이 header를 직접 만들거나 수정하지 않는다. connector runtime이 소유한다.

### 4.3 flags

| flag | 값 | 의미 |
|---|---|---|
| has request seq | `0x01` | `request_seq` 필드가 있다 |
| has metadata | `0x02` | `meta` 필드가 있다 |
| payload compressed | `0x04` | payload가 압축되어 있다 |
| has correlation id | `0x08` | correlation id 필드가 있다 |
| has flow id | `0x10` | `flow_id`·`flow_origin` 필드가 있다 |

`Control` packet에는 `has flow id`를 세우지 않는다
([flow-correlation §3](../27-flow-correlation.ko.md#3-형식과-소유권)).

### 4.4 metadata

```text
+---------------+-------------+-------------+
| count u8      | entry...    | entry...    |
+---------------+-------------+-------------+

entry:
+-------------+-------------+-------------+-------------+
| key_len u8  | key bytes   | val_len u16 | value bytes |
+-------------+-------------+-------------+-------------+
```

key와 value는 UTF-8 문자열이다.

- `key_len`은 1 이상이어야 한다.
- 같은 key가 두 번 등장하면 **decode error**다.
- `count`는 뒤따르는 entry 개수와 일치해야 한다.

**크기 제한은 두 단계다.**

| 단계 | 한도 |
|---|---|
| wire `meta_len` 필드의 표현 한계 | 65535 bytes |
| connector가 전송 전에 검증하는 한도 | **1024 bytes** — 초과 시 validation error. **public option으로 조절하지 않는다** |

metadata는 trace id·locale·tenant id처럼 **작은 값만** 싣는다.

### 4.5 decode error

다음은 모두 decode error다.

- 알 수 없는 `kind`·`codec`·flag bit
- `has request seq`·`has metadata` flag와 실제 필드 존재 여부의 불일치
- `Response` 또는 `Error`의 `name_len`이 `0`이 아닌 경우

### 4.6 control frame

`Control` kind는 connector 내부 control frame이다. **application packet name은 `$zlink.`
prefix를 사용할 수 없다.**

**control frame의 이름 공간은 packet kind로 분리된다.** control frame은 `Control` kind로만
전달되므로, 아래 control 이름과 같은 문자열을 application이 `Send`/`Request` kind로 쓰더라도
dispatch가 섞이지 않는다. 다만 혼동을 피하기 위해 application packet에 `session-closing`을
쓰지 않는다. 신규 control packet은 `$zlink.` prefix를 사용한다.

control frame은 `Raw` codec, request sequence 없음, metadata 없음, flow flag 없음이다.
**payload는 control packet마다 다르다.**

| control packet | payload |
|---|---|
| `$zlink.heartbeat.ping` | **비어 있다** |
| `$zlink.heartbeat.pong` | **비어 있다** |
| `session-closing` | **비어 있지 않다** — 아래 참조 |

`session-closing`은 서버가 세션을 닫기 직전에 보내는 control packet이며, client는 이를 읽어
`closeReason`을 확정한다
([Host Relocate와 Shutdown §9](../28-graceful-drain-handoff.ko.md#9-대기-중인-message-timer와-session을-옮긴다)).

```text
+------------+-------------------+----------------+--------------------+
| version u8 | close_reason u8   | diag_len u16   | diagnostic bytes   |
| = 1        | 1..6              | 0..512         | UTF-8              |
+------------+-------------------+----------------+--------------------+
```

| `close_reason` | 값 |
|---|---|
| `ClientClose` | 1 |
| `IdleTimeout` | 2 |
| `HeartbeatTimeout` | 3 |
| `ServerDrain` | 4 |
| `ProtocolError` | 5 |
| `TransportError` | 6 |

알 수 없는 version·reason, 또는 `diag_len`이 512를 넘거나 실제 payload 길이와 어긋나면
decode error다.

### 4.7 payload 크기 한도

송신과 수신 각각에 payload 한도가 있다. **기본값은 양쪽 모두 64KB(65536 bytes)** 이며,
metadata 한도와 달리 **option으로 조절한다.**

| 방향 | 기본 한도 | 위반 시 |
|---|---|---|
| 송신 | 64KB | **transport write 전에** validation error(§9)로 실패한다 |
| 수신 | 64KB | `FrameTooLarge`(§9) |

**한도는 length prefix와 encoded header를 뺀 payload 바이트에만 적용한다.** 압축 frame을 수신하면
wire의 압축된 payload와 압축 해제 결과를 각각 같은 수신 한도와 비교한다. 어느 쪽이든 넘으면
application handler나 request completion으로 전달하지 않는다. 송신 한도는 실제 transport에 쓰는
payload를 기준으로 하므로 압축을 요청한 송신은 압축 결과를 검사한다. 64KB보다 큰 payload가 필요한
애플리케이션은 이 값을 명시적으로 키운다.

## 5. Packet 모델

사용자 API는 raw header bytes를 다루지 않는다.

- **기본 packet 이름은 payload 타입 이름**이다.
- 호출자가 명시한 이름이 있으면 그쪽이 우선이다.
- 부가 정보가 필요하면 metadata key-value로 덧붙인다.
- **임의 header bytes를 다루는 API를 공개 표면에 두지 않는다.**

### 5.1 message kind

| kind | 값 | 의미 |
|---|---|---|
| Send | 1 | 응답을 기다리지 않는 단방향 packet |
| Request | 2 | 응답을 기다리는 packet |
| Response | 3 | request의 성공 응답 |
| Error | 4 | request의 실패 응답, 또는 request와 무관한 stream 오류 |
| Control | 5 | connector 내부 control frame(§4.6) |

### 5.2 request correlation

`request_seq`는 runtime이 관리하는 `u64` correlation sequence이며 **request·response·error
response에만** 들어간다.

- 같은 connector 인스턴스 안에서 **동시에 pending인 request 사이에 `request_seq`가 중복되면
  안 된다.**
- 값 `0`은 사용하지 않는다.

**매칭 규칙:**

| 상황 | 동작 |
|---|---|
| `Send` 전송 | `request_seq` 없이 보낸다. pending map에 넣지 않는다 |
| `Request` 전송 | 새 `request_seq`를 부여하고 pending map에 등록한다 |
| `Response` 수신 | 같은 `request_seq`의 pending request를 **성공으로 완료**한다 |
| `Error` 수신 — `request_seq` 있음 | 같은 `request_seq`의 pending request를 **실패로 완료**한다 |
| `Error` 수신 — `request_seq` 없음 | pending request와 무관한 **stream 수준 오류**로 error 표면에 전달한다(§9) |

- **pending request 매칭은 `request_seq`가 정본이다.** `Response`와 `Error`에는 **packet name
  필드가 아예 없으므로**(`name_len = 0`) 이름으로 대조할 수도 없다. 어떤 응답인지는 sequence가
  이미 정한다. STREAM session에서 Actor request를 relay할 때도 같은 terminal reply 원칙을 사용한다
  ([Session Actor Dispatch §3](../20-session-actor-dispatch.ko.md#3-inbound-dispatch와-reply)).
- **request timeout·close·disconnect가 발생하면 pending request는 모두 실패로 완료하고 map에서
  제거한다.** 재연결 후 자동 재전송하지 않는다(§6).

### 5.3 error payload

`Error` kind의 payload는 **codec 설정과 무관하게 항상 UTF-8 JSON object**이며,
header의 codec은 `JSON`이다.

```json
{"code":"error_code","message":"message"}
```

애플리케이션 도메인의 오류를 정상 reply로 다루려면 `Error`가 아니라 `Response` kind와 사용자
정의 payload를 사용한다.

### 5.4 codec

| codec | 값 |
|---|---|
| Raw | 0 |
| JSON | 1 |
| MessagePack | 2 |
| Protobuf | 3 |

**JSON이 기본 codec이다.** 모든 언어의 connector는 typed payload codec 하나를 connector 생성
option으로 받으며 typed send, request와 수신에 함께 사용한다. MessagePack·Protobuf는 선택 package가
이 codec 구현을 제공한다. 메시지 타입마다 codec을 등록하거나 send/request operation마다 codec을
바꾸는 public API는 제공하지 않는다. Raw encoded payload는 외부 protocol 연동을 위해 payload가
지정한 codec 번호를 그대로 사용할 수 있다.

TypeScript package root는 browser-safe `ZlinkStreamPayloadCodec`을 내보내며 connector를 만들 때
`codec` option으로 주입한다. Node framework serializer 등록은 같은 package의 `./framework` subpath를
사용한다. 두 진입점은 `stream-wire`가 소유하는 같은 codec 번호를 사용하지만 browser module graph가
server framework runtime을 참조하지 않아야 한다.

## 6. 연결 생명주기

다음 C# 발췌는 연결, 수동 dispatch와 종료가 하나의 connector interface에서 어떻게
보이는지 설명하기 위한 비규범 예시다. 다른 언어에 같은 signature를 요구하지
않으며, 정확한 .NET signature는
[.NET Stream Connector 계약](languages/dotnet/03-stream-connector.ko.md)이 정의한다.

```csharp
public interface IZlinkStreamConnector : IAsyncDisposable
{
    ZlinkStreamConnectionState State { get; }
    IZlinkStreamLifecycleCall Connect { get; }
    IZlinkStreamLifecycleCall Close { get; }
    IZlinkStreamLifecycleCall Dispatch { get; }
    IZlinkStreamRequestCall  Request(ZlinkStreamEncodedPayload payload);
}

public interface IZlinkStreamLifecycleCall
{
    ValueTask Async(CancellationToken cancellationToken = default);
}

public interface IZlinkStreamRequestCall
{
    IZlinkStreamRequestCall PacketName(string name);
    IZlinkStreamRequestCall Timeout(TimeSpan timeout);
    ValueTask<ZlinkStreamEncodedPayload> Async(CancellationToken cancellationToken = default);
}
```

```csharp
await connector.Connect.Async(cancellationToken); // 연결과 receive loop 준비가 끝날 때까지 기다린다.

var reply = await connector
    .Request(payload)
    .PacketName("inventory.get")
    .Timeout(TimeSpan.FromSeconds(5))
    .Async(cancellationToken); // 같은 request sequence의 terminal reply를 기다린다.

await connector.Dispatch.Async(cancellationToken); // Manual mode에서 대기 중인 callback을 현재 문맥에서 처리한다.
await connector.Close.Async(cancellationToken);    // callback 밖에서는 공유 종료 작업이 끝날 때까지 기다린다.
```

| 상태 | 의미 |
|---|---|
| `Created` | Connector를 만들었지만 아직 연결을 시작하지 않은 상태다. |
| `Connecting` | Connector가 초기 연결을 진행하는 상태다. |
| `Connected` | Connector가 연결을 완료한 상태다. |
| `Reconnecting` | Connector가 자동 재연결을 진행하는 상태다. |
| `Disconnected` | Connector의 transport 연결이 끊긴 상태다. |
| `Closed` | Connector를 닫은 상태다. 같은 connector 객체로 다시 연결하지 않는다. |

연결 요청은 현재 상태에 따라 다음과 같이 동작한다.

| 현재 상태 | 동작 |
|---|---|
| `Created` | Connector가 초기 연결을 시작한다. |
| `Disconnected` | Connector가 수동 재연결을 시작한다. |
| `Connecting` | Caller는 이미 진행 중인 연결 시도가 끝날 때까지 기다린다. |
| `Connected` | 이미 연결되어 있으므로 호출은 성공으로 즉시 완료된다. |
| `Reconnecting` | Caller는 진행 중인 자동 reconnect 결과를 기다린다. |
| `Closed` | 닫힌 connector를 다시 연결할 수 없으므로 호출은 오류로 실패한다. |

**재연결과 pending request:**

- 자동 reconnect는 **기본으로 켜져 있다.**
- reconnect 중의 send는 큐에 저장하지 않고 **`Disconnected` 오류로 실패**한다.
- 연결이 끊기면 **pending request는 모두 실패**하며, reconnect 후 **자동 재전송하지 않는다.**

**heartbeat:**

- 켜져 있으면 지정 주기마다 control ping을 보낸다.
- 지정 timeout 동안 inbound frame이 없으면 transport가 끊긴 것으로 처리하고 reconnect 정책을
  적용한다.
- **heartbeat를 껐더라도 inbound ping에는 pong으로 응답한다.**

### 6.1 기본값

언어별 이름은 달라도 **기본값은 모든 언어가 같아야 한다.**

| 항목 | 기본값 |
|---|---|
| connect timeout | 5초 |
| request timeout | 30초 |
| wait timeout(특정 packet 대기) | 5초 |
| heartbeat | 켜짐 — interval 1초, timeout 5초 |
| reconnect | 켜짐 — 초기 지연 250ms, 최대 지연 5초, backoff 계수 2.0, 최대 시도 3회 |
| [dispatch mode](../01-glossary.ko.md#dispatch-mode) | `Manual`(§7) |
| codec | JSON(§5.4) |
| 압축 | Lz4(§8) |
| 송신·수신 payload 한도 | 각 64KB(§4.7) |
| inbound observer 큐 | notification 1024개, payload preview 0바이트(§10) |
| 수신 메시지 큐 | message 1024개(§10.1) |
| TLS 인증서 검증 | 켜짐 — 검증 생략 option의 기본값은 꺼짐이며 테스트의 자체 서명 인증서에만 사용한다 |

### 6.2 Connector reconnect 계기

Connector는 자동·수동 reconnect 시도 결과를 다음 metric으로 기록한다. 이 계기는 client connector가
소유하며 server session runtime은 reconnect 여부를 추측하거나 대신 기록하지 않는다.

| 계기 | 종류 | 단위 | Label | 의미 |
|---|---|---|---|---|
| `zlink.stream.reconnects` | counter | `{reconnect}` | `transport`, `outcome`, `reason` | Connector reconnect 시도 결과 누계 |

`outcome`은 `connected|failed|cancelled|shutdown`, `reason`은
`transport_closed|connect_failed|tls_failed|timeout|requested`의 닫힌 값이다. `transport`는 §3.1의
`tcp|tls|ws|wss` 중 하나다. Session ID와 remote endpoint는 label에 포함하지 않는다. 언어별 connector는
같은 이름과 닫힌 label을 언어별 exact interface가 정한
public metric provider 또는 sink에 게시한다. E2E와 application은 그 provider나 sink의 public reader를
사용하며 server-side proxy API를 만들지 않는다. Reader, sink 또는 exporter failure는 send, request와
연결 상태를 바꾸지 않는다.

### 6.3 종료 사유

연결이 끊기면 connector는 **종료 사유**를 노출한다. 값 집합은 서버 측 `close_reason`
([runtime-metrics §4](../25-runtime-metrics.ko.md#4-object와-stream))과 정합하는 **닫힌 집합**이며, wire 인코딩은
§4.6의 `session-closing` control packet이 소유한다.

| 사유 | 의미 |
|---|---|
| `ClientClose` | client가 닫았다 |
| `IdleTimeout` | 서버가 유휴 세션을 닫았다 |
| `HeartbeatTimeout` | heartbeat가 응답하지 않아 끊겼다 |
| `ServerDrain` | 서버가 **우아한 종료(graceful drain)** 로 세션을 닫았다 |
| `ProtocolError` | 프로토콜 위반으로 끊겼다 |
| `TransportError` | transport 수준 실패로 끊겼다 |

`ServerDrain`을 받은 client는 이 값을 보고 **재접속과 백오프를 결정한다**
([Host Relocate와 Shutdown §9](../28-graceful-drain-handoff.ko.md#9-대기-중인-message-timer와-session을-옮긴다)). **서버가 대체 endpoint를
지정하는 기능은 이 계약에 포함하지 않는다.**

언어별 문서는 이 사유를 표현하는 **타입 이름과 노출 방식**(속성인지 이벤트 인자인지)만
소유한다.

## 7. Dispatch 모드

| 모드 | 동작 |
|---|---|
| **`Manual`**(기본) | receive loop가 handler·error·disconnect·request callback을 직접 호출하지 않고 내부 큐에 넣는다. 사용자가 명시적으로 pump해 실행한다 |
| `Immediate` | receive 경로에서 직접 실행한다 |

**기본값이 `Manual`인 이유는 게임 엔진 제약이다**(§2.2). 엔진 객체는 main thread 밖에서 다룰
수 없으므로, main thread에서 pump해야 안전하다.

`waitFor`·`expectNone`·`waitForSequence` 계열은 등록된 callback이 아니다. 이 표면은 두 dispatch
mode 모두에서 수신 메시지 큐의 아직 소비하지 않은 packet을 직접 관측하고 소비하므로 `Manual`에서도
별도의 dispatch pump가 필요하지 않다. `dispatch`는 등록된 push handler, error·disconnect handler와
request callback만 실행한다.

## 8. 압축

- 지원 알고리즘은 **없음(None)과 Lz4**이며, **기본값은 Lz4**다.
- 압축 알고리즘은 packet마다 header에 적지 않는다. **connector option으로 한 번 정한다.**
- `payload compressed` flag(§4.3)는 "이 payload가 그 알고리즘으로 압축되어 있다"는 표시일
  뿐이다.
- **server → client**: 서버가 flag를 켜서 보내면 connector가 typed callback 호출 **전에**
  압축을 해제한다.
- **client → server**: **명시적으로 압축을 요청한 send/request만** 압축한다. option을 켰다고
  자동 압축되지 않는다.
- **압축은 payload에만 적용한다. header는 압축하지 않는다.**
- **`None`으로 설정하면 압축 frame을 주고받지 않는다.** 압축을 요청한 send/request는 실패하고,
  `payload compressed` flag가 켜진 수신 frame은 `DecompressionFailed`로 거부한다.

## 9. 오류 의미

| 오류 | 의미 |
|---|---|
| `Disconnected` | 연결이 없거나 끊김 |
| `ConfigurationError` | 구성이 잘못됨(scheme 불일치, **환경이 지원하지 않는 transport** 등) |
| `ValidationFailed` | 전송 전 검증 실패(metadata 한도 초과, 송신 payload 한도 초과 등) |
| `RequestTimeout` | reply 대기 시간 초과 |
| `ConnectTimeout` | 연결 시간 초과 |
| `FrameDecodeFailed` | frame·header decode 실패(§4.5), 또는 구조가 올바른 Error frame의 JSON payload가 §5.3을 충족하지 않음 |
| `FrameTooLarge` | payload가 수신 한도를 초과 |
| `SendFailed` | 전송 실패 |
| `CompressionFailed` / `DecompressionFailed` | 압축·해제 실패 |
| `TlsValidationFailed` | TLS 검증 실패 |
| `ReceivedMessageDropped` | 수신 메시지 큐 overflow(§10.1) |
| `UserCallbackFailed` | 사용자 callback이 실패 |
| `ObserverFailed` / `ObserverDropped` | inbound observer callback 실패 / 큐 overflow |
| `RemoteError` | 서버가 §5.3을 충족하는 Error payload로 응답함. `request_seq`가 pending request와 맞으면 그 request를 실패시키고, 없거나 맞지 않으면 error 이벤트로 전달함 |

오류가 현재 operation과 연결에 미치는 영향은 다음과 같다. 언어별 문서는 오류 이름의 표현만 소유하며
terminal 여부, 종료 사유와 reconnect 조건을 바꾸지 않는다.

| 오류 | 현재 operation | 연결 | 종료 사유 | 자동 reconnect |
|---|---|---|---|---|
| `ConfigurationError`, `ValidationFailed` | 호출 실패 | 유지하거나 연결 시도 전 상태 유지 | 없음 | 안 함 |
| `RequestTimeout` | 해당 request만 실패 | 유지 | 없음 | 안 함 |
| `ConnectTimeout`, `TlsValidationFailed` | connect 실패 | `Disconnected` | `TransportError` | reconnect option의 시도 정책을 적용 |
| `Disconnected`, `SendFailed` | 진행 중인 operation 실패 | transport가 끊겼으면 `Disconnected` | `TransportError` | reconnect option이 켜져 있으면 적용 |
| `FrameDecodeFailed` — frame·header | 해당 frame을 전달하지 않고 pending request를 실패시킴 | 종료 | `TransportError` | reconnect option이 켜져 있으면 적용 |
| `FrameDecodeFailed` — Error JSON payload | 맞는 `request_seq`가 있으면 그 request만 실패시키고, 없거나 맞지 않으면 error 이벤트로 전달 | 유지 | 없음 | 안 함 |
| `FrameTooLarge` | 해당 frame을 전달하지 않고 pending request를 실패시킴 | 종료 | `TransportError` | reconnect option이 켜져 있으면 적용 |
| `CompressionFailed` | 해당 송신 operation만 실패 | 유지 | 없음 | 안 함 |
| `DecompressionFailed` | 해당 수신 packet 또는 pending request만 실패 | 유지 | 없음 | 안 함 |
| `ReceivedMessageDropped` | 새로 도착한 send만 폐기 | 유지 | 없음 | 안 함 |
| `UserCallbackFailed`, `ObserverFailed`, `ObserverDropped`, `RemoteError` | 오류 event 또는 관련 callback/request로 전달 | 유지 | 없음 | 안 함 |

**전달 방식은 표면에 따라 다르되 의미는 같다.**

- 비동기(await) 표면은 **실패 시 오류를 던진다.**
- callback 기반 표면은 **결과 객체로 실패를 전달한다.**
- request id가 없는 stream 수준 오류는 **error 이벤트**로 전달한다.

## 10. Inbound observer

수신 frame을 **읽기 전용으로 관찰**하는 표면이다. 연결 시작 **전에만** 등록할 수 있다.

- 관찰 값: message kind, packet name, codec, request sequence, metadata, payload 바이트 길이,
  압축 여부, 수신 시각, payload preview
- **payload preview 기본 길이는 0**이다.
- metadata와 preview는 snapshot이다. observer가 바꿔도 request 완료나 handler가 보는 값은
  바뀌지 않는다.
- observer callback은 **receive 경로에서 직접 실행하지 않는다.** 느린 로그·metric 전송이
  수신 처리를 막으면 안 된다.
- callback 실패는 `ObserverFailed`, 큐 overflow는 `ObserverDropped`로 보고하며 **원래 frame
  처리를 막지 않는다.**
- observer notification 큐는 사용자 수신 메시지 큐와 **별도**이며 **기본 한도는 notification
  1024개**다(§6.1). option으로 조절한다.

### 10.1 수신 메시지 큐

서버가 보낸 `Send` packet은 handler(`on` 계열)나 대기 표면(`waitFor` 계열)으로 넘어가기 전까지
**수신 메시지 큐**에 머문다. 기본 한도는 **message 1024개**이며 option으로 조절한다.

- **큐가 가득 차면 새로 도착한 send message를 버리고 `ReceivedMessageDropped`를 보고한다.**
  이미 큐에 있는 메시지를 밀어내지 않는다.
- **response·error response·heartbeat control frame은 이 한도에 넣지 않는다.** request 완료와
  연결 유지에 필요하기 때문이다.
- 이 큐는 inbound observer notification 큐와 **별도**다(§10).

### 10.2 테스트 대기 표면

connector는 **테스트에서 push를 관측하는 대기 표면**을 공개 API로 제공한다. 다섯 언어는 같은 timeout,
소비 순서와 부정 관측 의미를 제공해야 한다. 조건 확인, 예상 오류와 timeout 검증처럼 connector 상태와
무관한 범용 단언은 connector 공개 계약이 아니다. E2E는 언어별 `Client/Support`에서 그 보조 코드를
소유한다.

#### 10.2.1 push 관측 표면 — `waitFor` 계열

수신 메시지 큐(§10.1)를 관측해야만 판정할 수 있는 것. connector 인스턴스의 메서드다.

세 표면 모두 packet 이름을 호출자가 명시하거나 payload type에서 결정한다. 정확한 인자와 overload,
완료 종결자(`.Async`/`.submit`/`.run`)는 각 언어 문서가 소유하며, 나머지 조건은 builder 체이닝으로
좁힌다.

| 표면 | 계약 | 실패 |
|------|------|------|
| `waitFor<T>(name)` | 그 packet이 올 때까지 대기. `.where(predicate)`·`.timeout(t)`로 좁힌다. 기본 timeout은 §6.1의 `wait timeout`(5초) | timeout 내 미도착이면 **오류를 던진다**(§10.1은 이 표면이 큐를 소비한다고 규정) |
| `expectNone<T>(name)` | `.within(window)` 동안 그 packet이 **오지 않는지** 확인한다(negative). `waitFor`의 대칭 | window 안에 도착하면 **오류를 던진다** |
| `waitForSequence<T>(name)` | `.expect(p1).expect(p2)….timeout(t)` — 같은 이름의 push가 **주어진 술어 순서대로** 도착하는지 확인하고 payload 목록을 돌려준다 | 순서가 어긋나거나 timeout이면 **오류를 던진다.** "N개가 도착했다"가 아니라 **"순서대로 도착했다"** 를 검증하는 것이 이 표면의 존재 이유다 |

- **status 대기는 별도 표면을 두지 않는다.** status는 payload의 한 필드이므로
  `waitFor<T>(name).where(p => p.status == …)`로 표현한다. connector가 어느 필드가 status인지
  알면 안 된다.
- **도메인 REST 폴링(`/orders/{id}` 등)은 이 표면이 아니다.** 그건 HTTP client의 일이며 connector
  계약에 넣지 않는다.

## 11. 배포 산출물

각 대상이 어떤 산출물로 배포되는지도 이 스펙이 소유한다. 배포 형식이 그 환경의 제약을
반영하기 때문이다.

| 대상 | 산출물 | 배포 채널 |
|---|---|---|
| 일반 C++ client | `zlink-stream-connector` (`zlink::stream_connector`) | CMake · vcpkg · Conan |
| 서버 e2e/perf (C++) | `zlink-stream-e2e-client` (`zlink::stream_e2e_client`) | CMake · vcpkg · Conan |
| Unreal | `zlink-unreal-stream-connector` | source plugin |
| Godot(C++) | `zlink-godot-stream-connector` | source GDExtension |
| Cocos/Axmol | `zlink-axmol-connector` | source package |
| `.NET`(데스크톱·서버) | `Systems.Zlink.Stream.Connector` | NuGet |
| **Unity(네이티브)** | 위 `.NET` 패키지를 **그대로 사용**(전용 패키지 없음) | NuGet |
| **Godot C#** | 위 `.NET` 패키지를 **그대로 사용** | NuGet |
| Java | `systems.zlink:zlink-stream-connector` | Maven |
| **브라우저 계열**(웹·Cocos web·Unity WebGL·Godot Web) | `@zlink-systems/stream-connector` package root | npm |
| **Unity WebGL 어댑터** | `@zlink-systems/stream-connector`의 browser bundle과 jslib·C# interop source | `com.zlink.stream-connector.webgl` UPM source package |
| (공통) wire 계층 | `@zlink-systems/stream-wire` | npm |

**배포 원칙:**

- **웹 계열은 npm package root 하나를 공유한다.** 브라우저·Cocos web·Unity
  WebGL·Godot Web은 전부 브라우저 런타임이므로 대상별로 패키지를 늘리지 않는다.
- **네이티브 엔진 어댑터는 source 배포다**(Unreal plugin, GDExtension, Axmol CMake). 엔진
  빌드 시스템에 소스로 편입되는 것이 관례다.
- **Unity(네이티브)와 Godot C#은 별도 패키지를 두지 않는다.** `.NET` connector를 그대로
  사용한다.

Unity WebGL UPM package는 새 wire runtime을 만들지 않는다. npm package root의 browser bundle을
포함하고 Unity가 요구하는 jslib·C# 호출 경계만 source로 제공한다. 따라서 browser와 Unity WebGL은
같은 TypeScript connector protocol과 codec을 사용한다.

## 12. 회귀 테스트

이 스펙이 요구하는 검증 항목이다. 언어별 테스트 이름은 달라도 의미는 같아야 한다.

| 항목 | 검증 |
|---|---|
| transport frame | frame·header 인코딩·디코딩이 §4를 따른다 |
| **환경별 transport 가용성** | **TypeScript package root가 `tcp://`·`tls://`를 구성 오류로 거부한다** |
| **브라우저 번들** | **TypeScript package root 번들에 플랫폼 전용 소켓 module이 포함되지 않는다** |
| typed request/reply | correlation과 매칭 규칙이 §5.2를 따른다 |
| error 응답 | `Error` payload가 §5.3의 JSON object이고, `request_seq` 유무에 따라 pending 실패 / stream 오류로 갈린다 |
| pending request 정리 | timeout·close·disconnect에서 pending이 모두 실패하고 제거된다(§5.2) |
| payload 한도 | 송신 한도가 **transport write 전에** 적용되고, 수신은 wire payload와 압축 해제 결과를 각각 검사한다(§4.7) |
| metadata | 한도·중복·빈 key 검증(§4.4) |
| packet name | UTF-8 길이 제한(§4.2), `$zlink.` prefix 예약(§4.6), 언어별 exact interface의 기본 이름·override 규칙 |
| codec | connector option 주입, codec 번호 공유와 browser/server dependency 분리(§5.4) |
| compression | 방향별 동작(§8) |
| error handling | 오류 의미(§9) |
| inbound observer | 관찰·격리·overflow(§10) |
| 연결 생명주기 | 상태 전이·재연결·heartbeat(§6) |
