import { ZLinkFrameworkInternalErrorKind, createInternalFrameworkException  } from '../framework-errors-internal';
import type {
  ZLinkMessageSerializer,
  RoutingId,
  ZLinkSession,
  ZLinkSessionDispatchContext
} from '../../contracts';
import type { ZLinkProviderResolver } from '../../contracts/Common/ZLinkProviderResolver';
import {
  ZLinkFrameworkErrorKind,
  ZLinkFrameworkException
} from '../../contracts';
import { ZLinkSocketNativeEventType } from '../diagnostics/internal-event-contracts';
import {
  ZLinkRuntimeMessageFlowOutcome as ZLinkMessageFlowOutcome,
  ZLinkRuntimeDispatchErrorAction as ZLinkDispatchErrorAction,
  ZLinkRuntimeDispatchErrorReason as ZLinkDispatchErrorReason,
  ZLinkDispatchErrorSurface,
  ZLinkDispatchMessageKind
} from '../../contracts/Dispatch/ZLinkDispatchOptions';
import type { Message } from '../../contracts/Common/Message';
import { throwIfAborted } from '../abort';
import { ZLinkDispatchErrorReporter, ZLinkRouteDisconnectedError } from '../channels';
import { flowIfEnabled } from '../diagnostics';
import type { ZLinkRuntimeMetrics } from '../diagnostics';
import { wrapFrameworkPayloadMessage } from '../messaging/payload-codec';
import type {
  ZLinkBackendSocketMonitor,
  ZLinkBackendSocketMonitorEvent,
  ZLinkBackendReadablePoller,
  ZLinkBackendStreamSocket
} from '../backend/contracts';
import type { ZLinkMeshCompletionTable } from '../backend/mesh-completion-table';
import type { StreamSessionService } from '../foundation/service-runtime-contracts';
import {
  decodeStreamHeader,
  messageToBytes,
  type ZLinkStreamFrameHeader,
  ZLINK_STREAM_HEARTBEAT_PING,
  ZLINK_STREAM_HEARTBEAT_PONG,
  ZLinkStreamCloseReasonCode,
  ZLinkStreamMessageKind
} from './protocol';
import { createInboundFlow, runWithFlow } from '../diagnostics/flow-context';
import {
  streamSessionIdFromRoutingId,
  ZLinkManagedStream,
  type ZLinkNativeSessionRoute
} from './managed-stream';
import { DefaultZLinkSessionContext } from './session-context';
import { ZLinkStreamSessionSerialExecutor } from './session-serial-executor';
import type { ZLinkApplicationWorkClaim } from '../admission';
import type { ZLinkInboundDispatchBudget } from '../dispatch/inbound-dispatch-budget';
import { ZLinkAsyncSubmitter } from '../messaging';
import { ownedMessage } from './stream-message-utils';
import { ZLinkStreamFrameReassembler } from './stream-frame-reassembler';
import { ZLinkStreamDispatchCapacity } from './stream-dispatch-capacity';

const ZLINK_SEND_DONT_WAIT = 1;
const ZLINK_RECV_DONT_WAIT = 1;
const ZLINK_STREAM_HEARTBEAT_INTERVAL_MS = 1_000;
const ZLINK_STREAM_HEARTBEAT_TIMEOUT_MS = 5_000;
const ZLINK_STREAM_APPLICATION_IDLE_TIMEOUT_MS = 30_000;
const ZLINK_STREAM_RECEIVE_FRAME_BATCH_LIMIT = 64;

interface ZLinkStreamLivenessClock {
  now(): number;
  setTimer(callback: () => void, delayMs: number): unknown;
  clearTimer(timer: unknown): void;
}

interface ZLinkStreamLivenessOptions {
  readonly livenessClock?: ZLinkStreamLivenessClock;
  readonly acceptNewSession?: () => boolean;
  readonly claimApplicationWork?: () => ZLinkApplicationWorkClaim;
}

interface ZLinkStreamReceiveState {
  readonly routingId: unknown;
  readonly session: ZLinkStreamSessionRuntime;
  readonly assembler: ZLinkStreamFrameReassembler;
}

interface ZLinkStreamReceived {
  readonly parts: readonly Message[];
  readonly routingId: unknown;
  close(): void;
}

const systemLivenessClock: ZLinkStreamLivenessClock = {
  now: () => Date.now(),
  setTimer: (callback, delayMs) => setTimeout(callback, delayMs),
  clearTimer: (timer) => clearTimeout(timer as ReturnType<typeof setTimeout>)
};

class ZLinkStreamIdleWaiter {
  private pending?: Promise<void>;
  private resolvePending?: () => void;
  private timer?: ReturnType<typeof setTimeout>;
  private readonly finish = (): void => {
    if (this.timer !== undefined) {
      clearTimeout(this.timer);
      this.timer = undefined;
    }
    const resolve = this.resolvePending;
    this.pending = undefined;
    this.resolvePending = undefined;
    resolve?.();
  };

  constructor(private readonly delayMs: number) {}

  wait(): Promise<void> {
    if (this.pending !== undefined) return this.pending;
    this.pending = new Promise<void>((resolve) => {
      this.resolvePending = resolve;
    });
    this.timer = setTimeout(this.finish, this.delayMs);
    return this.pending;
  }

  wake(): void {
    this.finish();
  }
}

export interface ZLinkStreamSessionContextFactory {
  createSessionContext(
    stream: ZLinkManagedStream,
    close?: (signal?: AbortSignal) => Promise<void>,
    providerResolver?: ZLinkProviderResolver
  ): DefaultZLinkSessionContext;
}

export interface ZLinkStreamSessionRuntimeOptions {
  readonly socket: ZLinkBackendStreamSocket;
  readonly nativeSessionService?: StreamSessionService;
  readonly meshCompletions?: ZLinkMeshCompletionTable;
  readonly nativeSessionRouteForMesh?: (
    meshName: string
  ) => ZLinkNativeSessionRoute | undefined;
  readonly sessionFactory: (context: DefaultZLinkSessionContext) => ZLinkSession | Promise<ZLinkSession>;
  readonly bindingRuntime: ZLinkStreamSessionContextFactory;
  readonly providerResolver?: ZLinkProviderResolver;
  readonly onError?: (error: unknown) => void;
  readonly dispatchErrors?: ZLinkDispatchErrorReporter;
  readonly messageSerializers?: ReadonlyMap<string, ZLinkMessageSerializer>;
  readonly metrics?: ZLinkRuntimeMetrics;
  readonly submitter?: ZLinkAsyncSubmitter;
  readonly inboundDispatchBudget?: ZLinkInboundDispatchBudget;
}

