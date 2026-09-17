# Stream Connector — 공통 스펙

[스펙 목차](../server/README.ko.md) | [이전: Session Actor Dispatch](../server/04-session/02-session-actor-binding.ko.md) | [다음: Location Runtime](../server/05-location-relocation/01-location-runtime.ko.md)

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
> 정의한다([공개 계약 관리](../server/00-foundation/01-public-contract-governance.ko.md)).

## 1. 목적과 범위

Stream Connector는 서버 framework의 **STREAM 모델에 접속하는 client 쪽 라이브러리**다.
서버 session callback이 받는 것과 같은
[packet](../server/00-foundation/02-glossary.ko.md#stream-packet)(header + payload)을 client에서도 동일하게
주고받게 한다.

[Connector](../server/00-foundation/02-glossary.ko.md#stream-connector)는 도메인을 포함하지 않는다. 사용자가 그 위에 채팅·게임·장비 제어·알림 같은 자기
protocol을 구성한다.

**의존성 경계:**

- connector package는 **서버 framework package에 의존하지 않는다**(ASP.NET Core adapter,
  SPOT, Stage wrapper 등).
- 의존성은 transport·codec·compression처럼 connector 실행에 필요한 client-side runtime으로만
  한정한다.
- 반대 방향도 같다. 서버 framework package는 connector를 참조하지 않는다.

## 2. 대상 실행 환경

**이 절이 이 스펙의 출발점이다.** 실행 환경의 제약이 계약을 결정하기 때문이다. 어떤
connector를 사용하는지는 **언어가 아니라 "엔진 × 빌드 타깃"** 으로 정해진다.

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
| **브라우저 JavaScript** | `AsyncLocalStorage`에 해당하는 **ambient 실행 문맥이 없다** | 수신 message의 flow를 이어받으려면 호출자가 송신 call에 그 flow를 **명시적으로 전달한다**(§5.5) |
| Node.js | TypeScript connector의 제품 실행 환경이 아니다 | 서버 process와 browser test runner만 담당한다 |

## 3. Transport

### 3.1 endpoint scheme → transport

| URI scheme | transport |
|---|---|
| `tcp://` | TCP |
| `tls://` | TLS over TCP |
| `ws://` | WebSocket |
| `wss://` | WebSocket over TLS |

- **transport를 명시하지 않으면 endpoint scheme이 transport를 결정한다.** 위 표가 그 대응이며,
  option의 고정 기본값이 scheme을 덮어쓰지 않는다. `ws://` endpoint 하나만 지정한 구성은 추가
  option 없이 WebSocket으로 연결한다.
- **명시한 transport가 endpoint scheme과 어긋나면 `ConfigurationError`(§9)로 실패한다.** 두 값이
  서로 다른 transport를 가리키면 어느 쪽을 따를지 정할 수 없다.

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
  [packet name](../server/00-foundation/02-glossary.ko.md#packet-name)을 담지 않는다** — `name_len = 0`으로 인코딩한다. 응답은 handler를 고르지 않고
  상관관계는 `request_seq`가 이미 정하므로 이 필드가 쓰이지 않는다
  ([03 message model](../server/00-foundation/05-message-model.ko.md)의 "reply 상관관계").
- metadata는 `u16 meta_len + metadata bytes`, correlation id는 `u8 len + bytes`로 이어진다.
- flow 필드는 **36바이트 `flow_id`와 1바이트 `flow_origin`이 항상 함께** 존재하거나 함께 없다.
  의미는 [메시지 흐름 상관관계 §3](../server/06-observability/04-flow-correlation.ko.md#3-형식과-소유권)이 소유한다.
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
([flow-correlation §3](../server/06-observability/04-flow-correlation.ko.md#3-형식과-소유권)).

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
사용하지 않는다. 신규 control packet은 `$zlink.` prefix를 사용한다.

control frame은 `Raw` codec, request sequence 없음, metadata 없음, flow flag 없음이다.
**payload는 control packet마다 다르다.**

| control packet | payload |
|---|---|
| `$zlink.heartbeat.ping` | **비어 있다** |
| `$zlink.heartbeat.pong` | **비어 있다** |
| `session-closing` | **비어 있지 않다** — 아래 참조 |

`session-closing`은 서버가 세션을 닫기 직전에 보내는 control packet이며, client는 이를 읽어
`closeReason`을 확정한다
([Host relocation 전체 흐름 §9](../server/05-location-relocation/05-host-relocation-flow.ko.md#12-대기-중인-message-timer와-session을-옮긴다)).

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
application handler나 request completion으로 전달하지 않는다. 송신 한도는 실제 transport에 사용하는
payload를 기준으로 하므로 압축을 요청한 송신은 압축 결과를 검사한다. 64KB보다 큰 payload가 필요한
애플리케이션은 이 값을 명시적으로 키운다.

## 5. Packet 모델

사용자 API는 raw header bytes를 다루지 않는다.

- **기본 packet 이름은 payload 타입의 단순 이름**이다. namespace·package 한정자를 붙이지 않는다.
- **컴파일러나 런타임에 따라 달라지는 이름을 packet 이름으로 사용하지 않는다.** packet 이름은
  서버와 합의한 값이므로, 컴파일러가 만드는 mangled name처럼 빌드 환경이 바뀌면 달라지는 값을
  쓰면 같은 타입을 보내도 서버가 handler를 찾지 못한다.
- **payload 타입에 packet 이름을 직접 붙이는 수단을 다섯 언어가 모두 제공한다.** 붙이는 형태
  (attribute, annotation, 정적 멤버 등)는 언어 문서가 소유한다. 타입에 붙인 이름은 단순 타입
  이름보다 우선한다.
- operation마다 **호출자가 명시한 이름**이 있으면 그쪽이 가장 우선이다.
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
  ([Session Actor Dispatch §3](../server/04-session/02-session-actor-binding.ko.md#5-bind와-relay)).
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
바꾸는 public API는 제공하지 않는다. **typed payload codec과, payload 타입에서 packet 이름을 정하는
name resolver(§5)는 둘 다 connector 생성 option으로 주입한다.** operation마다 바꾸는 표면을 두지
않는 대신, 두 주입점은 생성 option에만 둔다. 둘 다 지정하지 않으면 JSON codec과 §5의 기본
이름 규칙을 사용하며, 주입점의 타입 이름은 언어 문서가 소유한다. Raw encoded payload는 외부
protocol 연동을 위해 payload가 지정한 codec 번호를 그대로 사용할 수 있다.

TypeScript package root는 browser-safe `ZlinkStreamPayloadCodec`을 내보내며 connector를 만들 때
`codec` option으로 주입한다. Node framework serializer 등록은 같은 package의 `./framework` subpath를
사용한다. 두 진입점은 `stream-wire`가 소유하는 같은 codec 번호를 사용하지만 browser module graph가
server framework runtime을 참조하지 않아야 한다.

### 5.5 flow 노출과 전파

**수신 message는 payload, packet 이름, metadata와 함께 `flow_id`·`flow_origin`을 노출한다.**
다섯 언어가 모두 노출한다. 두 값의 형식과 의미는
[메시지 흐름 상관관계 §3](../server/06-observability/04-flow-correlation.ko.md#3-형식과-소유권)이 소유하며,
diagnostics level이 `Off`이면 connector가 값을 전달하지 않으므로 두 값이 비어 있다(§13).
application은 이 값을 읽어 자기 log와 서버 trace를 같은 흐름으로 맞춘다.

handler가 수신 message를 처리하는 동안 시작한 send와 request는 **그 message의 flow를 이어받는다.**
이어받는 방법은 실행 환경이 정한다.

| 실행 환경 | flow를 잇는 방법 |
|---|---|
| ambient 실행 문맥을 제공하는 런타임 | connector가 handler를 실행하는 문맥에 현재 flow를 보관하고, 같은 문맥에서 시작한 send·request가 인자 없이 그 값을 사용한다 |
| ambient 실행 문맥이 없는 런타임(브라우저 JavaScript) | 호출자가 처리 중인 message의 flow를 송신 call에 명시적으로 전달한다. 전달 표면의 이름은 언어 문서가 소유한다 |

- **두 방법의 차이는 호출 방식이며 보장의 차이가 아니다.** 어느 쪽이든 만들어진 frame의
  `flow_id`·`flow_origin`은 같은 값이고, flow를 잇지 않은 송신은 어느 쪽에서도 새 flow로 시작한다.
- **ambient 문맥이 없는 런타임에 명시 전달 표면을 두는 것은
  [메시지 흐름 상관관계 §6](../server/06-observability/04-flow-correlation.ko.md#6-async-작업과-execution-context)의
  요구를 connector에 적용한 것이다.** 브라우저 JavaScript에는 `AsyncLocalStorage`에 해당하는 표면이
  없어 실행 문맥에 값을 보관할 수 없다(§2.2). 같은 문서가 금지하는 대로 process 전역 변수나
  connector의 변경 가능한 field로 현재 flow를 추정하지 않는다.

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
- **reconnect 최대 시도 횟수는 무제한을 표현할 수 있어야 한다.** 표현 수단(널 값, 음수, 이름
  붙인 상수 등)은 언어 문서가 소유한다. 연결이 복구될 때까지 계속 시도하는 client는 큰 수를
  적는 대신 무제한을 지정하며, 그래야 시도 횟수가 유한한 구성과 구분된다.
- **시도 사이의 지연에 무작위를 넣는다.** 기준 지연은 초기 지연에서 시작해 시도마다 backoff
  계수를 곱하고 최대 지연에서 멈춘다. 실제로 기다리는 시간은 **그 기준 지연의 50%에서 100%
  사이에서 고른 값**이다.

    결정적 지연을 쓰면 서버가 한 번 끊겼을 때 연결되어 있던 client가 **모두 같은 시점에 다시
    연결한다.** 그 순간에 서버의 부하가 가장 크므로 시점을 서로 다르게 만든다.

- **시도를 다 쓰면 끊김 handler가 실행된다.** 마지막 시도가 실패하면 연결 상태가
  `Disconnected`가 되고, 등록된 끊김 handler가 실행된다. 무제한으로 지정한 구성에서는
  이 자리에 이르지 않는다.

    **handler가 종료 사유를 인자로 받는지는 언어가 정한다.** 인자로 받지 않는 언어에서는
    §6.2의 읽기 표면으로 사유를 확인한다. 어느 쪽이든 사유에 닿는 길이 있다.

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
| [dispatch mode](../server/00-foundation/02-glossary.ko.md#dispatch-mode) | `Manual`(§7) |
| codec | JSON(§5.4) |
| 압축 | Lz4(§8) |
| 송신·수신 payload 한도 | 각 64KB(§4.7) |
| TLS 인증서 검증 | 켜짐 — 검증 생략 option의 기본값은 꺼짐이며 테스트의 자체 서명 인증서에만 사용한다 |
| diagnostics level | `Errors`(§13) |

### 6.2 종료 사유

연결이 끊기면 connector는 **종료 사유**를 노출한다. 값 집합은 서버 측 `close_reason`
([runtime-metrics §4](../server/06-observability/02-runtime-metrics.ko.md#6-object-수capacity와-relocation-계기))과 정합하는 **닫힌 집합**이며, wire 인코딩은
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
([Host relocation 전체 흐름 §9](../server/05-location-relocation/05-host-relocation-flow.ko.md#12-대기-중인-message-timer와-session을-옮긴다)). **서버가 대체 endpoint를
지정하는 기능은 이 계약에 포함하지 않는다.**

**종료 사유는 connector의 읽기 표면으로 언제든 조회할 수 있다.** disconnect 이벤트를 받지 못한
code도 연결이 닫힌 뒤 같은 값을 읽는다. 연결이 한 번도 끊긴 적이 없으면 값이 비어 있다. **첫 connect가 실패한 경우도 사유가
남는다** — §9의 영향 표가 정한 값을 따르므로 `ConnectTimeout`·`TlsValidationFailed`는
`TransportError`다. 연결이 성립한 적이 없어도 그 시도가 어떻게 끝났는지는 남는다.
disconnect 이벤트가 사유를 인자로 함께 전달하는 것은 이 읽기 표면에 추가하는 것이며 대신하지
않는다. 다시 연결하면 값을 지우지 않고 마지막 종료의 사유를 유지한다.

언어별 문서는 이 사유를 표현하는 **타입 이름과 읽기 표면의 모양**(속성인지 method인지)만
소유한다.

### 6.3 옵션 검증

**option 전 항목을 검증한다.** 일부만 보면 나머지 항목의 잘못된 값이 조용히 무시되고, 호출자는
구성 실수와 연결 실패를 구분하지 못한다.

- endpoint와 transport의 정합(§3.1), connect·request·wait timeout, heartbeat interval과 timeout,
  reconnect 지연·backoff 계수·최대 시도, 송신·수신 payload 한도, codec과 압축 설정, dispatch mode,
  diagnostics level을 **모두 확인한다.** §6.1의 기본값을 적용한 뒤의 값을 검증한다.
- **검증 지점은 그 언어가 실패를 알릴 수 있는 가장 이른 곳이다.** 실패를 돌려줄 통로가 있는
  언어는 connector를 만들 때 검증하고, 생성 표면에 그 통로가 없는 언어는 연결을 시도할 때
  검증한다. 어느 쪽이든 **연결이 이뤄지기 전에** 거부한다.
- **검증을 통과하지 못한 구성으로는 연결되지 않는다.** 통과한 option만 연결에 사용한다.
- 값 하나가 허용 범위를 벗어나면 **`ValidationFailed`**, 항목 사이가 맞지 않으면
  **`ConfigurationError`**(§9)다. endpoint scheme과 transport의 충돌, 환경이 지원하지 않는
  transport, 압축을 끈 구성에 압축 codec을 함께 넣는 것이 뒤쪽에 해당한다.
- **전달은 §9.2를 따른다.** 예외를 끈 빌드는 값으로 받고 나머지 언어는 코드를 담은 예외로
  받는다. 언어 표준 예외를 그대로 던지면 호출자가 두 코드를 구분하지 못한다.

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

**handler 등록은 등록을 해제할 수 있는 값을 돌려준다.** push handler와 error·disconnect·connection
state handler 모두 같다. connector를 닫아야만 등록을 없앨 수 있으면, 화면 하나의 수명에 맞춰
구독을 등록하고 해제하는 client가 연결을 다시 만들어야 한다. 해제한 handler는 그 뒤의 dispatch에서
실행하지 않으며, 같은 값을 두 번 해제해도 오류로 처리하지 않는다.

**돌려준 값의 수명이 등록의 수명인지는 언어가 정한다.** 소유권을 값으로 나타내는 언어는
그 값이 사라질 때 등록도 해제한다. 그 언어에서는 돌려받은 값을 등록이 살아 있어야 하는
동안 보관한다. 나머지 언어는 값을 버려도 등록이 남고 명시적으로 해제할 때까지 유지된다. 돌려주는 타입 이름은 언어
문서가 소유한다.

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
| `ValidationFailed` | 검증 실패 — 전송 전 검증(metadata 한도 초과, 송신 payload 한도 초과), option 값이 허용 범위를 벗어난 구성 검증(§6.3), 대기 표면의 관측 조건 위반(§10.1)을 함께 덮는다 |
| `RequestTimeout` | reply 대기 시간 초과 |
| `ConnectTimeout` | 연결 시간 초과 |
| `FrameDecodeFailed` | frame·header decode 실패(§4.5), 또는 구조가 올바른 Error frame의 JSON payload가 §5.3을 충족하지 않음 |
| `FrameTooLarge` | payload가 수신 한도를 초과 |
| `SendFailed` | 전송 실패 |
| `CompressionFailed` | 압축 실패 |
| `DecompressionFailed` | 압축 해제 실패 |
| `TlsValidationFailed` | TLS 검증 실패 |
| `UserCallbackFailed` | 사용자 callback이 실패 |
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
| `UserCallbackFailed`, `RemoteError` | 오류 event 또는 관련 callback/request로 전달 | 유지 | 없음 | 안 함 |

### 9.1 닫힌 오류 코드 집합

위 표의 **13개가 전부다.** 구현이 코드를 더하거나 빼지 않는다. 언어별 문서는 이름의 표기만
소유한다.

한 가지 예외를 명시한다 — **브라우저 런타임에서는 `TlsValidationFailed`가 발생하지 않는다.**
브라우저의 WebSocket API가 TLS 실패를 일반 연결 실패와 구분해 주지 않기 때문이다. 코드는
집합에 그대로 남고, 그 런타임에서 쓰이지 않을 뿐이다. 집합을 런타임마다 다르게 하면 위
영향 표도 런타임마다 달라진다.

### 9.2 전달 — 받는 쪽이 코드를 읽을 수 있어야 한다

**전달 방식은 표면에 따라 다르되 의미는 같다.**

- 비동기(await) 표면은 **실패 시 오류를 전달한다.**
- callback 기반 표면은 **결과 객체로 실패를 전달한다.**
- request id가 없는 stream 수준 오류는 **error 이벤트**로 전달한다.

어느 방식이든 **받는 쪽이 위 13개 중 무엇인지 읽을 수 있어야 한다.** 이것이 이 절의 요구
사항이고, 무엇으로 전달하는지는 플랫폼이 정한다.

| 대상 | 전달 | 코드를 읽는 법 |
|---|---|---|
| 예외를 끈 빌드(게임 엔진 등) | 값으로 돌려준다. **던지지 않는다** | 결과 객체의 오류 코드 |
| 그 밖의 C++ 소비자(e2e·도구) | 예외를 던지는 어댑터를 사용한다 | 예외가 담은 코드 |
| 그 밖의 언어 | 예외를 던진다 | 예외가 담은 코드 |

**언어 표준 예외를 그대로 쓰지 않는다.** `IllegalArgumentException`·`InvalidOperationException`
같은 타입은 코드를 담는 자리가 없어, 호출자가 `ValidationFailed`인지 `ConfigurationError`인지
판정하지 못한다. 예외로 전달하는 언어는 **코드를 담는 전용 예외 타입**을 둔다.

이 요구는 옵션 검증 실패와 관찰 표면의 위반(§10.1)에도 똑같이 적용된다.

## 10. 수신 메시지 큐

서버가 보낸 `Send` packet은 handler(`on` 계열)나 대기 표면(`waitFor` 계열)으로 넘어가기 전까지
**수신 메시지 큐**에 머문다.

- **client는 받은 것을 계속 받아서 처리한다.** 큐에 한도를 두지 않고, message를 버리지 않으며,
  이 때문에 연결을 닫지도 않는다.
- **connector는 backpressure를 하지 않는다.** connector는 socket을 직접 구현하지 않고 실행 환경이
  주는 것을 사용한다(§2·§3.2). 브라우저·WASM은 네이티브 WebSocket API 위에서 동작하는데 거기에는
  읽기를 보류할 표면이 없다. 흐름 제어는 서버 STREAM socket이 소유하며 이 문서의 범위가 아니다.
- **response·error response·heartbeat control frame은 이 큐를 거치지 않는다.** request 완료와
  연결 유지에 필요하기 때문이다.
- **connector는 packet 이름별 수신 개수를 `receivedCount(name)`로 공개하며, 다섯 언어가 모두
  제공한다.** 값은 그 이름으로 **받은 개수**다. **소비해도 줄지 않는다** — handler가
  dispatch하든 대기 표면이 가져가든 값은 그대로다. **dispatch mode와 무관하게 센다** —
  `Manual`에서 pump를 기다리든 `Immediate`에서 수신 경로가 바로 실행하든, packet이 도착한
  시점에 값이 올라간다(§7). 이 표면의 용도가 시나리오 단정이므로, `on` handler를 걸고
  dispatch한 뒤에도 "이 이름으로 몇 건 왔다"를 판정할 수 있어야 한다.
- **기준점은 연결이 성립한 시점이다.** 연결이 성립할 때 0에서 시작해 그 연결에서 받은 수를
  센다. 재연결하면 새 연결이므로 다시 0에서 시작한다. 이름을 받은 적이 없으면 0이다.
- 이 값은 시나리오 단정과 진단에 사용하며, 흐름 제어의 근거로 사용하지 않는다 — 큐에 한도가
  없으므로 값이 커졌다고 connector가 하는 일이 달라지지 않는다.

정상 동작하는 client에서는 메시지가 쌓이지 않는다. handler가 dispatch하거나 wait 표면이 소비한다.
쌓인다면 client 버그이며, 그 상태에서 connector가 버리거나 연결을 닫아도 얻는 것이 없다 — 어차피
client를 재기동해야 한다. 그래서 connector에는 이에 대한 정책을 두지 않는다.

### 10.1 테스트 대기 표면

connector는 **테스트에서 push를 관측하는 대기 표면**을 공개 API로 제공한다. 다섯 언어는 같은 timeout,
소비 순서와 부정 관측 의미를 제공해야 한다. 조건 확인, 예상 오류와 timeout 검증처럼 connector 상태와
무관한 범용 단언은 connector 공개 계약이 아니다. E2E는 언어별 `Client/Support`에서 그 보조 코드를
소유한다.

#### 10.1.1 push 관측 표면 — `waitFor` 계열

수신 메시지 큐(§10)를 관측해야만 판정할 수 있는 것. connector 인스턴스의 메서드다.

아래 표면은 packet 이름을 **호출자가 명시하는 길과 payload type에서 결정하는 길을 함께**
제공한다. 둘 중 하나만 두지 않는다. 정확한 인자와 overload, 완료 종결자
(`.Async`/`.submit`/`.run`)는 각 언어 문서가 소유하며, 나머지 조건은 builder 체이닝으로 좁힌다.

| 표면 | 계약 | 실패 |
|------|------|------|
| `waitFor<T>(name)` | 그 packet이 올 때까지 대기. `.where(predicate)`·`.timeout(t)`로 좁힌다. 기본 timeout은 §6.1의 `wait timeout`(5초) | timeout 내 미도착이면 **오류로 실패한다**(§10은 이 표면이 큐를 소비한다고 규정) |
| `expectNone<T>(name)` | `.within(window)` 동안 그 packet이 **오지 않는지** 확인한다(negative). `waitFor`의 대칭 | window 안에 도착하면 **오류로 실패한다** |
| `waitForSequence<T>(name)` | `.expect(p1).expect(p2)….timeout(t)` — 같은 이름의 push가 **주어진 술어 순서대로** 도착하는지 확인하고 **message 목록**을 돌려준다 | 순서가 어긋나거나 timeout이면 **오류로 실패한다.** "N개가 도착했다"가 아니라 **"순서대로 도착했다"** 를 검증하는 것이 이 표면의 존재 이유다 |

- **술어와 반환은 payload가 아니라 message를 다룬다.** payload만 주면 술어가 metadata와
  packet 이름을 보지 못한다. `T`는 message가 담은 payload의 타입이다.
- **이 표면의 실패는 모두 `ValidationFailed`다.** 전달 수단은 §9.2가 정한다 — 예외를 끈
  빌드는 값으로, 나머지는 코드를 담은 예외로 받는다.

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
| `.NET`(데스크톱·서버) | `Zlink.Stream.Connector` | NuGet |
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
| error 응답 | `Error` payload가 §5.3의 JSON object이고, `request_seq` 유무에 따라 pending 실패와 stream 오류로 달라진다 |
| pending request 정리 | timeout·close·disconnect에서 pending이 모두 실패하고 제거된다(§5.2) |
| payload 한도 | 송신 한도가 **transport write 전에** 적용되고, 수신은 wire payload와 압축 해제 결과를 각각 검사한다(§4.7) |
| metadata | 한도·중복·빈 key 검증(§4.4) |
| packet name | UTF-8 길이 제한(§4.2), `$zlink.` prefix 예약(§4.6), 언어별 exact interface의 기본 이름·override 규칙 |
| codec | connector option 주입, codec 번호 공유와 browser/server dependency 분리(§5.4) |
| compression | 방향별 동작(§8) |
| error handling | 오류 의미(§9) |
| 연결 생명주기 | 상태 전이·재연결·heartbeat(§6) |
| **옵션 검증** | **연결이 이뤄지기 전에 전 항목을 검증하고, 값 범위 위반은 `ValidationFailed`, 항목 사이 불일치는 `ConfigurationError`로 거부한다(§6.3)** |
| **transport 추론** | **transport를 명시하지 않으면 endpoint scheme이 정하고, 명시한 값이 scheme과 어긋나면 `ConfigurationError`다(§3.1)** |
| **오류 전달** | **받는 쪽이 §9의 13개 중 무엇인지 읽어낸다. 언어 표준 예외를 그대로 쓰지 않는다(§9.2)** |
| **재연결 지연** | **시도 사이의 대기가 기준 지연의 50%에서 100% 사이에 들어간다. 시도를 다 쓰면 상태가 `Disconnected`가 되고 끊김 handler가 실행된다(§6)** |
| **등록 해제** | **push·error·disconnect·connection state 네 등록이 모두 해제할 수 있는 값을 돌려주고, 해제한 handler는 그 뒤의 dispatch에서 실행되지 않는다(§7)** |
| **수신 개수** | **`receivedCount(name)`가 받은 개수를 세고 소비해도 줄지 않으며, dispatch mode와 무관하다. 연결이 성립할 때 0에서 다시 시작한다(§10)** |
| **대기 표면** | **이름을 명시하는 길과 payload type에서 결정하는 길이 모두 있고, 술어와 반환이 message이며, 위반은 `ValidationFailed`다(§10.1)** |
| **flow 노출과 전파** | **수신 message가 flow 식별자와 출처를 노출하고, ambient 문맥이 없는 런타임은 명시 전달 수단을 제공한다(§5.5)** |
| **종료 사유 읽기** | **끊긴 뒤 이벤트를 받지 않은 코드도 같은 값을 읽는다. 첫 connect 실패에도 사유가 남고, 재연결해도 지워지지 않는다(§6.2)** |
| diagnostics level | `Off` outbound frame에 flow 필드·flag(0x10) 부재, inbound flow 값 검증 생략, `Errors` 기본값에서 현행 wire 유지, one-way `Send`의 correlation id 부재(§13) |

## 13. Diagnostics level

Connector는 생성 시점 option으로 diagnostics level을 받는다. 값과 의미는
[Message flow tracing §4](../server/06-observability/03-message-flow-tracing.ko.md#4-기록-범위-설정--level과-sampling)의
네 값 `Off`·`Errors`·`Normal`·`Detailed`를 그대로 사용하며 **기본값은 `Errors`다.**
Connector는 [Flow correlation §4](../server/06-observability/04-flow-correlation.ko.md#4-flow를-만드는-시점)가
말하는 client connector 규칙의 적용 대상이므로, 실행 중 level 변경도
[Message flow tracing §4.1](../server/06-observability/03-message-flow-tracing.ko.md#5-실행-중-기록-수준-변경과-비용-규칙)을
그대로 따른다. Application은 connector를 다시 만들지 않고 level을 읽고 바꿀 수 있으며,
변경은 그 뒤의 처리 지점부터 적용한다. 이미 만들어진 frame에는 소급 적용하지 않는다.
각 처리 지점은 현재 level을 한 번 읽어 그 값으로 판단한다.

**level을 읽는 표면과 바꾸는 동기 표면을 제공한다.** level 변경은 값 하나를 바꾸는 작업이므로
호출자가 기다릴 완료가 없다. 비동기 완료가 그 언어의 관용이면 같은 의미의 비동기 짝을 함께
둔다 — 두 표면은 같은 값을 바꾸며, 비동기 짝이 동기 표면을 대신하지 않는다. 동기 표면을 비동기
짝 위의 blocking 호출로 구현하면 receive callback 안에서 그 호출이 자기 완료를 기다리게 되므로,
동기 표면은 기다리지 않고 값을 바꾼다. 두 표면의 이름은 언어 문서가 소유한다.

`Off`가 아니면 connector는 기존과 같이 outbound frame에 `flow_id`·`flow_origin`을
생성·부착하고(§4.2, flag `0x10`), inbound frame의 flow 필드를 검증해 수신 message에
전달한다.

`Off`이면 [Flow correlation §4](../server/06-observability/04-flow-correlation.ko.md#4-flow를-만드는-시점)의
client connector 규칙을 그대로 적용한다.

- Outbound frame에 `flow_id`·`flow_origin`을 만들지 않고 부착하지 않는다(flag `0x10`을
  세우지 않는다).
- Inbound frame의 flow 필드는 **구조 길이 검사만 유지**하고 값 검증(UUIDv7·origin 범위)과
  수신 message로의 전달을 생략한다.
- Flow 생성·검증·전파 등 관측 전용 작업을 하지 않는다.

Diagnostics level은 protocol 정보에 영향을 주지 않는다. Request의 correlation id는
`Off`에서도 만들고 보존한다. 반대로 **one-way `Send`는 어느 level에서도 correlation id를
만들지 않는다** — [Flow correlation §2](../server/06-observability/04-flow-correlation.ko.md#2-두-식별자의-역할)의
"reply가 없는 one-way message에는 `correlation_id`를 만들지 않는다"가 connector에도 그대로
적용된다(flag `0x08` 미설정).
