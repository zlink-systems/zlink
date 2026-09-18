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

/**
 * Spec stream-connector 32 §6: the wait between attempts is a value picked in
 * [50%, 100%] of the base delay. Deterministic delays make every client that
 * was connected to a server reconnect at the same instant, which is the moment
 * the server is least able to take them.
 */
function randomizedDelay(baseDelayMs: number): number {
  return Math.round(baseDelayMs * (0.5 + Math.random() * 0.5));
}

export class ZlinkStreamConnectorLifecycle {
  private receiveLoopAbort: AbortController | undefined;
  private receiveLoopSleeping = false;
  private receiveLoopWake: (() => void) | undefined;
  private readonly receiveLoopSettled: Array<() => void> = [];
  private currentConnection: ZlinkStreamConnection | undefined;
  private connectionGeneration = 0;
  private currentState = ZlinkStreamConnectionState.Created;
  private heartbeatTimer: ReturnType<typeof setInterval> | undefined;
  private heartbeatTickRunning = false;
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
    private readonly events: ZlinkStreamConnectorEvents
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
      // Spec stream-connector 32 §10: the baseline for `receivedCount` is the
      // moment a connection is established, so each new connection counts from 0.
      this.receivedMessages.resetReceivedCounts();
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
      // Spec stream-connector 32 §6.2 and the §9 impact table: `ConnectTimeout`
      // ends as `TransportError`, and the reason is recorded even when no
      // connection was ever established — the TypeScript disconnect handler
      // takes no argument and reads `closeReason`, so it has to be set before
      // the handler runs.
      this.closeReasonValue ??= 'TransportError';
      await this.setState(ZlinkStreamConnectionState.Disconnected, error, signal);
      // Spec stream-connector 32 §6: once the attempts are spent the state is
      // `Disconnected` and the registered disconnect handler runs. A caller
      // that only subscribed to that handler learns about the failure here,
      // not only through the rejected `connect`.
      await this.publishDisconnectedOnce(signal);
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
      () => this.isCurrentConnection(connection, generation),
      () => this.connectionForSend()
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
    // Spec stream-connector 32 §6: `null` attempts means unlimited, so the loop
    // has no upper bound and only a successful connect or a close leaves it.
    const maxAttempts = this.options.reconnect.enabled ? this.options.reconnect.maxAttempts : 1;
    const unlimited = maxAttempts === null;

    while (unlimited || attempt < maxAttempts) {
      attempt += 1;
      try {
        return await this.options.transportFactory.connect(this.options, signal);
      } catch (cause) {
        lastError = toStreamError(cause, ZlinkStreamErrorCode.ConnectTimeout, 'Connect failed.');
        if (!this.options.reconnect.enabled || (!unlimited && attempt >= maxAttempts)) {
          break;
        }
        await this.setState(ZlinkStreamConnectionState.Reconnecting, lastError, signal);
        await delay(randomizedDelay(delayMs), signal);
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
    // The interval fires on the clock, not on the previous tick. A transport
    // whose write really awaits would let the next tick start on top of an
    // unfinished one and put two pings on the wire out of step with the
    // interval. A tick already in flight is the ping for this period, so the
    // one the clock just asked for is dropped. The timer identity in the
    // release guards a tick left over from a previous connection from clearing
    // the flag a newer heartbeat holds.
    const timer = setInterval(() => {
      if (this.heartbeatTickRunning) {
        return;
      }
      this.heartbeatTickRunning = true;
      void this.runHeartbeatTick().finally(() => {
        if (this.heartbeatTimer === timer) {
          this.heartbeatTickRunning = false;
        }
      });
    }, this.options.heartbeat.intervalMs);
    this.heartbeatTimer = timer;
  }

  private stopHeartbeat(): void {
    if (this.heartbeatTimer !== undefined) {
      clearInterval(this.heartbeatTimer);
      this.heartbeatTimer = undefined;
    }
    this.heartbeatTickRunning = false;
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
    // `disconnectTask` covers the transport teardown and nothing else, because
    // `connect` waits on it. Everything application code can hold open — the
    // state handler, the disconnect handler — stays outside it: a disconnect
    // handler that calls `connect` would otherwise wait for the task its own
    // caller has not yet left, and one whose promise never settles would keep
    // the `Promise.allSettled` in `publishDisconnected` from ever returning.
    this.disconnectTask = this.tearDownConnection(error)
      .finally(() => { this.disconnectTask = undefined; });
    await this.disconnectTask;
    await this.announceDisconnect(error);
  }

  private isCurrentConnection(connection: ZlinkStreamConnection | undefined, generation: number): boolean {
    return !this.closeRequested &&
      this.currentConnection === connection &&
      this.connectionGeneration === generation;
  }

  /** Transport teardown only — no application callback runs from here. */
  private async tearDownConnection(error: ZlinkStreamError): Promise<void> {
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
  }

  /**
   * Runs once the teardown promise has settled, so a handler reached from here
   * may call `connect` without waiting for a task its own caller still holds.
   * The reconnect is queued before the notification is awaited for the same
   * reason: spec stream-connector 32 §6 has reconnect on by default, and a
   * handler that is slow — or whose promise never settles at all — must not
   * cost the connector the attempt. The state is already `Disconnected` and the
   * state handlers are already invoked by the time the queued microtask runs,
   * because `setState` records the state and hands the change to the handlers
   * before it awaits any of them.
   */
  private async announceDisconnect(error: ZlinkStreamError): Promise<void> {
    if (this.closeRequested) return;
    const announce = this.claimDisconnectedPublish();
    const notified = (async (): Promise<void> => {
      await this.setState(ZlinkStreamConnectionState.Disconnected, error);
      if (announce) {
        await this.events.publishDisconnected();
      }
    })();
    if (this.shouldReconnect()) {
      queueMicrotask(() => { void this.connect().catch(() => undefined); });
    }
    await notified;
  }

  private shouldReconnect(): boolean {
    return this.options.reconnect.enabled && !this.closeRequested;
  }

  // Synchronous test-and-set, taken before any await, so the single disconnect
  // notification the spec promises is claimed by exactly one caller even when
  // the publishing itself is deferred.
  private claimDisconnectedPublish(): boolean {
    if (this.disconnectedPublished) return false;
    this.disconnectedPublished = true;
    return true;
  }

  private async publishDisconnectedOnce(signal?: AbortSignal): Promise<void> {
    if (!this.claimDisconnectedPublish()) return;
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
