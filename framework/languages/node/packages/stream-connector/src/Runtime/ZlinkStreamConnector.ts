import {
  Disposable,
  RequiredZlinkStreamConnectorOptions,
  ZlinkStreamCodec,
  ZlinkStreamConnectionState,
  ZlinkStreamConnectionStateChanged,
  ZlinkStreamConnector,
  ZlinkStreamConnectorOptions,
  ZlinkStreamDispatchMode,
  ZlinkStreamEncodedPayload,
  ZlinkStreamError,
  ZlinkStreamErrorCode,
  ZlinkStreamException,
  ZlinkStreamExpectNoneCall,
  zlinkStreamJsonCodec,
  ZlinkStreamMessage,
  ZlinkStreamRequestSendingContext,
  ZlinkStreamReplyReceivedContext,
  ZlinkStreamMessageKind,
  ZlinkStreamMetadata,
  ZlinkStreamRequestCall,
  ZlinkStreamActor,
  ZlinkStreamSendCall,
  ZlinkStreamSequenceCall,
  ZlinkStreamWaitCall
} from '../Contracts';
import {
  ZlinkStreamRequestBuilder,
  ZlinkStreamSendBuilder,
  ZlinkStreamWaitBuilder
} from './Calls/ZlinkStreamCallBuilders';
import {
  ZlinkStreamExpectNoneBuilder,
  ZlinkStreamSequenceBuilder
} from './Calls/ZlinkStreamObservationBuilders';
import {
  ZLINK_STREAM_HEARTBEAT_PING,
  ZLINK_STREAM_HEARTBEAT_PONG,
  ZlinkStreamFrameProtocol
} from './Protocol/ZlinkStreamFrameProtocol';
import { validateName } from './Protocol/ZlinkStreamPacketNameValidator';
import { normalizeOptions } from './ZlinkStreamConnectorOptions';
import {
  connectorError,
  throwIfAborted,
  unwrapStreamError,
  subscription
} from './ZlinkStreamSupport';
import { ZlinkStreamPendingRequests } from './ZlinkStreamPendingRequests';
import { ZlinkStreamReceivedMessages } from './ZlinkStreamReceivedMessages';
import { ZlinkStreamFrameSender } from './ZlinkStreamFrameSender';
import { ZlinkStreamReceiveDispatcher } from './ZlinkStreamReceiveDispatcher';
import { ZlinkStreamConnectorLifecycle } from './ZlinkStreamConnectorLifecycle';
import { ZlinkStreamConnectorEvents } from './ZlinkStreamConnectorEvents';
import { BrowserStreamTransportFactory } from './Transport/BrowserWebSocketConnection';
import { DefaultZlinkStreamActor, ZlinkStreamActors } from './ZlinkStreamActors';

export class DefaultZlinkStreamConnector implements ZlinkStreamConnector {
  static readonly heartbeatPingName = ZLINK_STREAM_HEARTBEAT_PING;
  static readonly heartbeatPongName = ZLINK_STREAM_HEARTBEAT_PONG;

  private readonly receivedMessages: ZlinkStreamReceivedMessages;
  private readonly lifecycle: ZlinkStreamConnectorLifecycle;
  private readonly events = new ZlinkStreamConnectorEvents((callback, callbacks) =>
    this.receivedMessages.enqueueCallback(callback, callbacks)
  );
  private correlationCounter = 0n;
  private readonly pendingRequests = new ZlinkStreamPendingRequests();
  private readonly frameSender: ZlinkStreamFrameSender;
  private readonly receiveDispatcher: ZlinkStreamReceiveDispatcher;
  private readonly requestSendingHandlers = new Set<
    (context: ZlinkStreamRequestSendingContext) => void
  >();
  private readonly replyReceivedHandlers = new Set<
    (context: ZlinkStreamReplyReceivedContext, signal?: AbortSignal) => Promise<void> | void
  >();
  private readonly boundActors: ZlinkStreamActors;

  readonly options: RequiredZlinkStreamConnectorOptions;