export interface ZLinkStreamSessionNodeRuntimeOptions extends ZLinkStreamSessionRuntimeOptions {
  /** Readiness is checked before each nonblocking Framework recv. */
  readonly readablePoller: ZLinkBackendReadablePoller;
  readonly nodeName?: string;
  readonly monitor?: ZLinkBackendSocketMonitor;
}

function createDispatchContext(header: ZLinkStreamFrameHeader): ZLinkSessionDispatchContext {
  return {
    packetName: header.name,
    metadata: header.metadata,
    canReply: header.requestSeq !== undefined
  };
}

export class ZLinkStreamSessionRuntime {
  readonly stream: ZLinkManagedStream;
  readonly context: DefaultZLinkSessionContext;
  private readonly sessionReady: Promise<ZLinkSession>;
  private readonly serial = new ZLinkStreamSessionSerialExecutor();
  private connected = false;
  private disconnected = false;
  private disposed = false;
  private closeReason = 'client_close';
  private metricsClosed = false;
  private readonly livenessClock: ZLinkStreamLivenessClock;
  private lastApplicationActivityAt = 0;
  private awaitingPongSince: number | undefined;
  private livenessPausedAt: number | undefined;
  private livenessTimer: unknown;
  private removeBudgetPauseListener: (() => void) | undefined;
  private removeBudgetResumeListener: (() => void) | undefined;

  constructor(
    private readonly options: ZLinkStreamSessionRuntimeOptions & ZLinkStreamLivenessOptions,
    routingId: unknown,
    private readonly removeSession: (sessionId: string, session: ZLinkStreamSessionRuntime) => void = () => {},
    private readonly dispatchCapacity?: ZLinkStreamDispatchCapacity
  ) {
    this.livenessClock = options.livenessClock ?? systemLivenessClock;
    this.stream = new ZLinkManagedStream(
      options.socket,
      routingId,
      options.messageSerializers,
      options.nativeSessionService,
      options.meshCompletions,
      options.submitter,
      undefined,
      options.nativeSessionRouteForMesh
    );
    this.context = options.bindingRuntime.createSessionContext(
      this.stream,
      (signal) => this.close(signal),
      options.providerResolver
    );
    const sessionOrPromise = options.sessionFactory(this.context);
    this.sessionReady = isPromiseLike(sessionOrPromise)
      ? sessionOrPromise.then(
        (session) => this.completeSessionCreation(session),
        (error) => {
          this.context.completeConfiguration();
          throw error;
        }
      )
      : Promise.resolve(this.completeSessionCreation(sessionOrPromise));
  }

  get session(): Promise<ZLinkSession> {
    return this.sessionReady;
  }

  get isDisconnected(): boolean {
    return this.disconnected;
  }

  private requireProvidedContext(session: ZLinkSession): ZLinkSession {
    if (session.context !== this.context) {
      throw createInternalFrameworkException(
        ZLinkFrameworkInternalErrorKind.RouteNotConnected,
        'Session must expose the context provided by the stream runtime.'
      );
    }
    return session;
  }

  private completeSessionCreation(session: ZLinkSession): ZLinkSession {
    this.context.completeConfiguration();
    const validatedSession = this.requireProvidedContext(session);
    this.registerBudgetLivenessListeners();
    return validatedSession;
  }

  private registerBudgetLivenessListeners(): void {
    const inboundDispatchBudget = this.options.inboundDispatchBudget;
    if (
      inboundDispatchBudget === undefined
      || this.removeBudgetPauseListener !== undefined
      || this.removeBudgetResumeListener !== undefined
    ) {
      return;
    }
    if (inboundDispatchBudget.receivePaused) {
      this.livenessPausedAt = this.livenessClock.now();
    }
    this.removeBudgetPauseListener = inboundDispatchBudget.onPause(() => {
      this.pauseLiveness();
    });
    this.removeBudgetResumeListener = inboundDispatchBudget.onResume(() => {
      this.resumeLiveness();
    });
  }

  private async requireSession(): Promise<ZLinkSession> {
    const session = await this.sessionReady;
    return session;
  }

  enqueueConnected(localAddr?: string, remoteAddr?: string): void {
    this.enqueue(async () => this.markConnected(localAddr, remoteAddr));
  }

  enqueuePacket(
    payload: Message,
    decodedHeader: ZLinkStreamFrameHeader,
    dispatchClaim?: ZLinkApplicationWorkClaim
  ): void {
    // Control frames are runtime traffic, not application payload, so they do
    // not contribute to the application dispatch budget.
    const payloadBytes = decodedHeader.kind === ZLinkStreamMessageKind.Control
      ? 0n
      : BigInt(payload.size());
    let budgetEnqueued = false;
    try {
      this.options.inboundDispatchBudget?.enqueue(payloadBytes);
      budgetEnqueued = true;
      this.enqueueApplication(
        async () => {
          let started = false;
          try {
            this.options.inboundDispatchBudget?.start(payloadBytes);
            started = true;
            await this.dispatchPacket(payload, decodedHeader);
          } finally {
            if (started) {
              this.options.inboundDispatchBudget?.complete(payloadBytes);
            } else {
              this.options.inboundDispatchBudget?.cancelQueued(payloadBytes);
            }
          }
        },
        (error?: unknown) => {
          payload.close();
          this.options.inboundDispatchBudget?.cancelQueued(payloadBytes);
          dispatchClaim?.close();
          if (error !== undefined) {
            void this.replyDispatchError(decodedHeader, error).catch((replyError) => {
              this.options.onError?.(replyError);
            });
          }
        },
        dispatchClaim,
        Number(payloadBytes > BigInt(Number.MAX_SAFE_INTEGER)
          ? BigInt(Number.MAX_SAFE_INTEGER)
          : payloadBytes)
      );
    } catch (error) {
      if (budgetEnqueued) {
        this.options.inboundDispatchBudget?.cancelQueued(payloadBytes);
      }
      dispatchClaim?.close();
      payload.close();
      throw error;
    }
  }

  enqueueDisconnected(error?: unknown): void {
    if (this.disconnected) {
      return;
    }
    this.disconnected = true;
    this.stopLivenessChecks();
    this.enqueue(async () => this.complete(error, true));
  }

