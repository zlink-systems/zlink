<!-- framework-adapter-nav:start -->
[문서 목록](../../../../../../README.ko.md)
<!-- framework-adapter-nav:end -->

# TypeScript Stream Connector

> 이 문서는 [Stream Connector 공통 스펙](../../32-stream-connector.ko.md)의 **TypeScript 투영**이다.
> transport·wire·생명주기·오류 의미는 공통 스펙이 소유하고, 이 문서는 그 의미가
> TypeScript에서 갖는 **정확한 public 표면**을 고정한다.

TypeScript connector는 `@zlink-systems/stream-connector` 패키지로 제공하는 브라우저 client
connector다.
서버 framework와 별도 모듈이며 request/reply, dispatch(`Manual`/`Immediate`), typed payload API를
client code에서 사용하게 한다. JSON, MessagePack, Protobuf 또는 custom codec은 connector를 만들 때
`codec` option 하나로 주입한다. typed `send`/`request`/`on`/`waitFor` 표면은 주입된 codec으로 업무
DTO를 encode/decode한다.

## 1. 대상 실행 환경

**엔진 × 빌드 타깃별 담당 connector는 [공통 스펙 §2](../../32-stream-connector.ko.md)가 소유한다.**
그 배정에 따라 TypeScript connector가 담당하는 것은 **브라우저 계열**(웹 client, Unity WebGL,
Cocos Creator web, Godot Web)이다. Node.js process는 connector의 제품 실행 환경이 아니다.

**웹(브라우저·WASM)으로 빌드하는 모든 엔진이 언어와 무관하게 이 connector를 사용한다.**
Unity WebGL은 npm package root의 browser bundle과 jslib·C# 호출 경계를 제공하는
`com.zlink.stream-connector.webgl` UPM source adapter를 사용한다. 이 adapter는 별도 wire runtime을
제공하지 않는다.

Unity WebGL C# adapter는 bound Actor 표면을 같은 의미로 투영한다.

```csharp
public partial class ZlinkStreamConnector
{
    public IReadOnlyList<ZlinkStreamActor> Actors { get; }
    public ZlinkStreamActor? Actor(string actorId);
    public IDisposable OnActorBound(Action<ZlinkStreamActor> handler);
    public IDisposable OnActorUnbound(Action<ZlinkStreamActor> handler);
}

public sealed class ZlinkStreamActor
{
    public string ActorId { get; }
    public bool IsBound { get; }
    public ZlinkStreamSendCall Send(object payload);
    public ZlinkStreamRequestCall Request(object payload);
    public IDisposable On<TPayload>(string name, Action<ZlinkStreamMessage<TPayload>> handler);
}
```

jslib JSON 경계는 `actorId`와 bound·unbound lifecycle event만 전달한다. `actor_slot`은 TypeScript
wire runtime 안에 남으며 public C# 값으로 노출하지 않는다.

## 2. 진입점(entrypoint)

공개 진입점은 package root인 `@zlink-systems/stream-connector` 하나다. 이 진입점은 플랫폼
`WebSocket`을 사용하는 브라우저 구현과 ESM type declaration을 직접 내보낸다. `/browser` subpath와
Node 조건부 export는 제공하지 않는다.

**계약:**

- package root의 번들 그래프에는 **`net`·`tls`·`Buffer` 같은 Node 전용 모듈이 포함되지
  않는다.** 검증 범위는 §7의 문서가 소유한다.
- 공통 wire 계층(`@zlink-systems/stream-wire`)은 두 런타임에서 **같은 코드**로 동작한다.
  브라우저 ESM과 server CommonJS 산출물은 같은 source와 wire 상수를 사용하며 `Uint8Array` byte
  fixture가 일치해야 한다.

## 3. Transport

scheme → transport 매핑은 [공통 스펙 §3.1](../../32-stream-connector.ko.md)을 따른다.
TypeScript connector가 사용할 수 있는 transport는 **`ws`와 `wss`뿐**이다.

**package root가 `tcp://`·`tls://` endpoint를 받으면 `ZlinkStreamErrorCode.ConfigurationError`로
즉시 실패한다.** 연결을 시도하다 런타임에 조용히 실패하지 않는다.