  constructor(options: ZlinkStreamConnectorOptions) {
    this.options = normalizeOptions(options, new BrowserStreamTransportFactory());
    const protocol = new ZlinkStreamFrameProtocol(this.options);
    this.frameSender = new ZlinkStreamFrameSender(protocol);
    this.receivedMessages = new ZlinkStreamReceivedMessages(
      this.events,
      this.options.dispatchMode === ZlinkStreamDispatchMode.Immediate
    );
    this.boundActors = new ZlinkStreamActors(this, this.receivedMessages, this.events);
    this.receiveDispatcher = new ZlinkStreamReceiveDispatcher(
      protocol,
      this.pendingRequests,
      this.receivedMessages,
      this.frameSender,
      this.events,
      this.boundActors,
      (reason) => this.lifecycle.serverClosing(reason)
    );
    this.lifecycle = new ZlinkStreamConnectorLifecycle(
      this.options,
      this.pendingRequests,
      this.frameSender,
      this.receiveDispatcher,
      this.receivedMessages,
      this.boundActors,
      this.events
    );
  }

  get isConnected(): boolean {
    return this.lifecycle.isConnected;
  }

  get closeReason() {
    return this.lifecycle.closeReason;
  }

  get state(): ZlinkStreamConnectionState {
    return this.lifecycle.state;
  }

  get pendingDispatchCount(): number {
    return this.receivedMessages.pendingCallbacks;
  }

  get actors(): readonly ZlinkStreamActor[] {
    return this.boundActors.snapshot;
  }

  actor(actorId: string): ZlinkStreamActor | undefined {
    return this.boundActors.find(actorId);
  }

  onActorBound(
    handler: (actor: ZlinkStreamActor, signal?: AbortSignal) => Promise<void> | void
  ): Disposable {
    return this.boundActors.onBound(handler);
  }

  onActorUnbound(
    handler: (actor: ZlinkStreamActor, signal?: AbortSignal) => Promise<void> | void
  ): Disposable {
    return this.boundActors.onUnbound(handler);
  }

  /**
   * Spec stream-connector 32 §10: how many packets carrying `name` arrived on
   * the current connection. Arrivals are what is counted, so a message a
   * handler dispatched or a wait surface consumed still counts, and `Manual`
   * and `Immediate` report the same number. Establishing a connection resets
   * the count to 0, a reconnect included.
   */
  receivedCount(name: string): number {
    validateName(name);
    return this.receivedMessages.receivedCount(name);
  }

  onErrorReceived(
    handler: (error: ZlinkStreamError, signal?: AbortSignal) => Promise<void> | void
  ): Disposable {
    return this.events.onError(handler);
  }

  onDisconnected(handler: (signal?: AbortSignal) => Promise<void> | void): Disposable {
    return this.events.onDisconnected(handler);
  }

  onConnectionStateChanged(
    handler: (
      change: ZlinkStreamConnectionStateChanged,
      signal?: AbortSignal
    ) => Promise<void> | void
  ): Disposable {
    return this.events.onStateChanged(handler);
  }

  onRequestSending(handler: (context: ZlinkStreamRequestSendingContext) => void): Disposable {
    this.requestSendingHandlers.add(handler);
    return subscription(() => this.requestSendingHandlers.delete(handler));
  }

  onReplyReceived(
    handler: (
      context: ZlinkStreamReplyReceivedContext,
      signal?: AbortSignal
    ) => Promise<void> | void
  ): Disposable {
    this.replyReceivedHandlers.add(handler);
    return subscription(() => this.replyReceivedHandlers.delete(handler));
  }

  async connect(signal?: AbortSignal): Promise<void> {
    await this.lifecycle.connect(signal);
  }

  async close(signal?: AbortSignal): Promise<void> {
    await this.lifecycle.close(signal);
  }

  async dispatch(signal?: AbortSignal): Promise<void> {
    await this.lifecycle.dispatch(signal);
  }

  send(payload: unknown, messageType?: Function): ZlinkStreamSendCall {
    const encoded = this.encodePayload(payload, messageType);
    return new ZlinkStreamSendBuilder(this, this.resolveNameOrDefault(encoded), encoded);
  }

  request(payload: unknown, messageType?: Function): ZlinkStreamRequestCall {
    const encoded = this.encodePayload(payload, messageType);
    return new ZlinkStreamRequestBuilder(
      this,
      this.resolveNameOrDefault(encoded),
      encoded,
      (callback) => this.receivedMessages.enqueueCallback(callback)
    );
  }

