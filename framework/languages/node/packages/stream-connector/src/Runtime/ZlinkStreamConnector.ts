import {
  Disposable,
  RequiredZlinkStreamConnectorOptions,
  ZlinkStreamCodec,
  ZlinkStreamConnectionState,
  ZlinkStreamConnectionStateChanged,
  ZlinkStreamConnector,
  ZlinkStreamConnectorOptions,
  ZlinkStreamEncodedPayload,
  ZlinkStreamError,
  ZlinkStreamErrorCode,
  ZlinkStreamExpectNoneCall,
  ZlinkStreamFlow,
  ZlinkStreamInboundObservation,
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
import { connectorError, throwIfAborted } from './ZlinkStreamSupport';
import { ZlinkStreamPendingRequests } from './ZlinkStreamPendingRequests';
import { ZlinkStreamReceivedMessages } from './ZlinkStreamReceivedMessages';
import { ZlinkStreamInboundObservers } from './ZlinkStreamInboundObservers';
import { ZlinkStreamFrameSender } from './ZlinkStreamFrameSender';
import { ZlinkStreamReceiveDispatcher } from './ZlinkStreamReceiveDispatcher';
import { ZlinkStreamConnectorLifecycle } from './ZlinkStreamConnectorLifecycle';
import { ZlinkStreamConnectorEvents } from './ZlinkStreamConnectorEvents';
import { BrowserZlinkFlowContext, type ZlinkFlowContext } from './ZlinkFlowContext';
import { BrowserStreamTransportFactory } from './Transport/BrowserWebSocketConnection';
import { ZlinkStreamRuntimeMetrics } from './ZlinkStreamRuntimeMetrics';

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
  private readonly inboundObservers: ZlinkStreamInboundObservers;

  readonly options: RequiredZlinkStreamConnectorOptions;

  constructor(options: ZlinkStreamConnectorOptions) {
    const flowContext: ZlinkFlowContext = new BrowserZlinkFlowContext();
    this.options = normalizeOptions(options, new BrowserStreamTransportFactory());
    const metrics = new ZlinkStreamRuntimeMetrics(this.options);
    const protocol = new ZlinkStreamFrameProtocol(this.options);
    this.frameSender = new ZlinkStreamFrameSender(protocol, flowContext, metrics);
    this.inboundObservers = new ZlinkStreamInboundObservers(
      this.options.maxInboundObserverNotifications,
      this.options.maxInboundObserverPayloadPreviewBytes,
      this.events
    );
    this.receivedMessages = new ZlinkStreamReceivedMessages(
      this.options.maxReceivedMessages,
      this.events
    );
    this.receiveDispatcher = new ZlinkStreamReceiveDispatcher(
      protocol,
      this.pendingRequests,
      this.inboundObservers,
      this.receivedMessages,
      this.frameSender,
      this.events,
      flowContext,
      metrics,
      (reason) => this.lifecycle.serverClosing(reason)
    );
    this.lifecycle = new ZlinkStreamConnectorLifecycle(
      this.options,
      this.pendingRequests,
      this.frameSender,
      this.receiveDispatcher,
      this.events,
      metrics
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

  observeInbound(
    observer: (observation: ZlinkStreamInboundObservation, signal?: AbortSignal) => Promise<void> | void
  ): Disposable {
    if (this.lifecycle.state !== ZlinkStreamConnectionState.Created) {
      throw connectorError(ZlinkStreamErrorCode.ValidationFailed, 'Inbound observers must be registered before connecting.');
    }
    return this.inboundObservers.add(observer);
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

  waitFor<TPayload = ZlinkStreamEncodedPayload>(name: string): ZlinkStreamWaitCall<TPayload> {
    validateName(name);
    return new ZlinkStreamWaitBuilder<TPayload>(this, name);
  }

  expectNone<TPayload = ZlinkStreamEncodedPayload>(name: string): ZlinkStreamExpectNoneCall<TPayload> {
    validateName(name);
    return new ZlinkStreamExpectNoneBuilder<TPayload>(this, name);
  }

  waitForSequence<TPayload = ZlinkStreamEncodedPayload>(name: string): ZlinkStreamSequenceCall<TPayload> {
    validateName(name);
    return new ZlinkStreamSequenceBuilder<TPayload>(this, name);
  }

  waitForMessage<TPayload>(
    name: string,
    timeoutMs: number,
    predicate: (message: ZlinkStreamMessage<TPayload>) => boolean,
    signal?: AbortSignal
  ): Promise<ZlinkStreamMessage<TPayload>> {
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
          resolve(message!);
        }
      };
      timer = setTimeout(() => {
        finish(connectorError(ZlinkStreamErrorCode.RequestTimeout, 'Wait for stream message timed out.'));
      }, timeoutMs);
      signal?.addEventListener('abort', onAbort, { once: true });
      disposable = this.receivedMessages.on(name, (message) => {
        try {
          const decoded = {
            name: message.name,
            metadata: message.metadata,
            payload: this.decodeWaitPayload<TPayload>(message.payload),
            flowId: message.flowId,
            flowOrigin: message.flowOrigin
          };
          if (predicate(decoded)) {
            finish(undefined, decoded);
          }
        } catch (cause) {
          finish(cause);
        }
      });
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
