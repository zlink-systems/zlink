import type {
  RequiredZlinkStreamConnectorOptions,
  ZlinkStreamConnection,
  ZlinkStreamError
} from '../Contracts';
import {
  ZlinkStreamConnectionState,
  ZlinkStreamDispatchMode,
  ZlinkStreamErrorCode,
  ZlinkStreamException
} from '../Contracts';
import type { ZlinkStreamCloseReason } from '../Contracts';
import { ZLINK_STREAM_HEARTBEAT_PING } from './Protocol/ZlinkStreamFrameProtocol';
import type { ZlinkStreamConnectorEvents } from './ZlinkStreamConnectorEvents';
import type { ZlinkStreamFrameSender } from './ZlinkStreamFrameSender';
import type { ZlinkStreamPendingRequests } from './ZlinkStreamPendingRequests';
import type { ZlinkStreamReceiveDispatcher } from './ZlinkStreamReceiveDispatcher';
import type { ZlinkStreamReceivedMessages } from './ZlinkStreamReceivedMessages';
import { connectorError, delay, throwIfAborted, toStreamError } from './ZlinkStreamSupport';
import type { ZlinkStreamRuntimeMetrics } from './ZlinkStreamRuntimeMetrics';

export class ZlinkStreamConnectorLifecycle {
  private receiveLoopAbort: AbortController | undefined;
  private receiveLoopSleeping = false;
  private receiveLoopWake: (() => void) | undefined;
  private readonly receiveLoopSettled: Array<() => void> = [];
  private currentConnection: ZlinkStreamConnection | undefined;
  private connectionGeneration = 0;
  private currentState = ZlinkStreamConnectionState.Created;
  private heartbeatTimer: ReturnType<typeof setTimeout> | undefined;
  private lastInboundAt = 0;
  private closeTask: Promise<void> | undefined;
  private connectTask: Promise<void> | undefined;
  private disconnectTask: Promise<void> | undefined;
  private closeRequested = false;
  private disconnectedPublished = false;
  private closeReasonValue?: ZlinkStreamCloseReason;
  private lateConnectCleanupError: unknown;

  constructor(
    private readonly options: RequiredZlinkStreamConnectorOptions,
    private readonly pendingRequests: ZlinkStreamPendingRequests,
    private readonly frameSender: ZlinkStreamFrameSender,
    private readonly receiveDispatcher: ZlinkStreamReceiveDispatcher,
    private readonly receivedMessages: ZlinkStreamReceivedMessages,
    private readonly events: ZlinkStreamConnectorEvents,
    private readonly metrics: ZlinkStreamRuntimeMetrics
  ) {}

  get isConnected(): boolean {
    return this.currentState === ZlinkStreamConnectionState.Connected;
  }

  get state(): ZlinkStreamConnectionState {
    return this.currentState;
  }

  get closeReason(): ZlinkStreamCloseReason | undefined {
    return this.closeReasonValue;
  }

  async connect(signal?: AbortSignal): Promise<void> {
    throwIfAborted(signal);
    await this.disconnectTask?.catch(() => undefined);
    if (this.closeRequested || this.currentState === ZlinkStreamConnectionState.Closed) {
      throw connectorError(ZlinkStreamErrorCode.Disconnected, 'Connector is closed.');
    }
    if (this.currentState === ZlinkStreamConnectionState.Connected) {
      return;
    }
    if (this.connectTask !== undefined) {
      return await this.connectTask;
    }
    this.connectTask = this.connectOnce(signal).finally(() => { this.connectTask = undefined; });
    return await this.connectTask;
  }

  private async connectOnce(signal?: AbortSignal): Promise<void> {
    await this.setState(ZlinkStreamConnectionState.Connecting, undefined, signal);
    try {
      const connection = await this.connectWithReconnect(signal);
      if (this.closeRequested) {
        try {
          await connection.close(signal);
        } catch (error) {
          this.lateConnectCleanupError = error;
          throw error;
        }
        throw connectorError(ZlinkStreamErrorCode.Disconnected, 'Connector closed while connecting.');
      }
      this.currentConnection = connection;
      this.connectionGeneration += 1;
      this.disconnectedPublished = false;
      this.lastInboundAt = Date.now();
      await this.setState(ZlinkStreamConnectionState.Connected, undefined, signal);
      this.startHeartbeat();
      this.startReceiveLoop();
    } catch (cause) {
      if (this.closeRequested) {
        const message = cause instanceof Error ? cause.message : 'Connector closed while connecting.';
        const error = toStreamError(cause, ZlinkStreamErrorCode.Disconnected, message);
        throw new ZlinkStreamException(error);
      }
      const error = toStreamError(cause, ZlinkStreamErrorCode.ConnectTimeout, 'Connect failed.');
      await this.setState(ZlinkStreamConnectionState.Disconnected, error, signal);
      throw new ZlinkStreamException(error);
    }
  }