  on<TPayload = ZlinkStreamEncodedPayload>(
    nameOrType: string | Function,
    handler: (message: ZlinkStreamMessage<TPayload>, signal?: AbortSignal) => Promise<void> | void,
    messageType?: Function
  ): Disposable {
    const encodedHandler = (
      message: ZlinkStreamMessage<ZlinkStreamEncodedPayload>,
      signal?: AbortSignal
    ) =>
      handler(
        {
          name: message.name,
          metadata: message.metadata,
          payload: this.decodePayload<TPayload>(
            message.payload,
            messageType ?? (typeof nameOrType === 'function' ? nameOrType : undefined)
          ),
          actorId: message.actorId
        },
        signal
      );
    return this.receivedMessages.on(this.observedName(nameOrType), encodedHandler);
  }

  sendForActor(
    actor: DefaultZlinkStreamActor,
    payload: unknown,
    messageType?: Function
  ): ZlinkStreamSendCall {
    const encoded = this.encodePayload(payload, messageType);
    return new ZlinkStreamSendBuilder(
      this,
      this.resolveNameOrDefault(encoded),
      encoded,
      actor.slot,
      () => actor.ensureBound()
    );
  }

  requestForActor(
    actor: DefaultZlinkStreamActor,
    payload: unknown,
    messageType?: Function
  ): ZlinkStreamRequestCall {
    const encoded = this.encodePayload(payload, messageType);
    return new ZlinkStreamRequestBuilder(
      this,
      this.resolveNameOrDefault(encoded),
      encoded,
      (callback) => this.receivedMessages.enqueueCallback(callback),
      actor.slot,
      () => actor.ensureBound()
    );
  }

  onActorMessage<TPayload>(
    actor: DefaultZlinkStreamActor,
    nameOrType: string | Function,
    handler: (message: ZlinkStreamMessage<TPayload>, signal?: AbortSignal) => Promise<void> | void,
    messageType?: Function
  ): Disposable {
    const encodedHandler = (
      message: ZlinkStreamMessage<ZlinkStreamEncodedPayload>,
      signal?: AbortSignal
    ) =>
      handler(
        {
          name: message.name,
          metadata: message.metadata,
          payload: this.decodePayload<TPayload>(
            message.payload,
            messageType ?? (typeof nameOrType === 'function' ? nameOrType : undefined)
          ),
          actorId: message.actorId
        },
        signal
      );
    return this.receivedMessages.on(this.observedName(nameOrType), encodedHandler, actor);
  }

  waitFor<TPayload = ZlinkStreamEncodedPayload>(
    nameOrType: string | Function
  ): ZlinkStreamWaitCall<TPayload> {
    return new ZlinkStreamWaitBuilder<TPayload>(this, this.observedName(nameOrType));
  }

  expectNone<TPayload = ZlinkStreamEncodedPayload>(
    nameOrType: string | Function
  ): ZlinkStreamExpectNoneCall<TPayload> {
    return new ZlinkStreamExpectNoneBuilder<TPayload>(this, this.observedName(nameOrType));
  }

  waitForSequence<TPayload = ZlinkStreamEncodedPayload>(
    nameOrType: string | Function
  ): ZlinkStreamSequenceCall<TPayload> {
    return new ZlinkStreamSequenceBuilder<TPayload>(this, this.observedName(nameOrType));
  }

  /**
   * Spec stream-connector 32 §10.1: each wait surface offers both ways of
   * naming a packet. A string is the name the caller states; a constructor
   * goes through the options' `nameResolver`, which is the same resolver
   * `send` and `request` use, so both paths land on one name for one type.
   */
  private observedName(nameOrType: string | Function): string {
    const name =
      typeof nameOrType === 'function' ? this.options.nameResolver.resolve(nameOrType) : nameOrType;
    if (typeof name !== 'string') {
      throw connectorError(
        ZlinkStreamErrorCode.ValidationFailed,
        'Packet name must be a string or a payload constructor.'
      );
    }
    validateName(name);
    return name;
  }

