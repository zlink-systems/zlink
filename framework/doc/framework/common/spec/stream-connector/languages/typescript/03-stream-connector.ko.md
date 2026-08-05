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

  connect(signal?: AbortSignal): Promise<void>;
  close(signal?: AbortSignal): Promise<void>;
  dispatch(signal?: AbortSignal): Promise<void>;

  send(payload: unknown, messageType?: Function): ZlinkStreamSendCall;
  request(payload: unknown, messageType?: Function): ZlinkStreamRequestCall;
  waitFor<TPayload = ZlinkStreamEncodedPayload>(name: string): ZlinkStreamWaitCall<TPayload>;
  expectNone<TPayload = ZlinkStreamEncodedPayload>(name: string): ZlinkStreamExpectNoneCall<TPayload>;
  waitForSequence<TPayload = ZlinkStreamEncodedPayload>(name: string): ZlinkStreamSequenceCall<TPayload>;
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
  observeInbound(
    observer: (observation: ZlinkStreamInboundObservation, signal?: AbortSignal) => Promise<void> | void
  ): Disposable;
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
  expect(predicate: (payload: TPayload) => boolean): ZlinkStreamSequenceCall<TPayload>;
  timeout(timeoutMs: number): ZlinkStreamSequenceCall<TPayload>;
  run(signal?: AbortSignal): Promise<readonly TPayload[]>;
}

interface Disposable { dispose(): void; }

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

interface ZlinkStreamInboundObservation {
  readonly kind: ZlinkStreamMessageKind;
  readonly name: string;
  readonly codec: ZlinkStreamCodec;
  readonly requestSeq?: bigint;
  readonly flowId?: string;
  readonly flowOrigin?: ZlinkFlowOrigin;
  readonly metadata: ZlinkStreamMetadata;
  readonly payloadLength: number;
  readonly isCompressed: boolean;
  readonly receivedAt: Date;
  readonly payloadPreview: Uint8Array;
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
  UserCallbackFailed = 'userCallbackFailed', ObserverFailed = 'observerFailed',
  ObserverDropped = 'observerDropped', ReceivedMessageDropped = 'receivedMessageDropped',
  RemoteError = 'remoteError'
}

type ZlinkFlowOrigin = 'Inbound' | 'Timer' | 'Application' | 'Lifecycle';
type ZlinkStreamCloseReason =
  | 'ClientClose' | 'IdleTimeout' | 'HeartbeatTimeout'
  | 'ServerDrain' | 'ProtocolError' | 'TransportError';

class ZlinkStreamException extends Error {
  constructor(readonly error: ZlinkStreamError);
}
```

Connector options와 transport·codec 확장점도 package root의 public 표면이다. 사용자가 지정하는
optional 값과 connector가 기본값을 채워 노출하는 required 값은 서로 다른 interface로 고정한다.

```ts
interface ZlinkStreamMeterProvider {
  getMeter(name: string): {
    createCounter(name: string, options?: { readonly unit?: string }): {
      add(value: number, attributes?: Readonly<Record<string, string | number | boolean>>): void;
    };
    createHistogram(name: string, options?: { readonly unit?: string }): {
      record(value: number, attributes?: Readonly<Record<string, string | number | boolean>>): void;
    };
  };
}