  async close(signal?: AbortSignal): Promise<void> {
    this.closeReasonValue = 'ClientClose';
    this.closeRequested = true;
    if (this.closeTask !== undefined) {
      return await this.closeTask;
    }
    if (this.currentState === ZlinkStreamConnectionState.Closed) {
      return;
    }
    this.closeTask = this.closeOnce(signal).finally(() => { this.closeTask = undefined; });
    return await this.closeTask;
  }

  async serverClosing(reason: ZlinkStreamCloseReason): Promise<void> {
    this.closeReasonValue = reason;
    const error = { code: ZlinkStreamErrorCode.Disconnected, message: `Server closed the session: ${reason}.` };
    await this.disconnectForTransportFailure(error, this.currentConnection, this.connectionGeneration);
  }

  private async closeOnce(signal?: AbortSignal): Promise<void> {
    await this.connectTask?.catch(() => undefined);
    await this.disconnectTask?.catch(() => undefined);
    const connection = this.currentConnection;
    this.stopHeartbeat();
    this.stopReceiveLoop();
    this.currentConnection = undefined;
    const errors: unknown[] = [];
    if (this.lateConnectCleanupError !== undefined) {
      errors.push(this.lateConnectCleanupError);
      this.lateConnectCleanupError = undefined;
    }
    try {
      await this.frameSender.drain(signal);
    } catch (error) {
      errors.push(error);
    }
    try {
      await connection?.close(signal);
    } catch (error) {
      errors.push(error);
    }
    this.pendingRequests.failAll({ code: ZlinkStreamErrorCode.Disconnected, message: 'Connector closed.' });
    await this.setState(ZlinkStreamConnectionState.Closed, undefined, signal);
    await this.publishDisconnectedOnce(signal);
    if (errors.length === 1) throw errors[0];
    if (errors.length > 1) throw new AggregateError(errors, 'Stream connector close failed.');
  }

  /**
   * Spec stream-connector 32 §7: `dispatch` runs the callbacks the receive loop
   * queued, it does not drive the transport. Receiving is the receive loop's
   * job in both dispatch modes, which is what lets a `Manual` consumer complete
   * a `waitFor` without pumping, and what keeps this call from blocking on an
   * idle connection. In `Manual` it first lets the loop settle whatever has
   * already arrived, so a packet the transport is holding is delivered by this
   * pump rather than the next one.
   */
  async dispatch(signal?: AbortSignal): Promise<void> {
    throwIfAborted(signal);
    if (this.options.dispatchMode !== ZlinkStreamDispatchMode.Immediate) {
      await this.settleReceiveLoop();
    }
    await this.receivedMessages.pump();
  }

  connectionForSend(): ZlinkStreamConnection {
    if (this.currentConnection === undefined || this.currentState !== ZlinkStreamConnectionState.Connected) {
      throw connectorError(ZlinkStreamErrorCode.Disconnected, 'Connector is not connected.');
    }
    return this.currentConnection;
  }

  private async dispatchAvailable(
    connection: ZlinkStreamConnection | undefined,
    generation: number,
    signal?: AbortSignal
  ): Promise<boolean> {
    throwIfAborted(signal);
    const result = await this.receiveDispatcher.readAndDispatch(
      connection,
      signal,
      () => this.isCurrentConnection(connection, generation)
    );
    if (result.inbound && this.isCurrentConnection(connection, generation)) {
      this.lastInboundAt = Date.now();
    }
    return result.available;
  }