  /**
   * Consumes the first message under `name` that `predicate` accepts, or
   * resolves `undefined` when `timeoutMs` elapses first.
   *
   * A timeout is not a failure here. Each wait surface decides what its own
   * timeout means — `waitFor` fails on it while `expectNone` succeeds — and
   * reports that decision as `ValidationFailed` (spec stream-connector 32
   * §10.1.1; .NET `ZlinkStreamReceivedMessages.WaitForAsync` parity). Losing
   * the connection the wait observes is the one ending decided here, because
   * the wait has no place left to observe: that is `Disconnected` for every
   * surface (§10.1.1).
   */
  waitForMessage<TPayload>(
    name: string,
    timeoutMs: number,
    predicate: (message: ZlinkStreamMessage<TPayload>) => boolean,
    signal?: AbortSignal
  ): Promise<ZlinkStreamMessage<TPayload> | undefined> {
    validateName(name);
    if (!Number.isFinite(timeoutMs) || timeoutMs < 0) {
      throw connectorError(
        ZlinkStreamErrorCode.ValidationFailed,
        'Timeout must be a non-negative finite number.'
      );
    }
    throwIfAborted(signal);
    return new Promise((resolve, reject) => {
      let done = false;
      let timer: ReturnType<typeof setTimeout> | undefined;
      let disposable: Disposable | undefined;
      const onAbort = () =>
        finish(connectorError(ZlinkStreamErrorCode.Disconnected, 'Operation canceled.'));
      const finish = (error?: unknown, message?: ZlinkStreamMessage<TPayload>) => {
        if (done) {
          return;
        }
        done = true;
        signal?.removeEventListener('abort', onAbort);
        if (timer !== undefined) {
          clearTimeout(timer);
        }
        disposable?.dispose();
        if (error !== undefined) {
          reject(error);
        } else {
          resolve(message);
        }
      };
      timer = setTimeout(() => finish(), timeoutMs);
      signal?.addEventListener('abort', onAbort, { once: true });
      // Spec stream-connector 32 §7: a wait surface is not a registered
      // callback. It observes the packets the receive queue has not delivered
      // yet and consumes the one it matches, in both dispatch modes, so
      // `Manual` completes this wait without a dispatch pump.
      disposable = this.receivedMessages.observe(
        name,
        (message) => {
          if (done) {
            return false;
          }
          try {
            const decoded = {
              name: message.name,
              metadata: message.metadata,
              payload: this.decodeWaitPayload<TPayload>(message.payload),
              actorId: message.actorId
            };
            if (!predicate(decoded)) {
              return false;
            }
            finish(undefined, decoded);
          } catch (cause) {
            finish(cause);
          }
          return true;
        },
        () =>
          finish(
            connectorError(
              ZlinkStreamErrorCode.Disconnected,
              `The connection this wait for '${name}' observed has ended.`
            )
          )
      );
    });
  }

  private encodePayload(payload: unknown, messageType?: Function): ZlinkStreamEncodedPayload {
    if (isEncodedPayload(payload)) {
      return payload;
    }
    const codec = this.options.codec ?? zlinkStreamJsonCodec;
    return codec.encode(payload, messageType);
  }

  private decodePayload<TPayload>(
    payload: ZlinkStreamEncodedPayload,
    messageType?: Function
  ): TPayload {
    if (messageType === undefined && this.options.codec === undefined) {
      return payload as TPayload;
    }
    return (this.options.codec ?? zlinkStreamJsonCodec).decode<TPayload>(payload, messageType);
  }

  private decodeWaitPayload<TPayload>(payload: ZlinkStreamEncodedPayload): TPayload {
    if (this.options.codec !== undefined || payload.codec === ZlinkStreamCodec.Json) {
      return (this.options.codec ?? zlinkStreamJsonCodec).decode<TPayload>(payload);
    }
    return payload as TPayload;
  }

  async sendEncoded(
    kind: ZlinkStreamMessageKind,
    name: string,
    payload: ZlinkStreamEncodedPayload,
    metadata: ZlinkStreamMetadata,
    compress: boolean,
    requestSeq: bigint | undefined,
    signal?: AbortSignal,
    correlationId?: string,
    actorSlot?: number,
    expiry?: Promise<unknown>,
    onAccepted?: () => void
  ): Promise<void> {
    await this.frameSender.send(
      this.lifecycle.connectionForSend(),
      kind,
      name,
      payload,
      metadata,
      compress,
      requestSeq,
      signal,
      correlationId,
      actorSlot,
      expiry,
      onAccepted
    );
  }

