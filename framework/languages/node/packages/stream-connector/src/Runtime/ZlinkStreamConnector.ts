import {
  Disposable,
  RequiredZlinkStreamConnectorOptions,
  ZlinkStreamCodec,
  ZlinkStreamConnectionState,
  ZlinkStreamConnectionStateChanged,
  ZlinkStreamConnector,
  ZlinkStreamConnectorOptions,
  ZlinkStreamDiagnosticsLevel,
  ZlinkStreamDispatchMode,
  ZlinkStreamEncodedPayload,
  ZlinkStreamError,
  ZlinkStreamErrorCode,
  ZlinkStreamExpectNoneCall,
  ZlinkStreamFlow,
  zlinkStreamJsonCodec,
  ZlinkStreamMessage,
  ZlinkStreamMessageKind,
  ZlinkStreamMetadata,
  ZlinkStreamRequestCall,
  ZlinkStreamSendCall,
  ZlinkStreamSequenceCall,
  ZlinkStreamWaitCall
} from '../Contracts';
import { ZlinkStreamRequestBuilder, ZlinkStreamSendBuilder, ZlinkStreamWaitBuilder } from './Calls/ZlinkStreamCallBuilders';
import { ZlinkStreamExpectNoneBuilder, ZlinkStreamSequenceBuilder } from './Calls/ZlinkStreamObservationBuilders';
import {
  ZLINK_STREAM_HEARTBEAT_PING,
  ZLINK_STREAM_HEARTBEAT_PONG,
  ZlinkStreamFrameProtocol
} from './Protocol/ZlinkStreamFrameProtocol';
import { validateName } from './Protocol/ZlinkStreamPacketNameValidator';
import { normalizeOptions } from './ZlinkStreamConnectorOptions';
import { ZlinkStreamDiagnosticsLevelCell } from './ZlinkStreamDiagnosticsLevelCell';
import { connectorError, throwIfAborted } from './ZlinkStreamSupport';
import { ZlinkStreamPendingRequests } from './ZlinkStreamPendingRequests';
import { ZlinkStreamReceivedMessages } from './ZlinkStreamReceivedMessages';
import { ZlinkStreamFrameSender } from './ZlinkStreamFrameSender';
import { ZlinkStreamReceiveDispatcher } from './ZlinkStreamReceiveDispatcher';
import { ZlinkStreamConnectorLifecycle } from './ZlinkStreamConnectorLifecycle';
import { ZlinkStreamConnectorEvents } from './ZlinkStreamConnectorEvents';
import { BrowserZlinkFlowContext, type ZlinkFlowContext } from './ZlinkFlowContext';
import { BrowserStreamTransportFactory } from './Transport/BrowserWebSocketConnection';

export class DefaultZlinkStreamConnector implements ZlinkStreamConnector {
  static readonly heartbeatPingName = ZLINK_STREAM_HEARTBEAT_PING;
  static readonly heartbeatPongName = ZLINK_STREAM_HEARTBEAT_PONG;

  private readonly receivedMessages: ZlinkStreamReceivedMessages;
  private readonly lifecycle: ZlinkStreamConnectorLifecycle;
  private readonly events = new ZlinkStreamConnectorEvents();
  private correlationCounter = 0n;
  private readonly pendingRequests = new ZlinkStreamPendingRequests();
  private readonly frameSender: ZlinkStreamFrameSender;
  private readonly receiveDispatcher: ZlinkStreamReceiveDispatcher;
  private readonly diagnosticsLevelCell: ZlinkStreamDiagnosticsLevelCell;

  readonly options: RequiredZlinkStreamConnectorOptions;