브라우저에서 `ws`·`wss`는 **플랫폼의 네이티브 `WebSocket`** 으로 구현한다. 핸드셰이크와 프레이밍을
브라우저가 수행하므로 connector가 직접 구현하지 않는다.

### 3.1 transport factory 주입

`transportFactory` option은 테스트 대역(in-memory transport)이나 플랫폼 전용 transport를 넣는
확장점이다. 기본값은 플랫폼 `WebSocket` adapter다. Node transport 호환 지점으로 사용하지 않는다.

## 4. Public 표면

package root가 노출하는 public 타입은 다음과 같다.

```ts
interface ZlinkStreamFlow {
  readonly flowId: string;
  readonly flowOrigin: ZlinkFlowOrigin;
}

interface ZlinkStreamConnector {
  readonly isConnected: boolean;
  readonly state: ZlinkStreamConnectionState;
  readonly closeReason?: ZlinkStreamCloseReason;
  readonly options: RequiredZlinkStreamConnectorOptions;
  readonly pendingDispatchCount: number;
  readonly diagnosticsLevel: ZlinkStreamDiagnosticsLevel;

  connect(signal?: AbortSignal): Promise<void>;
  close(signal?: AbortSignal): Promise<void>;
  dispatch(signal?: AbortSignal): Promise<void>;
  receivedCount(name: string): number;                                  // packet 이름별 수신 개수(§5)
  setDiagnosticsLevel(level: ZlinkStreamDiagnosticsLevel): void;        // 기다리지 않고 값을 바꾼다
  setDiagnosticsLevelAsync(level: ZlinkStreamDiagnosticsLevel): Promise<void>; // 같은 값을 바꾸는 비동기 짝

  send(payload: unknown, messageType?: Function): ZlinkStreamSendCall;
  request(payload: unknown, messageType?: Function): ZlinkStreamRequestCall;
  // packet 이름을 호출자가 명시하는 길과 payload 생성자에서 결정하는 길을 함께 둔다.
  waitFor<TPayload = ZlinkStreamEncodedPayload>(nameOrType: string | Function): ZlinkStreamWaitCall<TPayload>;
  expectNone<TPayload = ZlinkStreamEncodedPayload>(nameOrType: string | Function): ZlinkStreamExpectNoneCall<TPayload>;
  waitForSequence<TPayload = ZlinkStreamEncodedPayload>(nameOrType: string | Function): ZlinkStreamSequenceCall<TPayload>;
  on<TPayload = ZlinkStreamEncodedPayload>(
    name: string,
    handler: (message: ZlinkStreamMessage<TPayload>, signal?: AbortSignal) => Promise<void> | void,
    messageType?: Function
  ): Disposable;

  onErrorReceived(handler: (error: ZlinkStreamError, signal?: AbortSignal) => Promise<void> | void): Disposable;
  onDisconnected(handler: (signal?: AbortSignal) => Promise<void> | void): Disposable;
  onConnectionStateChanged(
    handler: (change: ZlinkStreamConnectionStateChanged, signal?: AbortSignal) => Promise<void> | void
  ): Disposable;

  // 지금 bind되어 있는 Actor handle(공통 스펙 §5.6). application이 만들지 않는다.
  readonly actors: readonly ZlinkStreamActor[];
  actor(actorId: string): ZlinkStreamActor | undefined;
  onActorBound(handler: (actor: ZlinkStreamActor, signal?: AbortSignal) => Promise<void> | void): Disposable;
  onActorUnbound(handler: (actor: ZlinkStreamActor, signal?: AbortSignal) => Promise<void> | void): Disposable;
}

interface ZlinkStreamActor {
  readonly actorId: string;
  readonly isBound: boolean;                    // unbound 통지 뒤 false
  send(payload: unknown, messageType?: Function): ZlinkStreamSendCall;       // 이 Actor의 slot을 싣는다
  request(payload: unknown, messageType?: Function): ZlinkStreamRequestCall;
  on<TPayload = ZlinkStreamEncodedPayload>(
    name: string,
    handler: (message: ZlinkStreamMessage<TPayload>, signal?: AbortSignal) => Promise<void> | void,
    messageType?: Function
  ): Disposable;                                // 이 Actor가 상대인 message만
}

interface ZlinkStreamSendCall {
  packetName(name: string): ZlinkStreamSendCall;
  metadata(key: string, value: string): ZlinkStreamSendCall;
  metadata(metadata: ZlinkStreamMetadata): ZlinkStreamSendCall;
  compress(): ZlinkStreamSendCall;
  flowFrom(flow: ZlinkStreamFlow): ZlinkStreamSendCall;
  submit(): Promise<void>;
}

interface ZlinkStreamRequestCall {
  packetName(name: string): ZlinkStreamRequestCall;
  metadata(key: string, value: string): ZlinkStreamRequestCall;
  metadata(metadata: ZlinkStreamMetadata): ZlinkStreamRequestCall;
  timeout(timeoutMs: number): ZlinkStreamRequestCall;
  compress(): ZlinkStreamRequestCall;
  flowFrom(flow: ZlinkStreamFlow): ZlinkStreamRequestCall;
  submit<TReply = unknown>(signal?: AbortSignal): Promise<TReply>;
  submitEncoded(signal?: AbortSignal): Promise<ZlinkStreamEncodedPayload>;
  submit(callback: (result: ZlinkStreamResultOf<ZlinkStreamEncodedPayload>) => void): void;
}

interface ZlinkStreamWaitCall<TPayload = ZlinkStreamEncodedPayload> {
  where(predicate: (message: ZlinkStreamMessage<TPayload>) => boolean): ZlinkStreamWaitCall<TPayload>;
  timeout(timeoutMs: number): ZlinkStreamWaitCall<TPayload>;
  submit(signal?: AbortSignal): Promise<ZlinkStreamMessage<TPayload>>;
}

interface ZlinkStreamExpectNoneCall<TPayload = ZlinkStreamEncodedPayload> {
  within(windowMs: number): ZlinkStreamExpectNoneCall<TPayload>;
  run(signal?: AbortSignal): Promise<void>;
}

interface ZlinkStreamSequenceCall<TPayload = ZlinkStreamEncodedPayload> {
  // 도착 순서대로 적용할 다음 술어를 더한다. 인자는 payload가 아니라 message다.
  expect(predicate: (message: ZlinkStreamMessage<TPayload>) => boolean): ZlinkStreamSequenceCall<TPayload>;
  timeout(timeoutMs: number): ZlinkStreamSequenceCall<TPayload>;
  run(signal?: AbortSignal): Promise<readonly ZlinkStreamMessage<TPayload>[]>;
}

interface Disposable { dispose(): void; }   // dispose()가 등록을 해제한다. 두 번 호출해도 오류가 아니다

interface ZlinkStreamMetadata {
  readonly count: number;
  readonly values: ReadonlyMap<string, string>;
  get(key: string): string | undefined;
  with(key: string, value: string): ZlinkStreamMetadata;
  withMany(values: Iterable<readonly [string, string]>): ZlinkStreamMetadata;
}

interface ZlinkStreamEncodedPayload {
  readonly codec: ZlinkStreamCodec;
  readonly payload: Uint8Array;
  readonly messageType?: Function;
}

interface ZlinkStreamMessage<TPayload = unknown> extends ZlinkStreamFlow {
  readonly name: string;
  readonly metadata: ZlinkStreamMetadata;
  readonly payload: TPayload;
  readonly actorId?: string;                   // 상대 bound Actor. slot 없는 frame은 undefined(공통 스펙 §5.6)
}

interface ZlinkStreamError {
  readonly code: ZlinkStreamErrorCode;
  readonly message: string;
  readonly cause?: unknown;
}

interface ZlinkStreamResult { readonly isSuccess: boolean; readonly error?: ZlinkStreamError; }
interface ZlinkStreamResultOf<T> extends ZlinkStreamResult { readonly value?: T; }

interface ZlinkStreamConnectionStateChanged {
  readonly previous: ZlinkStreamConnectionState;
  readonly current: ZlinkStreamConnectionState;
  readonly error?: ZlinkStreamError;
}

enum ZlinkStreamCodec { Raw = 0, Json = 1, MessagePack = 2, Protobuf = 3 }
enum ZlinkStreamTransport { WebSocket = 'webSocket', WebSocketSecure = 'webSocketSecure' }
enum ZlinkStreamCompression { None = 'none', Lz4 = 'lz4' }
enum ZlinkStreamDispatchMode { Manual = 'manual', Immediate = 'immediate' }
enum ZlinkStreamMessageKind { Send = 1, Request = 2, Response = 3, Error = 4, Control = 5 }
enum ZlinkStreamHeaderFlags {
  None = 0, HasRequestSeq = 0x01, HasMetadata = 0x02,
  PayloadCompressed = 0x04, HasCorrelationId = 0x08, HasFlowId = 0x10
}
enum ZlinkStreamConnectionState {
  Created = 'created', Connecting = 'connecting', Connected = 'connected',
  Reconnecting = 'reconnecting', Disconnected = 'disconnected', Closed = 'closed'
}
enum ZlinkStreamErrorCode {
  Disconnected = 'disconnected', ConfigurationError = 'configurationError',
  ValidationFailed = 'validationFailed', RequestTimeout = 'requestTimeout',
  ConnectTimeout = 'connectTimeout', FrameDecodeFailed = 'frameDecodeFailed',
  FrameTooLarge = 'frameTooLarge', SendFailed = 'sendFailed',
  CompressionFailed = 'compressionFailed', DecompressionFailed = 'decompressionFailed',
  TlsValidationFailed = 'tlsValidationFailed',
  UserCallbackFailed = 'userCallbackFailed',
  RemoteError = 'remoteError'
}

type ZlinkFlowOrigin = 'Inbound' | 'Timer' | 'Application' | 'Lifecycle';
type ZlinkStreamCloseReason =
  | 'ClientClose' | 'IdleTimeout' | 'HeartbeatTimeout'
  | 'ServerDrain' | 'ProtocolError' | 'TransportError';

class ZlinkStreamException extends Error {
  constructor(readonly error: ZlinkStreamError);   // error.code가 오류 코드를 읽는 자리다
}
```