  private async connectWithReconnect(signal?: AbortSignal): Promise<ZlinkStreamConnection> {
    let attempt = 0;
    let delayMs = this.options.reconnect.initialDelayMs;
    let lastError: ZlinkStreamError | undefined;
    const maxAttempts = this.options.reconnect.enabled ? this.options.reconnect.maxAttempts : 1;

    while (attempt < maxAttempts) {
      attempt += 1;
      if (attempt > 1) {
        this.metrics.reconnect();
      }
      const handshakeStartedAt = performance.now();
      try {
        const connection = await this.options.transportFactory.connect(this.options, signal);
        this.metrics.handshakeCompleted(handshakeStartedAt);
        return connection;
      } catch (cause) {
        this.metrics.handshakeCompleted(handshakeStartedAt);
        this.metrics.handshakeFailed(cause);
        lastError = toStreamError(cause, ZlinkStreamErrorCode.ConnectTimeout, 'Connect failed.');
        if (!this.options.reconnect.enabled || attempt >= maxAttempts) {
          break;
        }
        await this.setState(ZlinkStreamConnectionState.Reconnecting, lastError, signal);
        await delay(delayMs, signal);
        delayMs = Math.min(
          this.options.reconnect.maxDelayMs,
          Math.ceil(delayMs * this.options.reconnect.backoffFactor)
        );
      }
    }
    throw new ZlinkStreamException(
      lastError ?? { code: ZlinkStreamErrorCode.ConnectTimeout, message: 'Connect failed.' }
    );
  }

  private startHeartbeat(): void {
    this.stopHeartbeat();
    if (!this.options.heartbeat.enabled) {
      return;
    }
    this.heartbeatTimer = setInterval(() => {
      void this.runHeartbeatTick();
    }, this.options.heartbeat.intervalMs);
  }

  private stopHeartbeat(): void {
    if (this.heartbeatTimer !== undefined) {
      clearInterval(this.heartbeatTimer);
      this.heartbeatTimer = undefined;
    }
  }

  // Spec stream-connector 32 §7: the receive loop runs in both dispatch modes.
  // `Manual` only changes what the loop does with a frame — it queues the
  // registered callbacks instead of running them — never whether frames are
  // read off the transport.
  private startReceiveLoop(): void {
    if (this.currentConnection?.read === undefined) {
      return;
    }
    this.stopReceiveLoop();
    const abort = new AbortController();
    const connection = this.currentConnection;
    const generation = this.connectionGeneration;
    this.receiveLoopAbort = abort;
    void this.runReceiveLoop(connection, generation, abort.signal);
  }

  private stopReceiveLoop(): void {
    this.receiveLoopAbort?.abort();
    this.receiveLoopAbort = undefined;
    this.receiveLoopSleeping = false;
    this.releaseReceiveLoopSettled();
  }

  private async runReceiveLoop(
    connection: ZlinkStreamConnection | undefined,
    generation: number,
    signal: AbortSignal
  ): Promise<void> {
    try {
      while (this.shouldContinueReceiveLoop(connection, generation, signal)) {
        const dispatched = await this.dispatchAvailable(connection, generation, signal);
        if (!dispatched && this.shouldContinueReceiveLoop(connection, generation, signal)) {
          await this.sleepUntilWork(signal);
        }
      }
    } catch (cause) {
      if (signal.aborted) return;
      const error = toStreamError(cause, ZlinkStreamErrorCode.FrameDecodeFailed, 'Receive loop failed.');
      await this.disconnectForTransportFailure(error, connection, generation);
    } finally {
      this.receiveLoopSleeping = false;
      this.releaseReceiveLoopSettled();
    }
  }

  // A transport whose read resolves only when a frame arrives parks the loop
  // inside that read; one that reports "nothing available" instead parks it
  // here. Both are the loop waiting for new data, and `dispatch` treats them
  // the same way.
  private async sleepUntilWork(signal: AbortSignal): Promise<void> {
    this.receiveLoopSleeping = true;
    this.releaseReceiveLoopSettled();
    try {
      await new Promise<void>((resolve) => {
        const finish = (): void => {
          clearTimeout(timer);
          signal.removeEventListener('abort', onAbort);
          this.receiveLoopWake = undefined;
          resolve();
        };
        const onAbort = (): void => finish();
        const timer = setTimeout(finish, 1);
        signal.addEventListener('abort', onAbort, { once: true });
        this.receiveLoopWake = finish;
      });
    } finally {
      this.receiveLoopSleeping = false;
    }
  }