  async close(signal?: AbortSignal): Promise<void> {
    throwIfAborted(signal);
    await this.stream.close(signal);
    if (this.disconnected) {
      return;
    }
    this.disconnected = true;
    this.stopLivenessChecks();
    this.enqueue(async () => this.complete(undefined, true));
  }

  async dispose(): Promise<void> {
    if (this.disposed) {
      return;
    }
    this.disposed = true;
    this.stopLivenessChecks();
    await this.serial.dispose();
    if (!this.disconnected) {
      this.disconnected = true;
      const session = await this.requireSession();
      await session.onDisconnected?.(this.context);
    }
    await this.cleanup();
  }

  async drainClose(): Promise<void> {
    this.closeReason = 'server_drain';
    await this.serial.run(async () => {
      if (this.disposed) return;
      this.stopLivenessChecks();
      await this.stream.closeForDrain();
      if (!this.disconnected) {
        this.disconnected = true;
        await this.complete(undefined, true);
      }
    });
  }

  private async markConnected(localAddr?: string, remoteAddr?: string): Promise<void> {
    this.stream.updateAddresses(localAddr, remoteAddr);
    if (this.connected) {
      return;
    }
    this.connected = true;
    this.lastApplicationActivityAt = this.livenessClock.now();
    this.scheduleLivenessCheck();
    this.options.metrics?.change('zlink.stream.connections.active', 1, { transport: 'tcp' });
    this.options.metrics?.count('zlink.stream.connections.opened', 1, { transport: 'tcp' });
    const session = await this.requireSession();
    await session.onConnected?.(this.context);
  }

  private async dispatchPacket(
    payload: Message,
    decodedHeader: ZLinkStreamFrameHeader
  ): Promise<void> {
    let dispatchPayload = payload;
    let enteredDispatch = false;
    try {
      if (decodedHeader.kind === ZLinkStreamMessageKind.Control) {
        await this.handleControl(decodedHeader, payload);
        return;
      }
      this.lastApplicationActivityAt = this.livenessClock.now();
      dispatchPayload = this.context.payloadForHeader(decodedHeader, payload);
      if (this.context.tryCompleteResponse(decodedHeader, dispatchPayload)) {
        return;
      }
      this.context.enterDispatch(decodedHeader);
      enteredDispatch = true;
      const session = await this.requireSession();
      const streamKind = decodedHeader.kind === ZLinkStreamMessageKind.Request
        ? ZLinkDispatchMessageKind.Request
        : ZLinkDispatchMessageKind.Send;
      const streamCorr = decodedHeader.correlationId;
      const inboundHeader = decodedHeader;
      await runWithFlow(createInboundFlow(
        inboundHeader.flowId,
        inboundHeader.flowOrigin,
        this.options.dispatchErrors?.flow.flowCreationEnabled() ?? true
      ), async () => {
        flowIfEnabled(this.options.dispatchErrors?.flow, ZLinkMessageFlowOutcome.Received)?.trace({
          outcome: ZLinkMessageFlowOutcome.Received,
          surface: ZLinkDispatchErrorSurface.StreamSession,
          messageKind: streamKind,
          packetName: inboundHeader.name,
          correlationId: streamCorr,
          sourceRid: this.context.routingId === undefined ? undefined : String(this.context.routingId)
        });
        await session.onDispatch?.(
          createDispatchContext(inboundHeader),
          wrapFrameworkPayloadMessage(dispatchPayload, this.options.messageSerializers)
        );
        flowIfEnabled(this.options.dispatchErrors?.flow, ZLinkMessageFlowOutcome.Dispatched)?.trace({
          outcome: ZLinkMessageFlowOutcome.Dispatched,
          surface: ZLinkDispatchErrorSurface.StreamSession,
          messageKind: streamKind,
          packetName: inboundHeader.name,
          correlationId: streamCorr,
          sourceRid: this.context.routingId === undefined ? undefined : String(this.context.routingId)
        });
      });
    } catch (error) {
      this.options.dispatchErrors?.report({
        surface: ZLinkDispatchErrorSurface.StreamSession,
        messageKind: decodedHeader.kind === ZLinkStreamMessageKind.Request
          ? ZLinkDispatchMessageKind.Request
          : ZLinkDispatchMessageKind.Send,
        reason: ZLinkDispatchErrorReason.HandlerException,
        action: decodedHeader.requestSeq === undefined
          ? ZLinkDispatchErrorAction.Drop
          : ZLinkDispatchErrorAction.ReplyError,
        packetName: decodedHeader.name,
        sourceRid: this.context.routingId === undefined ? undefined : String(this.context.routingId),
        correlationId: decodedHeader.correlationId,
        flowId: decodedHeader.flowId,
        flowOrigin: decodedHeader.flowOrigin,
        error
      });
      this.options.onError?.(error);
      await this.replyDispatchError(decodedHeader, error);
      if (error instanceof ZLinkRouteDisconnectedError && decodedHeader.requestSeq === undefined) {
        await this.context.close();
      }
    } finally {
      if (enteredDispatch) {
        this.context.exitDispatch();
      }
      if (dispatchPayload !== payload) {
        dispatchPayload.close();
      }
      payload.close();
    }
  }

  private async handleControl(header: ZLinkStreamFrameHeader, payload: Message): Promise<void> {
    if (messageToBytes(payload).length !== 0) {
      await this.closeForLiveness(
        ZLinkStreamCloseReasonCode.ProtocolError,
        'protocol_error',
        'Control heartbeat payload must be empty.'
      );
      return;
    }
    if (header.name === ZLINK_STREAM_HEARTBEAT_PONG) {
      this.awaitingPongSince = undefined;
      return;
    }
    if (header.name === ZLINK_STREAM_HEARTBEAT_PING) {
      if (!this.stream.writeControl(ZLINK_STREAM_HEARTBEAT_PONG)) {
        await this.closeForLiveness(
          ZLinkStreamCloseReasonCode.TransportError,
          'transport_error',
          'Heartbeat pong send failed.'
        );
      }
      return;
    }
    await this.closeForLiveness(
      ZLinkStreamCloseReasonCode.ProtocolError,
      'protocol_error',
      `Unknown stream control frame: ${header.name}`
    );
  }