**브라우저 런타임에서는 `TlsValidationFailed`가 발생하지 않는다.** 브라우저의 WebSocket API가
TLS 실패를 일반 연결 실패와 구분해 주지 않기 때문이다. 코드는 위 닫힌 enum에 그대로 남고 이
런타임에서 쓰이지 않을 뿐이며, 집합 자체는 바뀌지 않는다([공통 스펙
§9.1](../../32-stream-connector.ko.md#91-닫힌-오류-코드-집합)).

TypeScript는 오류를 `ZlinkStreamError`로 전달하고, 던지는 표면은 그 값을 `ZlinkStreamException`에
담는다([공통 스펙 §9.2](../../32-stream-connector.ko.md#92-전달--받는-쪽이-코드를-읽을-수-있어야-한다)).
`Error`를 그대로 던지지 않으므로 호출자는 `catch`한 값의 `error.code`로 [공통 스펙
§9](../../32-stream-connector.ko.md#9-오류-의미)의 13개 중 무엇인지 판정한다. callback 종결자는
같은 값을 `ZlinkStreamResultOf<T>.error`로 전달한다.

[공통 스펙 §5](../../32-stream-connector.ko.md#5-packet-모델)가 요구하는 **타입에 packet 이름을
붙이는 수단은 정적 멤버다.** TypeScript에는 attribute나 annotation이 없으므로 payload 생성자에
이름을 둔다.

```ts
class OrderChanged {
  static readonly packetName = 'order.changed';   // 타입에 붙인 packet 이름
}
```

기본 `nameResolver`는 이 정적 멤버를 우선하고, 없으면 생성자의 `name`을 사용한다. 호출자가
builder의 `packetName(...)`으로 명시하면 그 이름이 가장 우선한다.

Connector options와 transport·codec 확장점도 package root의 public 표면이다. 사용자가 지정하는
optional 값과 connector가 기본값을 채워 노출하는 required 값은 서로 다른 interface로 고정한다.

```ts
interface ZlinkStreamConnectorOptions {
  readonly endpoint: string;
  readonly codec?: ZlinkStreamPayloadCodec;   // 공통 스펙 §5.4의 typed payload codec 주입점. 생략하면 JSON
  readonly transport?: ZlinkStreamTransport;
  readonly transportFactory?: ZlinkStreamTransportFactory;
  readonly connectTimeoutMs?: number;
  readonly requestTimeoutMs?: number;
  readonly waitTimeoutMs?: number;
  readonly heartbeat?: ZlinkStreamHeartbeatOptions;
  readonly reconnect?: ZlinkStreamReconnectOptions;
  readonly maxSendPayloadSize?: number;
  readonly maxReceivePayloadSize?: number;
  readonly dispatchMode?: ZlinkStreamDispatchMode;
  readonly compression?: ZlinkStreamCompression;
  readonly compressionCodec?: ZlinkStreamCompressionCodec;
  readonly nameResolver?: ZlinkStreamPacketNameResolver; // 공통 스펙 §5.4의 name resolver 주입점
  readonly diagnosticsLevel?: ZlinkStreamDiagnosticsLevel; // 생성 시점 초기값, 기본 Errors
}

// 계약은 공통 스펙 §13이 소유한다. 기본값 Errors, 미지 값은 구성 오류.
// Off: outbound frame flow pair 미생성(0x10 미설정), inbound flow 값 검증·전달 생략
// (구조 길이 검사 유지, ZlinkStreamMessage.flowId/flowOrigin은 undefined).
// 실행 중 read/write는 connector의 `diagnosticsLevel` getter와 `setDiagnosticsLevel(level)`가
// 제공하며, `setDiagnosticsLevelAsync(level)`가 같은 값을 바꾸는 비동기 짝이다. 동기 표면은
// 비동기 짝의 완료를 기다리지 않는다(공통 스펙 §13, 서버 스펙 26 §4.1). 미지 값은
// `setDiagnosticsLevel`도 생성 시점과 같은 ConfigurationError로 거부하며, 이전 값을
// 그대로 유지한다. 변경은 그 뒤의 처리 지점부터 적용되고 이미 만들어진 frame에는
// 소급 적용하지 않는다. `options.diagnosticsLevel`은 항상 현재 유효 level과 일치한다.
enum ZlinkStreamDiagnosticsLevel {
  Off = 'off',
  Errors = 'errors',
  Normal = 'normal',
  Detailed = 'detailed',
}

interface ZlinkStreamHeartbeatOptions {
  readonly enabled?: boolean;
  readonly intervalMs?: number;
  readonly timeoutMs?: number;
}

interface ZlinkStreamReconnectOptions {
  readonly enabled?: boolean;
  readonly initialDelayMs?: number;
  readonly maxDelayMs?: number;
  readonly backoffFactor?: number;
  readonly maxAttempts?: number | null;   // null이면 무제한. 그 밖에는 양수여야 한다(공통 스펙 §6)
}

interface ZlinkStreamPacketNameResolver { resolve(payloadType: Function): string; }
interface ZlinkStreamPayloadCodec {
  encode(payload: unknown, messageType?: Function): ZlinkStreamEncodedPayload;
  decode<T = unknown>(payload: ZlinkStreamEncodedPayload, messageType?: Function): T;
}
interface ZlinkStreamCompressionCodec {
  compress(payload: Uint8Array): Uint8Array;
  decompress(payload: Uint8Array, maxDecompressedSize: number): Uint8Array;
}
interface ZlinkStreamConnection {
  write(frame: Uint8Array, signal?: AbortSignal): Promise<void>;
  read?(signal?: AbortSignal): Promise<Uint8Array | undefined>;
  close(signal?: AbortSignal): Promise<void>;
}
interface ZlinkStreamTransportFactory {
  connect(options: RequiredZlinkStreamConnectorOptions, signal?: AbortSignal): Promise<ZlinkStreamConnection>;
}

interface RequiredZlinkStreamConnectorOptions {
  readonly endpoint: string;
  readonly transport: ZlinkStreamTransport;
  readonly connectTimeoutMs: number;
  readonly requestTimeoutMs: number;
  readonly waitTimeoutMs: number;
  readonly heartbeat: Required<ZlinkStreamHeartbeatOptions>;
  readonly reconnect: Required<ZlinkStreamReconnectOptions>;
  readonly maxSendPayloadSize: number;
  readonly maxReceivePayloadSize: number;
  readonly dispatchMode: ZlinkStreamDispatchMode;
  readonly compression: ZlinkStreamCompression;
  readonly compressionCodec?: ZlinkStreamCompressionCodec;
  readonly nameResolver: ZlinkStreamPacketNameResolver;
  readonly transportFactory: ZlinkStreamTransportFactory;
  readonly codec?: ZlinkStreamPayloadCodec;
  readonly diagnosticsLevel: ZlinkStreamDiagnosticsLevel;
}
```

connector 생성은 `zlinkStreamConnectorFactory.create(options)`를 사용한다.
**`create(options)`가 option 전 항목을 검증하며**, 검증에 실패하면 connector를 만들지 않고
`ZlinkStreamException`을 던진다([공통 스펙 §6.3](../../32-stream-connector.ko.md#63-옵션-검증)).
값 하나가 허용 범위를 벗어나면 `ValidationFailed`, 항목 사이가 맞지 않으면 `ConfigurationError`를
담는다. endpoint scheme과 `transport`의 충돌, 브라우저가 지원하지 않는 `tcp`·`tls`,
`compression: none`에 `compressionCodec`을 함께 넣는 것이 뒤쪽에 해당한다.

- **취소는 optional `AbortSignal`로 전달한다.** 다른 언어의 cancellation token 모양을 복제하지
  않는다([비동기 실행과 coroutine 정책](../../../server/01-execution/README.ko.md)).
- **`on(...)`과 event 구독은 `Disposable`을 반환한다.** 해제는 그 `Disposable`의 `dispose()`로
  하며, 같은 값을 두 번 해제해도 오류로 처리하지 않는다([공통 스펙 §7](../../32-stream-connector.ko.md#7-dispatch-모드)).
- `send`·`request`·`waitFor`는 즉시 실행하지 않고 **call builder를 반환한다.** builder에
  `packetName(...)`·`metadata(...)`·`timeout(...)`·`compress()`를 붙인 뒤 `submit()`으로 제출한다.
  `send`의 `submit()`은 응답을 기다리지 않으며 전송 결과나 admission status 없이 비동기 완료와 실패만
  전달한다.
- 수신 message는 `flowId`와 `flowOrigin`을 노출한다([공통 스펙 §5.5](../../32-stream-connector.ko.md#55-flow-노출과-전파)).
  `diagnosticsLevel`이 `Off`이면 두 값이 `undefined`다.
- inbound handler가 시작한 관련 outbound에는 `flowFrom(message)`를 호출한다. 이 메서드는 message의
  `flowId`와 `flowOrigin`을 한 쌍으로 복사한다. 호출하지 않은 outbound는 `origin=application`인 새
  flow를 시작한다. 자세한 비동기 문맥 경계는 [flow correlation §6](../../../server/06-observability/04-flow-correlation.ko.md#6-async-작업과-execution-context)를
  따른다.
- **`flowFrom(message)`로 flow를 명시해 전달하는 것은 브라우저 JavaScript의 환경 제약이다.**
  브라우저에는 `AsyncLocalStorage`에 해당하는 ambient 실행 문맥이 없어 connector가 handler 실행
  문맥에 현재 flow를 보관할 수 없다([공통 스펙 §2.2](../../32-stream-connector.ko.md#22-환경-제약이-계약에-미치는-영향),
  [§5.5](../../32-stream-connector.ko.md#55-flow-노출과-전파)). 명시 전달과 ambient 전파는 호출
  방식이 다를 뿐 만들어진 frame의 flow 값은 같다. connector의 변경 가능한 field나 module 전역
  변수로 현재 flow를 추정하지 않는다.

option의 기본값은 [공통 스펙 §6.1](../../32-stream-connector.ko.md)이 소유한다. TypeScript는 이를
`ZlinkStreamConnectorOptions`의 필드로 표현하며, 해석된 전체 값을 `RequiredZlinkStreamConnectorOptions`로
노출한다.

### 4.1 테스트 대기 표면

계약은 [공통 스펙 §10.1](../../32-stream-connector.ko.md)가 소유한다. TypeScript 표면은 다음과 같다.

**push 관측 — connector 메서드**(`waitFor`와 같은 자리). 각각 builder를 반환한다.

```ts
waitFor<T>(nameOrType: string | Function): ZlinkStreamWaitCall<T>;          // 도달할 때까지 대기
expectNone<T>(nameOrType: string | Function): ZlinkStreamExpectNoneCall<T>; // .within(ms) 동안 오지 않는지
waitForSequence<T>(nameOrType: string | Function): ZlinkStreamSequenceCall<T>; // .expect(p).expect(p)…를 순서대로
```

- **각 표면은 packet 이름을 호출자가 명시하는 길과 payload 생성자에서 결정하는 길을 함께
  제공한다.** 생성자를 주면 options의 `nameResolver`가 이름을 결정한다. TypeScript의 type은
  런타임에 남지 않으므로 type 기반 길의 인자는 생성자 값이다.
- `await expectNone<T>(name).within(ms).run(signal)` — window 안에 도착하면 **`ValidationFailed`를 담은 `ZlinkStreamException`을 throw**. `waitFor`의 대칭.
- `await waitForSequence<T>(name).expect(p1).expect(p2)….timeout(ms).run(signal)` — 같은 이름 push가 **술어 순서대로** 도착하는지 확인하고 `ZlinkStreamMessage<T>` 배열을 돌려준다. "N개 도착"이 아니라 **"순서대로 도착"** 을 검증한다.
- **술어와 반환은 `ZlinkStreamMessage<T>`를 다룬다.** `where(...)`와 `expect(...)`가 받는 인자도 payload가 아니라 message다.
- **status 전용 표면을 두지 않는다.** status는 payload 필드이므로
  `waitFor<T>(name).where(message => message.payload.status === …)`로 표현한다.

- **도메인 REST 폴링은 이 표면이 아니다.** 그건 HTTP client의 일이다.

## 5. 수신 큐

수신 큐의 계약은 [공통 스펙 §10](../../32-stream-connector.ko.md#10-수신-메시지-큐)이 소유한다.
TypeScript 표면에는 수신 큐 관련 option이나 오류 코드가 없다.

**`receivedCount(name)`은 packet 이름별 수신 개수를 돌려준다**(§4). 소비해도 줄지 않고
dispatch mode와 무관하며, 연결이 성립할 때 0에서 다시 시작한다.

## 6. 세션 종료 사유 (close reason)

사유의 값 집합과 의미는 [공통 스펙 §6.2](../../32-stream-connector.ko.md#62-종료-사유)가 소유한다. 이 문서는
TypeScript 표면만 고정한다.

`ZlinkStreamCloseReason`은 닫힌 union이다.

```ts
type ZlinkStreamCloseReason =
  | 'ClientClose' | 'IdleTimeout' | 'HeartbeatTimeout'
  | 'ServerDrain' | 'ProtocolError' | 'TransportError';
```

**TypeScript에서는 이 값을 connector의 읽기 전용 속성 `closeReason`으로 노출한다.**
`onDisconnected(...)` handler는 인자로 사유를 받지 않으므로, handler 안에서 `closeReason`을
읽는다. 공통 스펙 §7이 "handler가 사유를 인자로 받는지는 언어가 정한다"로 두고 있으며,
TypeScript는 읽기 속성 쪽을 택한다. 아직 끊긴 적이 없으면 `undefined`다. 첫 connect가
실패한 경우에도 사유가 남는다(공통 스펙 §6.2).

## 7. 검증

공통 동작의 검증 범위는 [공통 Stream Connector 스펙 §12](../../32-stream-connector.ko.md#12-회귀-테스트)가
소유한다. TypeScript 표면은 해당 항목을 browser의 WS/WSS 환경에서 같은 의미로 검증해야 한다. 이 문서는
공개 TypeScript 시그니처와 브라우저 실행 환경을 고정한다.

---
<!-- framework-adapter-nav:bottom:start -->
[문서 목록](../../../../../../README.ko.md)
<!-- framework-adapter-nav:bottom:end -->