interface ZlinkStreamConnectorOptions {
  readonly endpoint: string;
  readonly codec?: ZlinkStreamPayloadCodec;
  readonly transport?: ZlinkStreamTransport;
  readonly transportFactory?: ZlinkStreamTransportFactory;
  readonly connectTimeoutMs?: number;
  readonly requestTimeoutMs?: number;
  readonly waitTimeoutMs?: number;
  readonly heartbeat?: ZlinkStreamHeartbeatOptions;
  readonly reconnect?: ZlinkStreamReconnectOptions;
  readonly maxSendPayloadSize?: number;
  readonly maxReceivePayloadSize?: number;
  readonly maxReceivedMessages?: number;
  readonly maxInboundObserverNotifications?: number;
  readonly maxInboundObserverPayloadPreviewBytes?: number;
  readonly dispatchMode?: ZlinkStreamDispatchMode;
  readonly compression?: ZlinkStreamCompression;
  readonly compressionCodec?: ZlinkStreamCompressionCodec;
  readonly nameResolver?: ZlinkStreamPacketNameResolver;
  readonly meterProvider?: ZlinkStreamMeterProvider;
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
  readonly maxAttempts?: number;
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
  readonly maxReceivedMessages: number;
  readonly maxInboundObserverNotifications: number;
  readonly maxInboundObserverPayloadPreviewBytes: number;
  readonly dispatchMode: ZlinkStreamDispatchMode;
  readonly compression: ZlinkStreamCompression;
  readonly compressionCodec?: ZlinkStreamCompressionCodec;
  readonly nameResolver: ZlinkStreamPacketNameResolver;
  readonly transportFactory: ZlinkStreamTransportFactory;
  readonly codec?: ZlinkStreamPayloadCodec;
  readonly meterProvider?: ZlinkStreamMeterProvider;
}
```

connector 생성은 `zlinkStreamConnectorFactory.create(options)`를 사용한다.

- **취소는 optional `AbortSignal`로 전달한다.** 다른 언어의 cancellation token 모양을 복제하지
  않는다([비동기 실행과 coroutine 정책](../../../05-async-execution-policy.ko.md)).
- **event 구독은 `Disposable`을 반환한다.** 해제는 그 `Disposable`로 한다.
- `send`·`request`·`waitFor`는 즉시 실행하지 않고 **call builder를 반환한다.** builder에
  `packetName(...)`·`metadata(...)`·`timeout(...)`·`compress()`를 붙인 뒤 `submit()`으로 제출한다.
  `send`의 `submit()`은 응답을 기다리지 않으며 전송 결과나 admission status 없이 비동기 완료와 실패만
  전달한다.
- inbound handler가 시작한 관련 outbound에는 `flowFrom(message)`를 호출한다. 이 메서드는 message의
  `flowId`와 `flowOrigin`을 한 쌍으로 복사한다. 호출하지 않은 outbound는 `origin=application`인 새
  flow를 시작한다. 자세한 비동기 문맥 경계는 [flow correlation §6](../../../27-flow-correlation.ko.md#6-async-작업과-execution-context)를
  따른다.

option의 기본값은 [공통 스펙 §6.1](../../32-stream-connector.ko.md)이 소유한다. TypeScript는 이를
`ZlinkStreamConnectorOptions`의 필드로 표현하며, 해석된 전체 값을 `RequiredZlinkStreamConnectorOptions`로
노출한다.

### 4.1 테스트 대기 표면

계약은 [공통 스펙 §10.2](../../32-stream-connector.ko.md)가 소유한다. TypeScript 표면은 다음과 같다.

**push 관측 — connector 메서드**(`waitFor`와 같은 자리). 각각 builder를 반환한다.

```ts
waitFor<T>(name: string): ZlinkStreamWaitCall<T>;          // 도달할 때까지 대기
expectNone<T>(name: string): ZlinkStreamExpectNoneCall<T>; // .within(ms) 동안 오지 않는지
waitForSequence<T>(name: string): ZlinkStreamSequenceCall<T>; // .expect(p).expect(p)…를 순서대로
```

- `await expectNone<T>(name).within(ms).run(signal)` — window 안에 도착하면 **throw**. `waitFor`의 대칭.
- `await waitForSequence<T>(name).expect(p1).expect(p2)….timeout(ms).run(signal)` — 같은 이름 push가 **술어 순서대로** 도착하는지 확인하고 payload 배열을 돌려준다. "N개 도착"이 아니라 **"순서대로 도착"** 을 검증한다.
- **status 전용 표면을 두지 않는다.** status는 payload 필드이므로
  `waitFor<T>(name).where(message => message.payload.status === …)`로 표현한다.

- **도메인 REST 폴링은 이 표면이 아니다.** 그건 HTTP client의 일이다.

## 5. Inbound Observer와 수신 큐

관찰 의미와 격리·overflow 규칙은 [공통 스펙 §10](../../32-stream-connector.ko.md)이 소유한다. 이
문서는 TypeScript 표면만 고정한다.

`observeInbound(...)`는 `Disposable`을 반환하며, **`connect(...)` 호출 전에만** 등록할 수 있다.
연결이 시작된 뒤 등록하면 오류를 던진다.

두 큐의 한도는 다음 option으로 조절한다. 기본값은
[공통 스펙 §6.1](../../32-stream-connector.ko.md)이 소유한다.

| option | 대상 큐 | overflow 시 error handler로 보고하는 코드 |
|---|---|---|
| `maxInboundObserverNotifications` | observer notification 큐 | `ZlinkStreamErrorCode.ObserverDropped` |
| `maxReceivedMessages` | 수신 메시지 큐(§10.1) | `ZlinkStreamErrorCode.ReceivedMessageDropped` |

observer callback 실패는 `ZlinkStreamErrorCode.ObserverFailed`로 보고한다.
`maxInboundObserverPayloadPreviewBytes`는 observation에 담을 payload preview 길이를 정한다.

## 6. 세션 종료 사유 (close reason)

사유의 값 집합과 의미는 [공통 스펙 §6.3](../../32-stream-connector.ko.md#63-종료-사유)가 소유한다. 이 문서는
TypeScript 표면만 고정한다.

`ZlinkStreamCloseReason`은 닫힌 union이다.

```ts
type ZlinkStreamCloseReason =
  | 'ClientClose' | 'IdleTimeout' | 'HeartbeatTimeout'
  | 'ServerDrain' | 'ProtocolError' | 'TransportError';
```

**TypeScript에서는 이 값을 connector의 읽기 전용 속성 `closeReason`으로 노출한다.**
`onDisconnected(...)` handler는 인자로 사유를 받지 않으므로, handler 안에서 `closeReason`을
읽는다. 아직 끊긴 적이 없으면 `undefined`다.

### 6.1 Metric

TypeScript connector는 [공통 스펙 §6.2](../../32-stream-connector.ko.md#62-connector-reconnect-계기)의
`zlink.stream.reconnects`와 닫힌 attribute를 `meterProvider`에 게시한다. Provider를 생략하면
OpenTelemetry API의 global meter provider를 사용한다. Application과 E2E는 OpenTelemetry public reader로
counter를 읽는다. Provider 또는 exporter failure는 send, request와 연결 상태를 바꾸지 않는다.

## 7. 검증

공통 동작의 검증 범위는 [공통 Stream Connector 스펙 §12](../../32-stream-connector.ko.md#12-회귀-테스트)가
소유한다. TypeScript 표면은 해당 항목을 browser의 WS/WSS 환경에서 같은 의미로 검증해야 한다. 이 문서는
공개 TypeScript 시그니처와 브라우저 실행 환경을 고정한다.

---
<!-- framework-adapter-nav:bottom:start -->
[문서 목록](../../../../../../README.ko.md)
<!-- framework-adapter-nav:bottom:end -->