  private scheduleLivenessCheck(): void {
    if (this.disposed || this.disconnected || this.livenessTimer !== undefined) {
      return;
    }
    this.livenessTimer = this.livenessClock.setTimer(() => {
      this.livenessTimer = undefined;
      this.enqueue(async () => this.runLivenessCheck());
    }, ZLINK_STREAM_HEARTBEAT_INTERVAL_MS);
  }

  private async runLivenessCheck(): Promise<void> {
    if (this.disposed || this.disconnected || !this.connected) {
      return;
    }
    const now = this.livenessClock.now();
    if (this.isLivenessPaused(now)) {
      this.scheduleLivenessCheck();
      return;
    }
    if (now - this.lastApplicationActivityAt >= ZLINK_STREAM_APPLICATION_IDLE_TIMEOUT_MS) {
      await this.closeForLiveness(
        ZLinkStreamCloseReasonCode.IdleTimeout,
        'idle_timeout',
        'Application idle timeout.'
      );
      return;
    }
    if (
      this.awaitingPongSince !== undefined
      && now - this.awaitingPongSince >= ZLINK_STREAM_HEARTBEAT_TIMEOUT_MS
    ) {
      await this.closeForLiveness(
        ZLinkStreamCloseReasonCode.HeartbeatTimeout,
        'heartbeat_timeout',
        'Heartbeat timeout.'
      );
      return;
    }
    if (!this.stream.writeControl(ZLINK_STREAM_HEARTBEAT_PING)) {
      await this.closeForLiveness(
        ZLinkStreamCloseReasonCode.TransportError,
        'transport_error',
        'Heartbeat ping send failed.'
      );
      return;
    }
    this.awaitingPongSince ??= now;
    this.scheduleLivenessCheck();
  }

  private async closeForLiveness(
    reason: ZLinkStreamCloseReasonCode,
    closeReason: string,
    diagnostic: string
  ): Promise<void> {
    if (this.disconnected) {
      return;
    }
    this.disconnected = true;
    this.closeReason = closeReason;
    this.stopLivenessChecks();
    await this.stream.closeForReason(reason, diagnostic);
    await this.complete(undefined, true);
  }

  private stopLivenessChecks(): void {
    if (this.livenessTimer !== undefined) {
      this.livenessClock.clearTimer(this.livenessTimer);
      this.livenessTimer = undefined;
    }
  }

  private async replyDispatchError(header: ZLinkStreamFrameHeader, error: unknown): Promise<void> {
    if (header.requestSeq === undefined) {
      return;
    }
    const message = this.context.createJsonReplyFrameMessage(
      header,
      ZLinkStreamMessageKind.Error,
      new Map(),
      false,
      {
        code: error instanceof ZLinkFrameworkException
          ? ZLinkFrameworkErrorKind[error.kind]
          : error instanceof Error
            ? error.constructor.name
            : 'RemoteError',
        message: error instanceof Error ? error.message : String(error)
      }
    );
    try {
      if (!this.context.stream.writeRaw(message, ZLINK_SEND_DONT_WAIT)) {
        throw new Error('Client stream error reply send failed.');
      }
      flowIfEnabled(this.options.dispatchErrors?.flow, ZLinkMessageFlowOutcome.Sent)?.trace({
        outcome: ZLinkMessageFlowOutcome.Sent,
        surface: ZLinkDispatchErrorSurface.StreamSession,
        messageKind: ZLinkDispatchMessageKind.Request,
        packetName: header.name,
        correlationId: header.correlationId,
        sourceRid: this.context.routingId === undefined ? undefined : String(this.context.routingId)
      });
    } catch (replyError) {
      this.options.onError?.(replyError);
    } finally {
      message.close();
    }
  }

  private async complete(error: unknown, notifyDisconnected: boolean): Promise<void> {
    if (error !== undefined && this.closeReason !== 'server_drain') {
      this.closeReason = 'transport_error';
    }
    if (error !== undefined) {
      const session = await this.requireSession();
      await session.onError?.(this.context, {
        error: 'transportError' as never,
        message: error instanceof Error ? error.message : String(error)
      });
    }
    if (notifyDisconnected) {
      const session = await this.requireSession();
      await session.onDisconnected?.(this.context);
    }
    await this.cleanup();
  }

  private async cleanup(): Promise<void> {
    this.removeBudgetPauseListener?.();
    this.removeBudgetPauseListener = undefined;
    this.removeBudgetResumeListener?.();
    this.removeBudgetResumeListener = undefined;
    if (this.connected && !this.metricsClosed) {
      this.metricsClosed = true;
      this.options.metrics?.change('zlink.stream.connections.active', -1, { transport: 'tcp' });
      this.options.metrics?.count('zlink.stream.connections.closed', 1, {
        transport: 'tcp',
        close_reason: normalizeStreamCloseReason(this.closeReason)
      });
    }
    await this.context.cleanupBindings();
    this.removeSession(this.stream.sessionId, this);
  }

  private enqueue(work: () => Promise<void>, onRejected?: () => void): void {
    if (!this.serial.enqueue(work, 'lifecycle', {}, onRejected)) {
      onRejected?.();
    }
  }

  private enqueueApplication(
    work: () => Promise<void>,
    onRejected?: (error?: unknown) => void,
    dispatchClaim?: ZLinkApplicationWorkClaim,
    payloadBytes = 0
  ): void {
    let claim: ZLinkApplicationWorkClaim | undefined;
    try {
      claim = this.options.claimApplicationWork?.();
    } catch (error) {
      onRejected?.(error);
      return;
    }
    if (!this.serial.enqueue(async () => {
      try {
        await work();
      } finally {
        claim?.close();
        dispatchClaim?.close();
      }
    }, 'application', { payloadBytes }, () => {
      claim?.close();
      dispatchClaim?.close();
      onRejected?.();
    })) {
      claim?.close();
      dispatchClaim?.close();
      onRejected?.();
    }
  }

  private pauseLiveness(): void {
    this.livenessPausedAt ??= this.livenessClock.now();
  }

  private resumeLiveness(): void {
    const pausedAt = this.livenessPausedAt;
    if (pausedAt === undefined) {
      return;
    }
    const elapsed = Math.max(0, this.livenessClock.now() - pausedAt);
    this.lastApplicationActivityAt += elapsed;
    if (this.awaitingPongSince !== undefined) {
      this.awaitingPongSince += elapsed;
    }
    this.livenessPausedAt = undefined;
  }