  // Returns once the loop has consumed everything the transport already had.
  // A loop that is mid-batch, or parked inside a read that has not produced a
  // frame, is already caught up, so only a sleeping loop is woken and awaited.
  private async settleReceiveLoop(): Promise<void> {
    if (this.receiveLoopAbort === undefined || !this.receiveLoopSleeping) {
      return;
    }
    const settled = new Promise<void>((resolve) => { this.receiveLoopSettled.push(resolve); });
    this.receiveLoopWake?.();
    await settled;
  }

  private releaseReceiveLoopSettled(): void {
    for (const resolve of this.receiveLoopSettled.splice(0)) {
      resolve();
    }
  }

  private shouldContinueReceiveLoop(
    connection: ZlinkStreamConnection | undefined,
    generation: number,
    signal: AbortSignal
  ): boolean {
    return !signal.aborted &&
      this.currentState === ZlinkStreamConnectionState.Connected &&
      this.isCurrentConnection(connection, generation);
  }

  private async runHeartbeatTick(): Promise<void> {
    if (!this.isConnected) {
      return;
    }
    if (Date.now() - this.lastInboundAt > this.options.heartbeat.timeoutMs) {
      this.closeReasonValue = 'HeartbeatTimeout';
      const error = { code: ZlinkStreamErrorCode.Disconnected, message: 'Heartbeat timed out.' };
      await this.disconnectForTransportFailure(error, this.currentConnection, this.connectionGeneration);
      return;
    }
    const connection = this.currentConnection;
    const generation = this.connectionGeneration;
    try {
      await this.frameSender.sendControl(this.connectionForSend(), ZLINK_STREAM_HEARTBEAT_PING);
    } catch (cause) {
      const error = toStreamError(cause, ZlinkStreamErrorCode.SendFailed, 'Heartbeat send failed.');
      await this.disconnectForTransportFailure(error, connection, generation);
    }
  }

  private async disconnectForTransportFailure(
    error: ZlinkStreamError,
    origin: ZlinkStreamConnection | undefined,
    generation: number
  ): Promise<void> {
    this.closeReasonValue ??= 'TransportError';
    if (this.closeRequested || this.currentState === ZlinkStreamConnectionState.Closed) {
      return;
    }
    if (origin !== undefined && !this.isCurrentConnection(origin, generation)) {
      return;
    }
    if (this.disconnectTask !== undefined) {
      return await this.disconnectTask;
    }
    this.disconnectTask = this.disconnectOnce(error).finally(() => { this.disconnectTask = undefined; });
    return await this.disconnectTask;
  }

  private isCurrentConnection(connection: ZlinkStreamConnection | undefined, generation: number): boolean {
    return !this.closeRequested &&
      this.currentConnection === connection &&
      this.connectionGeneration === generation;
  }

  private async disconnectOnce(error: ZlinkStreamError): Promise<void> {
    this.stopHeartbeat();
    this.stopReceiveLoop();
    const connection = this.currentConnection;
    this.currentConnection = undefined;
    this.pendingRequests.failAll(error);
    try {
      await connection?.close();
    } catch {
      // The original transport failure remains the connector-visible error.
    }
    if (this.closeRequested) return;
    await this.setState(ZlinkStreamConnectionState.Disconnected, error);
    await this.publishDisconnectedOnce();
    if (this.shouldReconnect()) {
      queueMicrotask(() => { void this.connect().catch(() => undefined); });
    }
  }

  private shouldReconnect(): boolean {
    return this.options.reconnect.enabled && !this.closeRequested;
  }

  private async publishDisconnectedOnce(signal?: AbortSignal): Promise<void> {
    if (this.disconnectedPublished) return;
    this.disconnectedPublished = true;
    await this.events.publishDisconnected(signal);
  }

  private async setState(
    current: ZlinkStreamConnectionState,
    error: ZlinkStreamError | undefined,
    signal?: AbortSignal
  ): Promise<void> {
    const previous = this.currentState;
    this.currentState = current;
    if (previous === current && error === undefined) {
      return;
    }
    await this.events.publishStateChanged({ previous, current, error }, signal);
    if (error !== undefined) {
      await this.events.publishError(error, signal);
    }
  }
}