  constructor(options: ZlinkStreamConnectorOptions) {
    const flowContext: ZlinkFlowContext = new BrowserZlinkFlowContext();
    this.options = normalizeOptions(options, new BrowserStreamTransportFactory());
    // Spec 26 §4.1 / spec stream-connector 32 §13: the level is a runtime
    // control, not a construction-time constant. `options.diagnosticsLevel`
    // is redefined as a live getter over the cell so every reader of
    // `this.options` (protocol, application code) observes the
    // current level instead of the value captured at connector creation.
    this.diagnosticsLevelCell = new ZlinkStreamDiagnosticsLevelCell(this.options.diagnosticsLevel);
    Object.defineProperty(this.options, 'diagnosticsLevel', {
      enumerable: true,
      configurable: true,
      get: () => this.diagnosticsLevelCell.level
    });
    const protocol = new ZlinkStreamFrameProtocol(this.options);
    this.frameSender = new ZlinkStreamFrameSender(protocol, flowContext);
    this.receivedMessages = new ZlinkStreamReceivedMessages(
      this.events,
      this.options.dispatchMode === ZlinkStreamDispatchMode.Immediate
    );
    this.receiveDispatcher = new ZlinkStreamReceiveDispatcher(
      protocol,
      this.pendingRequests,
      this.receivedMessages,
      this.frameSender,
      this.events,
      flowContext,
      (reason) => this.lifecycle.serverClosing(reason)
    );
    this.lifecycle = new ZlinkStreamConnectorLifecycle(
      this.options,
      this.pendingRequests,
      this.frameSender,
      this.receiveDispatcher,
      this.receivedMessages,
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
    return this.pendingRequests.count;
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

  /**
   * Current diagnostics level (spec 26 §4.1, spec stream-connector 32 §13).
   * Reflects the level set by the most recent {@link setDiagnosticsLevel}
   * call, or the construction-time option (default
   * {@link ZlinkStreamDiagnosticsLevel.Errors}) if it was never changed.
   */
  get diagnosticsLevel(): ZlinkStreamDiagnosticsLevel {
    return this.diagnosticsLevelCell.level;
  }

  /**
   * Changes the diagnostics level in place without recreating the connector
   * (spec 26 §4.1, spec stream-connector 32 §13). The change is an atomic
   * state update: it applies to processing points that read the level after
   * this call returns and is never applied retroactively to frames already
   * built. Rejects unknown values with {@link ZlinkStreamErrorCode.ConfigurationError}.
   * Spec stream-connector 32 §13: this surface changes the value without
   * waiting for anything; it is not a blocking call over the asynchronous pair,
   * which a receive callback would otherwise make wait for its own completion.
   */
  setDiagnosticsLevel(level: ZlinkStreamDiagnosticsLevel): void {
    this.diagnosticsLevelCell.set(level);
  }

  /**
   * Asynchronous counterpart of {@link setDiagnosticsLevel} (spec
   * stream-connector 32 §13). It changes the same value; awaiting it is how a
   * caller observes the change, and it never replaces the synchronous surface.
   */
  setDiagnosticsLevelAsync(level: ZlinkStreamDiagnosticsLevel): Promise<void> {
    this.setDiagnosticsLevel(level);
    return Promise.resolve();
  }

  onErrorReceived(handler: (error: ZlinkStreamError, signal?: AbortSignal) => Promise<void> | void): Disposable {
    return this.events.onError(handler);
  }

  onDisconnected(handler: (signal?: AbortSignal) => Promise<void> | void): Disposable {
    return this.events.onDisconnected(handler);
  }

  onConnectionStateChanged(handler: (change: ZlinkStreamConnectionStateChanged, signal?: AbortSignal) => Promise<void> | void): Disposable {
    return this.events.onStateChanged(handler);
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
    return new ZlinkStreamRequestBuilder(this, this.resolveNameOrDefault(encoded), encoded);
  }

  on<TPayload = ZlinkStreamEncodedPayload>(
    name: string,
    handler: (message: ZlinkStreamMessage<TPayload>, signal?: AbortSignal) => Promise<void> | void,
    messageType?: Function
  ): Disposable {
    const encodedHandler = (message: ZlinkStreamMessage<ZlinkStreamEncodedPayload>, signal?: AbortSignal) => handler({
      name: message.name,
      metadata: message.metadata,
      payload: this.decodePayload<TPayload>(message.payload, messageType),
      flowId: message.flowId,
      flowOrigin: message.flowOrigin
    }, signal);
    return this.receivedMessages.on(name, encodedHandler);
  }

  waitFor<TPayload = ZlinkStreamEncodedPayload>(nameOrType: string | Function): ZlinkStreamWaitCall<TPayload> {
    return new ZlinkStreamWaitBuilder<TPayload>(this, this.observedName(nameOrType));
  }

  expectNone<TPayload = ZlinkStreamEncodedPayload>(nameOrType: string | Function): ZlinkStreamExpectNoneCall<TPayload> {
    return new ZlinkStreamExpectNoneBuilder<TPayload>(this, this.observedName(nameOrType));
  }

  waitForSequence<TPayload = ZlinkStreamEncodedPayload>(nameOrType: string | Function): ZlinkStreamSequenceCall<TPayload> {
    return new ZlinkStreamSequenceBuilder<TPayload>(this, this.observedName(nameOrType));
  }

  /**
   * Spec stream-connector 32 §10.1: each wait surface offers both ways of
   * naming a packet. A string is the name the caller states; a constructor
   * goes through the options' `nameResolver`, which is the same resolver
   * `send` and `request` use, so both paths land on one name for one type.
   */
  private observedName(nameOrType: string | Function): string {
    const name = typeof nameOrType === 'function'
      ? this.options.nameResolver.resolve(nameOrType)
      : nameOrType;
    if (typeof name !== 'string') {
      throw connectorError(ZlinkStreamErrorCode.ValidationFailed, 'Packet name must be a string or a payload constructor.');
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
      throw connectorError(ZlinkStreamErrorCode.ValidationFailed, 'Timeout must be a non-negative finite number.');
    }
    throwIfAborted(signal);
    return new Promise((resolve, reject) => {
      let done = false;
      let timer: ReturnType<typeof setTimeout> | undefined;
      let disposable: Disposable | undefined;
      const onAbort = () => finish(connectorError(ZlinkStreamErrorCode.Disconnected, 'Operation canceled.'));
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
      disposable = this.receivedMessages.observe(name, (message) => {
        if (done) {
          return false;
        }
        try {
          const decoded = {
            name: message.name,
            metadata: message.metadata,
            payload: this.decodeWaitPayload<TPayload>(message.payload),
            flowId: message.flowId,
            flowOrigin: message.flowOrigin
          };
          if (!predicate(decoded)) {
            return false;
          }
          finish(undefined, decoded);
        } catch (cause) {
          finish(cause);
        }
        return true;
      }, () => finish(connectorError(
        ZlinkStreamErrorCode.Disconnected,
        `The connection this wait for '${name}' observed has ended.`
      )));
    });
  }

  private encodePayload(payload: unknown, messageType?: Function): ZlinkStreamEncodedPayload {
    if (isEncodedPayload(payload)) {
      return payload;
    }
    const codec = this.options.codec ?? zlinkStreamJsonCodec;
    return codec.encode(payload, messageType);
  }

  private decodePayload<TPayload>(payload: ZlinkStreamEncodedPayload, messageType?: Function): TPayload {
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
    flow?: ZlinkStreamFlow,
    correlationId?: string
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
      flow
    );
  }

  /**
   * Per-connector monotonic correlation id (hex). The client generates it on each request
   * and the server echoes it back on the reply, so flows can be joined across the wire.
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
    flow?: ZlinkStreamFlow
  ): Promise<ZlinkStreamEncodedPayload> {
    const pending = this.pendingRequests.create(name, timeoutMs);
    try {
      await this.sendEncoded(
        ZlinkStreamMessageKind.Request,
        name,
        payload,
        metadata,
        compress,
        pending.requestSeq,
        signal,
        flow,
        this.nextCorrelationId()
      );
      return await pending.promise;
    } catch (error) {
      this.pendingRequests.cancel(pending.requestSeq);
      throw error;
    }
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