  private isLivenessPaused(now: number): boolean {
    if (
      this.options.inboundDispatchBudget?.receivePaused === true
      || this.dispatchCapacity?.receivePaused === true
    ) {
      this.livenessPausedAt ??= now;
      return true;
    }
    this.resumeLiveness();
    return false;
  }
}

export class ZLinkStreamSessionNodeRuntime {
  private readonly sessions = new Map<string, ZLinkStreamSessionRuntime>();
  private readonly receiveStates = new Map<string, ZLinkStreamReceiveState>();
  private readonly readyReceiveStates = new Set<ZLinkStreamReceiveState>();
  private readonly pendingConnectionMetadata: Array<{
    readonly localAddr?: string;
    readonly remoteAddr?: string;
  } | undefined> = [];
  private pendingConnectionMetadataHead = 0;
  private pendingConnectionMetadataCount = 0;
  private readonly unaddressedMonitorSessions: Array<string | undefined> = [];
  private unaddressedMonitorSessionHead = 0;
  private unaddressedMonitorSessionCount = 0;
  private readonly disconnectedEndpoints = new Set<string>();
  private pendingEndpointlessDisconnect:
    | {
        readonly session: ZLinkStreamSessionRuntime;
        cancelled: boolean;
        readonly error: Error;
        readonly activityVersion: number;
      }
    | undefined;
  private activityVersion = 0;
  private stopped = false;
  private readonly submitter: ZLinkAsyncSubmitter;
  private readonly receiveAbortController = new AbortController();
  private receiveLoop: Promise<void> | undefined;
  private readonly receiveIdleWaiter = new ZLinkStreamIdleWaiter(5);
  private receiveWorkSinceYield = 0;
  private readonly maxMessageSize: number;
  private readonly dispatchCapacity: ZLinkStreamDispatchCapacity;

  constructor(
    private readonly options: ZLinkStreamSessionNodeRuntimeOptions & ZLinkStreamLivenessOptions,
    dispatchCapacity = new ZLinkStreamDispatchCapacity()
  ) {
    this.dispatchCapacity = dispatchCapacity;
    const maxMessageSize = options.socket.maxMessageSize;
    this.maxMessageSize = Number.isSafeInteger(maxMessageSize) && maxMessageSize >= 0
      ? maxMessageSize
      : 0;
    const sendTimeoutMs = Number(options.socket.sendTimeoutMs);
    const sendHighWaterMark = Number(options.socket.sendHighWaterMark);
    this.submitter = options.submitter ?? new ZLinkAsyncSubmitter(
      (handler) => options.socket.onSendReady(handler),
      {
        timeoutMs: sendTimeoutMs > 0 ? sendTimeoutMs : 1000,
        capacity: Math.max(1, sendHighWaterMark)
      }
    );
  }

  start(): void {
    if (this.stopped || this.receiveLoop !== undefined) {
      return;
    }
    this.options.monitor?.onEvent((event) => {
      this.onMonitorEvent(event);
    });
    const running = this.runReceiveLoop(this.receiveAbortController.signal)
      .catch((error) => {
        if (!this.stopped) {
          this.options.onError?.(error);
        }
      });
    this.receiveLoop = running;
  }

  markConnected(routingId: unknown, localAddr?: string, remoteAddr?: string): void {
    const session = this.getOrCreateSession(routingId);
    session?.enqueueConnected(localAddr, remoteAddr);
  }

  markDisconnected(routingId: unknown, error?: unknown): void {
    const sessionId = streamSessionIdFromRoutingId(routingId);
    this.removeReceiveState(sessionId);
    this.sessions.get(sessionId)?.enqueueDisconnected(error);
  }

  findSession(routingId: unknown): ZLinkStreamSessionRuntime | undefined {
    return this.sessions.get(streamSessionIdFromRoutingId(routingId));
  }

  async dispose(): Promise<void> {
    this.stopped = true;
    this.receiveAbortController.abort();
    this.wakeReceiveLoop();
    try {
      await this.receiveLoop;
      this.submitter.dispose();
      const sessions = [...this.sessions.values()];
      this.sessions.clear();
      this.receiveStates.clear();
      this.readyReceiveStates.clear();
      this.unaddressedMonitorSessions.length = 0;
      this.unaddressedMonitorSessionHead = 0;
      this.unaddressedMonitorSessionCount = 0;
      this.pendingConnectionMetadata.length = 0;
      this.pendingConnectionMetadataHead = 0;
      this.pendingConnectionMetadataCount = 0;
      for (const session of sessions) {
        await session.dispose();
      }
    } finally {
      this.options.readablePoller.dispose();
    }
  }

  async drainCloseSessions(): Promise<void> {
    await Promise.allSettled([...this.sessions.values()].map((session) => session.drainClose()));
  }

  private async runReceiveLoop(signal: AbortSignal): Promise<void> {
    while (!this.isReceiveStopped(signal)) {
      const receiveResumeWait = this.waitForReceiveResume(signal);
      if (receiveResumeWait !== undefined) {
        await receiveResumeWait;
      }
      if (this.isReceiveStopped(signal)) {
        return;
      }
      let bufferedFrames: { readonly progressed: boolean; readonly frameCount: number };
      try {
        bufferedFrames = this.drainBufferedFrames();
      } catch (error) {
        if (!this.isReceiveStopped(signal)) {
          this.options.onError?.(error);
          this.receiveWorkSinceYield = 0;
          await this.waitReceiveLoopIdle();
        }
        continue;
      }
      if (bufferedFrames.progressed) {
        this.receiveWorkSinceYield += Math.max(1, bufferedFrames.frameCount);
        if (
          this.receiveWorkSinceYield >= ZLINK_STREAM_RECEIVE_FRAME_BATCH_LIMIT
          && !this.isReceiveStopped(signal)
        ) {
          this.receiveWorkSinceYield = 0;
          await new Promise<void>((resolve) => setImmediate(resolve));
        }
        continue;
      }
      if (this.isReceivePaused()) {
        const pausedReceiveWait = this.waitForReceiveResume(signal);
        if (pausedReceiveWait !== undefined) {
          await pausedReceiveWait;
        }
        continue;
      }
      let received: ZLinkStreamReceived | undefined;
      try {
        if (!this.options.readablePoller.wait(0)) {
          await this.waitReceiveLoopIdle();
          continue;
        }
        received = this.options.socket.recv(ZLINK_RECV_DONT_WAIT);
      } catch (error) {
        if (!this.isReceiveStopped(signal)) {
          this.options.onError?.(error);
          this.receiveWorkSinceYield = 0;
          await this.waitReceiveLoopIdle();
        }
        continue;
      }
      if (received === undefined) {
        this.receiveWorkSinceYield = 0;
        await this.waitReceiveLoopIdle();
        continue;
      }
      let receivedFrameCount = 0;
      try {
        receivedFrameCount = this.onReceived(received);
      } catch (error) {
        this.handleReceiveError(
          received.routingId,
          error instanceof Error ? error : new Error(String(error))
        );
      } finally {
        try {
          received.close();
        } catch (error) {
          this.options.onError?.(error);
        }
      }
      this.receiveWorkSinceYield += Math.max(1, receivedFrameCount);
      if (
        this.receiveWorkSinceYield >= ZLINK_STREAM_RECEIVE_FRAME_BATCH_LIMIT
        && !this.isReceiveStopped(signal)
      ) {
        this.receiveWorkSinceYield = 0;
        await new Promise<void>((resolve) => setImmediate(resolve));
      }
    }
  }