  /**
   * Per-connector monotonic correlation id (hex). The client generates it on each request
   * and the server echoes it back on the reply.
   */
  private nextCorrelationId(): string {
    this.correlationCounter += 1n;
    return this.correlationCounter.toString(16);
  }

  async requestEncoded(
    name: string,
    payload: ZlinkStreamEncodedPayload,
    metadata: ZlinkStreamMetadata,
    compress: boolean,
    timeoutMs: number,
    signal?: AbortSignal,
    actorSlot?: number
  ): Promise<ZlinkStreamEncodedPayload> {
    const startedAt = Date.now();
    const actorId =
      actorSlot === undefined ? undefined : this.boundActors.resolve(actorSlot).actorId;
    let requestMetadata = metadata;
    const sendingContext: ZlinkStreamRequestSendingContext = {
      requestPacketName: name,
      actorId,
      setMetadata(key, value) {
        requestMetadata = requestMetadata.with(key, value);
      }
    };
    let pending: ReturnType<ZlinkStreamPendingRequests['create']> | undefined;
    let stopCancellation: (() => void) | undefined;
    try {
      throwIfAborted(signal);
      for (const handler of Array.from(this.requestSendingHandlers)) {
        try {
          handler(sendingContext);
        } catch (cause) {
          this.events.publishError(
            {
              code: ZlinkStreamErrorCode.UserCallbackFailed,
              message: 'Request sending hook failed.',
              cause
            },
            signal
          );
        }
      }
      pending = this.pendingRequests.create(name, timeoutMs);
      const accepted = pending;
      const write = this.sendEncoded(
        ZlinkStreamMessageKind.Request,
        name,
        payload,
        requestMetadata,
        compress,
        accepted.requestSeq,
        signal,
        this.nextCorrelationId(),
        actorSlot,
        accepted.promise,
        accepted.startTimeout
      );
      // Spec stream-connector 32 §5.2: a cancellation after the frame write
      // started does not stop the write; the request ends with whichever comes
      // first, its connector result or the cancellation.
      const canceled = new Promise<never>((_, reject) => {
        const onAbort = (): void =>
          reject(connectorError(ZlinkStreamErrorCode.Disconnected, 'Operation canceled.'));
        signal?.addEventListener('abort', onAbort, { once: true });
        stopCancellation = () => signal?.removeEventListener('abort', onAbort);
      });
      const reply = await Promise.race([
        write.then(() => accepted.promise),
        accepted.promise,
        canceled
      ]);
      this.publishReplyReceived(
        {
          requestPacketName: name,
          actorId,
          succeeded: true,
          reply: { ...reply, actorId },
          elapsed: Date.now() - startedAt
        },
        signal
      );
      return reply.payload;
    } catch (error) {
      if (pending !== undefined) this.pendingRequests.cancel(pending.requestSeq);
      // Spec stream-connector 32 §5.7: the reply received hook reports the
      // result the connector decided, so a request the caller canceled skips it.
      if (signal?.aborted === true) throw error;
      this.publishReplyReceived(
        {
          requestPacketName: name,
          actorId,
          succeeded: false,
          error: unwrapStreamError(error),
          elapsed: Date.now() - startedAt
        },
        signal
      );
      throw error;
    } finally {
      stopCancellation?.();
    }
  }

  private publishReplyReceived(
    context: ZlinkStreamReplyReceivedContext,
    signal?: AbortSignal
  ): void {
    if (this.replyReceivedHandlers.size === 0) return;
    this.receivedMessages.enqueueCallback(
      () => {
        for (const handler of Array.from(this.replyReceivedHandlers)) {
          this.events.runUserCallback(
            () => handler(context, signal),
            'Reply received hook failed.',
            signal
          );
        }
      },
      () => this.replyReceivedHandlers.size
    );
  }

  private resolveNameOrDefault(payload: ZlinkStreamEncodedPayload): string | undefined {
    if (payload.messageType === undefined) {
      return undefined;
    }
    return this.options.nameResolver.resolve(payload.messageType);
  }
}

function isEncodedPayload(value: unknown): value is ZlinkStreamEncodedPayload {
  if (value === null || typeof value !== 'object') {
    return false;
  }
  const candidate = value as Partial<ZlinkStreamEncodedPayload>;
  return typeof candidate.codec === 'number' && candidate.payload instanceof Uint8Array;
}