  private onReceived(received: ZLinkStreamReceived): number {
    if (this.stopped) {
      return 0;
    }
    const routingId = received.routingId;
    if (routingId === null || routingId === undefined) {
      this.options.onError?.(new Error('STREAM recv did not provide a source routing id.'));
      return 0;
    }
    this.activityVersion += 1;
    const session = this.getOrCreateSession(routingId);
    if (session === undefined) {
      return 0;
    }
    this.applyPendingConnectionMetadata(session);
    let state = this.receiveStates.get(session.stream.sessionId);
    if (state !== undefined && state.session !== session) {
      this.removeReceiveState(session.stream.sessionId);
      state = undefined;
    }
    state ??= {
      routingId,
      session,
      assembler: new ZLinkStreamFrameReassembler()
    };
    this.receiveStates.set(session.stream.sessionId, state);
    try {
      for (const part of received.parts) {
        state.assembler.append(part.data());
      }
    } catch (error) {
      this.handleMalformedFrame(
        state,
        error instanceof Error ? error : new Error(String(error))
      );
      return 0;
    }
    const drained = this.drainReceiveState(state, ZLINK_STREAM_RECEIVE_FRAME_BATCH_LIMIT);
    this.updateReadyState(state, drained.progressed);
    return drained.frameCount;
  }

  private drainBufferedFrames(): { readonly progressed: boolean; readonly frameCount: number } {
    let progressed = false;
    let frameCount = 0;
    while (
      !this.isReceivePaused()
      && frameCount < ZLINK_STREAM_RECEIVE_FRAME_BATCH_LIMIT
    ) {
      const next = this.readyReceiveStates.values().next();
      if (next.done === true) break;
      const state = next.value;
      this.readyReceiveStates.delete(state);
      if (this.receiveStates.get(state.session.stream.sessionId) !== state) continue;
      try {
        const drained = this.drainReceiveState(
          state,
          ZLINK_STREAM_RECEIVE_FRAME_BATCH_LIMIT - frameCount
        );
        frameCount += drained.frameCount;
        progressed = drained.progressed || progressed;
        this.updateReadyState(state, drained.progressed);
      } catch (error) {
        progressed = true;
        this.handleReceiveError(
          state.routingId,
          error instanceof Error ? error : new Error(String(error))
        );
      }
    }
    return { progressed, frameCount };
  }

  private drainReceiveState(
    state: ZLinkStreamReceiveState,
    frameLimit: number
  ): { readonly progressed: boolean; readonly frameCount: number } {
    let progressed = false;
    let frameCount = 0;
    while (!this.isReceivePaused() && frameCount < frameLimit) {
      if (state.session.isDisconnected) {
        this.removeReceiveState(state.session.stream.sessionId);
        return { progressed: true, frameCount };
      }
      const dispatchClaim = this.dispatchCapacity.tryClaim();
      if (dispatchClaim === undefined) {
        return { progressed, frameCount };
      }
      const result = state.assembler.next(this.maxMessageSize);
      if (result.kind === 'incomplete') {
        dispatchClaim.close();
        return { progressed, frameCount };
      }
      if (result.kind === 'malformed') {
        dispatchClaim.close();
        this.handleMalformedFrame(state, result.error);
        return { progressed: true, frameCount };
      }
      let decodedHeader: ZLinkStreamFrameHeader;
      try {
        decodedHeader = decodeStreamHeader(result.frame.header);
      } catch (error) {
        dispatchClaim.close();
        this.handleMalformedFrame(
          state,
          error instanceof Error ? error : new Error(String(error))
        );
        return { progressed: true, frameCount };
      }
      const payload = ownedMessage(result.frame.payload);
      try {
        state.session.enqueuePacket(payload, decodedHeader, dispatchClaim);
      } catch (error) {
        dispatchClaim.close();
        throw error;
      }
      progressed = true;
      frameCount += 1;
    }
    return { progressed, frameCount };
  }

  private updateReadyState(state: ZLinkStreamReceiveState, progressed: boolean): void {
    if (
      this.receiveStates.get(state.session.stream.sessionId) === state
      && state.assembler.hasPendingBytes
      && (progressed || this.isReceivePaused())
    ) {
      this.readyReceiveStates.add(state);
      return;
    }
    this.readyReceiveStates.delete(state);
  }

  private removeReceiveState(sessionId: string): void {
    const state = this.receiveStates.get(sessionId);
    if (state === undefined) return;
    this.receiveStates.delete(sessionId);
    this.readyReceiveStates.delete(state);
    state.assembler.clear();
  }

  private handleMalformedFrame(state: ZLinkStreamReceiveState, error: Error): void {
    this.removeReceiveState(state.session.stream.sessionId);
    try {
      this.options.socket.disconnectPeer(state.routingId as RoutingId);
    } catch (disconnectError) {
      this.options.onError?.(disconnectError);
    }
    state.session.enqueueDisconnected(error);
  }

  private isReceivePaused(): boolean {
    return this.options.inboundDispatchBudget?.receivePaused === true
      || this.dispatchCapacity.receivePaused;
  }

  private waitForReceiveResume(signal: AbortSignal): Promise<void> | undefined {
    const budget = this.options.inboundDispatchBudget;
    const budgetPaused = budget?.receivePaused === true;
    const capacityPaused = this.dispatchCapacity.receivePaused;
    if (!budgetPaused) {
      return capacityPaused
        ? this.dispatchCapacity.waitUntilResumed(signal)
        : undefined;
    }
    const budgetWait = budget!.waitUntilResumed(signal);
    if (!capacityPaused) {
      return budgetWait;
    }
    return Promise.all([
      budgetWait,
      this.dispatchCapacity.waitUntilResumed(signal)
    ]).then(() => undefined);
  }

  private isReceiveStopped(signal: AbortSignal): boolean {
    return this.stopped || signal.aborted;
  }

  private handleReceiveError(routingId: unknown, error: Error): void {
    let sessionId: string;
    try {
      sessionId = streamSessionIdFromRoutingId(routingId);
    } catch {
      this.options.onError?.(error);
      return;
    }
    const state = this.receiveStates.get(sessionId);
    if (state !== undefined) {
      this.handleMalformedFrame(state, error);
      return;
    }
    try {
      this.options.socket.disconnectPeer(routingId as RoutingId);
    } catch (disconnectError) {
      this.options.onError?.(disconnectError);
    }
    this.sessions.get(sessionId)?.enqueueDisconnected(error);
    this.options.onError?.(error);
  }

  private onMonitorEvent(event: ZLinkBackendSocketMonitorEvent): void {
    if (this.stopped) {
      return;
    }
    switch (event.nativeEvent) {
      case ZLinkSocketNativeEventType.ConnectionReady:
        this.activityVersion += 1;
        {
          const endpointKey = streamMonitorEndpointKey(event.localAddr, event.remoteAddr);
          if (this.disconnectedEndpoints.delete(endpointKey)) {
            return;
          }
        }
        if (event.routingId === undefined) {
          const endpointKey = streamMonitorEndpointKey(event.localAddr, event.remoteAddr);
          const unaddressed = this.firstUnaddressedSession();
          if (unaddressed !== undefined) {
            this.disconnectedEndpoints.delete(endpointKey);
            unaddressed.enqueueConnected(event.localAddr, event.remoteAddr);
            this.enqueueUnaddressedMonitorSession(unaddressed.stream.sessionId);
            return;
          }
          if (this.hasPendingConnectionMetadata(endpointKey)) {
            return;
          }
          this.pendingConnectionMetadata.push({
            localAddr: event.localAddr,
            remoteAddr: event.remoteAddr
          });
          this.pendingConnectionMetadataCount += 1;
          return;
        }
        this.getOrCreateSession(event.routingId)?.enqueueConnected(event.localAddr, event.remoteAddr);
        return;
      case ZLinkSocketNativeEventType.Disconnected:
        {
          const endpointKey = streamMonitorEndpointKey(event.localAddr, event.remoteAddr);
          this.disconnectedEndpoints.add(endpointKey);
          this.removePendingConnectionMetadata(endpointKey);
          const session = this.resolveMonitorSession(event);
          const error = new Error(`Stream disconnected: ${event.nativeEvent}/${event.value}`);
          if (session !== undefined) {
            this.removeReceiveState(session.stream.sessionId);
            session.enqueueDisconnected(error);
            return;
          }
          if (
            event.routingId === undefined
            && !streamMonitorHasEndpoint(event)
          ) {
            this.enqueueEndpointlessDisconnect(error);
          }
        }
        return;
      default:
        return;
    }
  }

  private applyPendingConnectionMetadata(session: ZLinkStreamSessionRuntime): void {
    if (
      session.stream.localAddr !== undefined
      || session.stream.remoteAddr !== undefined
      || this.pendingConnectionMetadataCount === 0
    ) {
      return;
    }
    const metadata = this.takePendingConnectionMetadata();
    if (metadata === undefined) return;
    this.removePendingConnectionMetadata(streamMonitorEndpointKey(metadata.localAddr, metadata.remoteAddr));
    session.enqueueConnected(metadata.localAddr, metadata.remoteAddr);
    this.enqueueUnaddressedMonitorSession(session.stream.sessionId);
  }

  private hasPendingConnectionMetadata(endpointKey: string): boolean {
    for (let index = this.pendingConnectionMetadataHead; index < this.pendingConnectionMetadata.length; index += 1) {
      const metadata = this.pendingConnectionMetadata[index];
      if (metadata !== undefined
        && streamMonitorEndpointKey(metadata.localAddr, metadata.remoteAddr) === endpointKey) {
        return true;
      }
    }
    return false;
  }

  private takePendingConnectionMetadata(): {
    readonly localAddr?: string;
    readonly remoteAddr?: string;
  } | undefined {
    while (
      this.pendingConnectionMetadataHead < this.pendingConnectionMetadata.length
      && this.pendingConnectionMetadata[this.pendingConnectionMetadataHead] === undefined
    ) {
      this.pendingConnectionMetadataHead += 1;
    }
    const metadata = this.pendingConnectionMetadata[this.pendingConnectionMetadataHead];
    if (metadata === undefined) return undefined;
    this.pendingConnectionMetadata[this.pendingConnectionMetadataHead] = undefined;
    this.pendingConnectionMetadataHead += 1;
    this.pendingConnectionMetadataCount -= 1;
    this.compactPendingConnectionMetadata();
    return metadata;
  }

  private compactPendingConnectionMetadata(): void {
    if (this.pendingConnectionMetadataCount === 0) {
      this.pendingConnectionMetadata.length = 0;
      this.pendingConnectionMetadataHead = 0;
    } else if (this.pendingConnectionMetadataHead >= 1024
      && this.pendingConnectionMetadataHead * 2 >= this.pendingConnectionMetadata.length) {
      this.pendingConnectionMetadata.splice(0, this.pendingConnectionMetadataHead);
      this.pendingConnectionMetadataHead = 0;
    }
  }

  private enqueueUnaddressedMonitorSession(sessionId: string): void {
    this.unaddressedMonitorSessions.push(sessionId);
    this.unaddressedMonitorSessionCount += 1;
  }

  private takeUnaddressedMonitorSession(): string | undefined {
    while (
      this.unaddressedMonitorSessionHead < this.unaddressedMonitorSessions.length
      && this.unaddressedMonitorSessions[this.unaddressedMonitorSessionHead] === undefined
    ) {
      this.unaddressedMonitorSessionHead += 1;
    }
    const sessionId = this.unaddressedMonitorSessions[this.unaddressedMonitorSessionHead];
    if (sessionId === undefined) return undefined;
    this.unaddressedMonitorSessions[this.unaddressedMonitorSessionHead] = undefined;
    this.unaddressedMonitorSessionHead += 1;
    this.unaddressedMonitorSessionCount -= 1;
    if (this.unaddressedMonitorSessionCount === 0) {
      this.unaddressedMonitorSessions.length = 0;
      this.unaddressedMonitorSessionHead = 0;
    } else if (this.unaddressedMonitorSessionHead >= 1024
      && this.unaddressedMonitorSessionHead * 2 >= this.unaddressedMonitorSessions.length) {
      this.unaddressedMonitorSessions.splice(0, this.unaddressedMonitorSessionHead);
      this.unaddressedMonitorSessionHead = 0;
    }
    return sessionId;
  }

  private firstUnaddressedSession(): ZLinkStreamSessionRuntime | undefined {
    for (const session of this.sessions.values()) {
      if (!session.isDisconnected
        && session.stream.localAddr === undefined
        && session.stream.remoteAddr === undefined) {
        return session;
      }
    }
    return undefined;
  }

  private getActiveSession(sessionId: string): ZLinkStreamSessionRuntime | undefined {
    const session = this.sessions.get(sessionId);
    if (session === undefined || session.isDisconnected) {
      return undefined;
    }
    return session;
  }

  private removePendingConnectionMetadata(endpointKey: string): void {
    for (let index = this.pendingConnectionMetadataHead; index < this.pendingConnectionMetadata.length; index += 1) {
      const metadata = this.pendingConnectionMetadata[index];
      if (metadata === undefined) continue;
      if (streamMonitorEndpointKey(metadata.localAddr, metadata.remoteAddr) === endpointKey) {
        this.pendingConnectionMetadata[index] = undefined;
        this.pendingConnectionMetadataCount -= 1;
      }
    }
    this.compactPendingConnectionMetadata();
  }

  private resolveMonitorSession(event: ZLinkBackendSocketMonitorEvent): ZLinkStreamSessionRuntime | undefined {
    if (event.routingId !== undefined) {
      const session = this.getActiveSession(streamSessionIdFromRoutingId(event.routingId));
      if (session !== undefined) {
        return session;
      }
    }
    if (streamMonitorHasEndpoint(event)) {
      let match: ZLinkStreamSessionRuntime | undefined;
      let matchCount = 0;
      for (const session of this.sessions.values()) {
        if (session.isDisconnected
          || session.stream.localAddr !== event.localAddr
          || session.stream.remoteAddr !== event.remoteAddr) {
          continue;
        }
        match = session;
        matchCount += 1;
        if (matchCount > 1) return undefined;
      }
      return matchCount === 1 ? match : undefined;
    }
    return undefined;
  }

  private enqueueEndpointlessDisconnect(error: Error): void {
    const sessionId = this.takeUnaddressedMonitorSession();
    if (sessionId !== undefined) {
      this.getActiveSession(sessionId)?.enqueueDisconnected(error);
      return;
    }
    let session: ZLinkStreamSessionRuntime | undefined;
    let activeCount = 0;
    for (const candidate of this.sessions.values()) {
      if (candidate.isDisconnected) continue;
      session = candidate;
      activeCount += 1;
      if (activeCount > 1) return;
    }
    if (activeCount !== 1 || session === undefined) {
      return;
    }
    if (this.pendingEndpointlessDisconnect !== undefined) {
      this.pendingEndpointlessDisconnect.cancelled = true;
    }
    const pending = {
      session,
      cancelled: false,
      error,
      activityVersion: this.activityVersion
    };
    this.pendingEndpointlessDisconnect = pending;
    // A pending endpointless disconnect must yield to a recv that is already
    // queued. Wake the recv loop instead of waiting for its idle backoff.
    this.wakeReceiveLoop();
    setImmediate(() => {
      if (
        pending.cancelled
        || this.stopped
        || this.pendingEndpointlessDisconnect !== pending
        || this.activityVersion !== pending.activityVersion
        || this.sessions.get(pending.session.stream.sessionId) !== pending.session
      ) {
        return;
      }
      this.pendingEndpointlessDisconnect = undefined;
      pending.session.enqueueDisconnected(pending.error);
    });
  }

  private waitReceiveLoopIdle(): Promise<void> {
    return this.receiveIdleWaiter.wait();
  }

  private wakeReceiveLoop(): void {
    this.receiveIdleWaiter.wake();
  }

  private getOrCreateSession(routingId: unknown): ZLinkStreamSessionRuntime | undefined {
    const sessionId = streamSessionIdFromRoutingId(routingId);
    const existing = this.getActiveSession(sessionId);
    if (existing !== undefined) {
      return existing;
    }
    if (this.options.acceptNewSession?.() === false) {
      const rejected = new ZLinkManagedStream(
        this.options.socket,
        routingId,
        this.options.messageSerializers,
        this.options.nativeSessionService,
        this.options.meshCompletions,
        undefined,
        undefined,
        this.options.nativeSessionRouteForMesh
      );
      void rejected.closeForDrain().catch((error) => this.options.onError?.(error));
      return undefined;
    }
    const created = new ZLinkStreamSessionRuntime(
      { ...this.options, submitter: this.submitter },
      routingId,
      (sessionId, session) => {
        if (this.sessions.get(sessionId) === session) {
          this.sessions.delete(sessionId);
        }
        if (this.receiveStates.get(sessionId)?.session === session) {
          this.removeReceiveState(sessionId);
        }
      },
      this.dispatchCapacity
    );
    this.sessions.set(sessionId, created);
    return created;
  }
}

function streamMonitorEndpointKey(localAddr: string | undefined, remoteAddr: string | undefined): string {
  return `${localAddr ?? ''}\n${remoteAddr ?? ''}`;
}

function streamMonitorHasEndpoint(event: ZLinkBackendSocketMonitorEvent): boolean {
  const endpoint = event as { readonly localAddr?: string; readonly remoteAddr?: string };
  return endpoint.localAddr !== undefined || endpoint.remoteAddr !== undefined;
}

function isPromiseLike<T>(value: T | Promise<T>): value is Promise<T> {
  return typeof (value as { then?: unknown }).then === 'function';
}

function normalizeStreamCloseReason(reason: string): string {
  switch (reason) {
    case 'client_close':
    case 'idle_timeout':
    case 'heartbeat_timeout':
    case 'protocol_error':
      return reason;
    case 'server_shutdown':
    case 'server_drain':
      return 'server_shutdown';
    default:
      return 'transport_error';
  }
}
