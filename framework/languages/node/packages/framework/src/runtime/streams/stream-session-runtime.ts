import { ZLinkFrameworkInternalErrorKind, createInternalFrameworkException  } from '../framework-errors-internal';
import type {
  ZLinkMessageSerializer,
  RoutingId,
  ZLinkSession
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
  ZLinkBackendStreamPacket,
  ZLinkBackendStreamSocket
} from '../backend/contracts';
import type { ZLinkMeshCompletionTable } from '../backend/mesh-completion-table';
import type { StreamSessionService } from '../foundation/service-runtime-contracts';
import {
  decodeStreamHeader,
  messageToBytes,
  streamCodecContentType,
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
import { createSessionDispatchContext, DefaultZLinkSessionContext } from './session-context';
import { ZLinkSessionSerialExecutor } from './session-serial-executor';
import type { ZLinkApplicationWorkClaim } from '../admission';
import { ownedMessage } from './stream-message-utils';
import type { ServiceActorRef } from '../foundation/service-stateful-registry';
import type { ServiceRetiredBoundSessionRouteFence } from '../foundation/service-stateful-wire-codec';
import type {
  ApplicationJobPermitPort,
  ApplicationJobQueuePort
} from '../application-jobs/contracts';
import { runWithApplicationJobPermit } from '../application-jobs/application-job-queue-scope';
import { releaseApplicationJobPermitBeforeHandler } from '../application-jobs/application-job-queue-scope';

const ZLINK_SEND_DONT_WAIT = 1;
const ZLINK_RECV_DONT_WAIT = 1;
const ZLINK_STREAM_HEARTBEAT_INTERVAL_MS = 1_000;
const ZLINK_STREAM_HEARTBEAT_TIMEOUT_MS = 5_000;
const ZLINK_STREAM_APPLICATION_IDLE_TIMEOUT_MS = 30_000;
const ZLINK_STREAM_RECEIVE_FRAME_BATCH_LIMIT = 64;
const ZLINK_STREAM_ACTOR_BINDING_REPLACEMENT_CALLBACK_TIMEOUT_MS = 30_000;
const ZLINK_STREAM_ACTOR_BINDING_REPLACEMENT_CLOSE_DELAY_MS = 100;

interface ZLinkStreamLivenessClock {
  now(): number;
  setTimer(callback: () => void, delayMs: number): unknown;
  clearTimer(timer: unknown): void;
}

interface ZLinkRetainedStreamOwner {
  close(): void;
}

interface ZLinkStreamLivenessOptions {
  readonly livenessClock?: ZLinkStreamLivenessClock;
  readonly acceptNewSession?: () => boolean;
  readonly claimApplicationWork?: () => ZLinkApplicationWorkClaim;
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
  /** Internal lifecycle deadline for a stalled replacement callback. */
  readonly replacementCallbackTimeoutMs?: number;
}

export interface ZLinkStreamSessionNodeRuntimeOptions extends ZLinkStreamSessionRuntimeOptions {
  /** Readiness is checked before each nonblocking Framework recv. */
  readonly readablePoller: ZLinkBackendReadablePoller;
  readonly createPacket: () => ZLinkBackendStreamPacket;
  readonly nodeName?: string;
  readonly monitor?: ZLinkBackendSocketMonitor;
  readonly applicationJobQueue: ApplicationJobQueuePort;
}

export class ZLinkStreamSessionRuntime {
  readonly stream: ZLinkManagedStream;
  readonly context: DefaultZLinkSessionContext;
  private readonly sessionReady: Promise<ZLinkSession>;
  private readonly serial = new ZLinkSessionSerialExecutor();
  private connected = false;
  private disconnected = false;
  private disconnectQueued = false;
  private disposed = false;
  private closeReason = 'client_close';
  private metricsClosed = false;
  private readonly livenessClock: ZLinkStreamLivenessClock;
  private lastApplicationActivityAt = 0;
  private awaitingPongSince: number | undefined;
  private livenessTimer: unknown;

  constructor(
    private readonly options: ZLinkStreamSessionRuntimeOptions & ZLinkStreamLivenessOptions,
    routingId: unknown,
    private readonly removeSession: (sessionId: string, session: ZLinkStreamSessionRuntime) => void = () => {}
  ) {
    this.livenessClock = options.livenessClock ?? systemLivenessClock;
    this.stream = new ZLinkManagedStream(
      options.socket,
      routingId,
      options.messageSerializers,
      options.nativeSessionService,
      options.meshCompletions,
      undefined,
      options.nativeSessionRouteForMesh
    );
    this.context = options.bindingRuntime.createSessionContext(
      this.stream,
      (signal) => this.close(signal),
      options.providerResolver
    );
    this.context.setActorBindingReplacementHandler((actor, retiredSession) => {
      this.enqueueActorBindingReplaced(actor, retiredSession);
    });
    this.context.setFrameWrittenObserver((kind, packetName, correlationId) => {
      this.traceFrameWritten(kind, packetName, correlationId);
    });
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
    return this.disconnected || this.disconnectQueued;
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
    return this.requireProvidedContext(session);
  }

  private requireSession(): Promise<ZLinkSession> {
    return this.sessionReady;
  }

  enqueueConnected(localAddr?: string, remoteAddr?: string): void {
    this.serial.executeInfrastructure(async () => this.markConnected(localAddr, remoteAddr));
  }

  enqueuePacket(
    payload: Message,
    decodedHeader: ZLinkStreamFrameHeader,
    terminalOwner?: ZLinkRetainedStreamOwner,
    applicationJobPermit?: ApplicationJobPermitPort
  ): void {
    let terminalReleased = false;
    const releaseTerminal = (): void => {
      if (terminalReleased) return;
      terminalReleased = true;
      terminalOwner?.close();
    };
    if (decodedHeader.kind === ZLinkStreamMessageKind.Control) {
      if (
        messageToBytes(payload).length === 0
        && (decodedHeader.name === ZLINK_STREAM_HEARTBEAT_PONG
          || decodedHeader.name === ZLINK_STREAM_HEARTBEAT_PING)
      ) {
        void this.handleControl(decodedHeader, payload).catch(error => {
          this.options.onError?.(error);
        }).finally(() => {
          applicationJobPermit?.releaseAfterInternalProcessing();
          payload.close();
          releaseTerminal();
        });
        return;
      }
      this.enqueueControl(
        async () => {
          try {
            const dispatch = () => this.dispatchPacket(payload, decodedHeader);
            if (applicationJobPermit === undefined) await dispatch();
            else await runWithApplicationJobPermit(applicationJobPermit, dispatch);
          } finally {
            releaseTerminal();
          }
        },
        () => {
          applicationJobPermit?.releaseAfterInternalProcessing();
          payload.close();
          releaseTerminal();
        },
        () => this.traceShutdownDrop(decodedHeader)
      );
      return;
    }
    if (
      decodedHeader.kind === ZLinkStreamMessageKind.Response
      || decodedHeader.kind === ZLinkStreamMessageKind.Error
    ) {
      this.enqueueInfrastructure(
        async () => {
          try {
            await this.dispatchPacket(payload, decodedHeader);
          } finally {
            releaseTerminal();
          }
        },
        () => {
          applicationJobPermit?.releaseAfterInternalProcessing();
          payload.close();
          releaseTerminal();
        },
        () => this.traceShutdownDrop(decodedHeader)
      );
      return;
    }
    this.enqueueApplication(
      async () => {
        try {
          const dispatch = () => this.dispatchPacket(payload, decodedHeader);
          if (applicationJobPermit === undefined) await dispatch();
          else await runWithApplicationJobPermit(applicationJobPermit, dispatch);
        } finally {
          releaseTerminal();
        }
      },
      (error?: unknown) => {
        applicationJobPermit?.releaseAfterInternalProcessing();
        payload.close();
        releaseTerminal();
        if (error !== undefined) {
          void this.replyDispatchError(decodedHeader, error).catch((replyError) => {
            this.options.onError?.(replyError);
          });
        }
      },
      () => this.traceShutdownDrop(decodedHeader)
    );
  }

  enqueueDisconnected(error?: unknown): void {
    this.queueDisconnect(error);
  }

  /**
   * Starts the callback on the session lane and observes its terminal result
   * without retaining that lane while application code awaits unrelated work.
   */
  enqueueActorBindingReplaced(
    actor: ServiceActorRef,
    retiredSession: ServiceRetiredBoundSessionRouteFence
  ): void {
    this.serial.executeControl(async () => {
      if (this.disposed || this.disconnected) return;
      if (!this.context.beginActorBindingReplacement(actor, retiredSession)) return;
      const session = await this.requireSession();
      let callbackDeadlineTimer: unknown;
      let callbackTerminal = false;
      let forcedClose = false;
      const scheduleClose = (): void => {
        if (forcedClose) return;
        this.livenessClock.setTimer(() => {
          if (
            this.disposed
            || this.disconnected
            || !this.context.isExactRetiredActorBinding(actor, retiredSession)
          ) {
            return;
          }
          void this.close().catch(error => this.options.onError?.(error));
        }, ZLINK_STREAM_ACTOR_BINDING_REPLACEMENT_CLOSE_DELAY_MS);
      };
      const completeCallback = (): void => {
        if (callbackTerminal) return;
        callbackTerminal = true;
        if (callbackDeadlineTimer !== undefined) {
          this.livenessClock.clearTimer(callbackDeadlineTimer);
          callbackDeadlineTimer = undefined;
        }
        scheduleClose();
      };
      const reportCallbackFailure = (error: unknown): void => {
        try {
          this.options.onError?.(error);
        } catch {
          // Diagnostics must not prevent the lifecycle terminal transition.
        }
        completeCallback();
      };
      callbackDeadlineTimer = this.livenessClock.setTimer(() => {
        if (
          this.disposed
          || this.disconnected
          || !this.context.isExactRetiredActorBinding(actor, retiredSession)
        ) {
          return;
        }
        forcedClose = true;
        void this.close().catch(error => this.options.onError?.(error));
      }, this.options.replacementCallbackTimeoutMs
        ?? ZLINK_STREAM_ACTOR_BINDING_REPLACEMENT_CALLBACK_TIMEOUT_MS);

      const callback = session.onActorBindingReplaced;
      if (callback === undefined) {
        completeCallback();
        return;
      }
      try {
        // Start the callback on this lifecycle turn, but do not retain the
        // serial lane while application code awaits an unrelated operation.
        const result = callback.call(session, this.context, actor.actorId);
        void Promise.resolve(result).then(
          () => completeCallback(),
          reportCallbackFailure
        );
      } catch (error) {
        reportCallbackFailure(error);
      }
    });
  }

  async close(signal?: AbortSignal): Promise<void> {
    throwIfAborted(signal);
    this.stream.markTransportClosed();
    await this.stream.close(signal);
    if (this.disconnected || this.disconnectQueued) {
      return;
    }
    this.queueDisconnect(undefined);
  }

  async dispose(): Promise<void> {
    if (this.disposed) {
      return;
    }
    this.disposed = true;
    this.stopLivenessChecks();
    await this.serial.dispose();
    this.stream.markTransportClosed();
    if (!this.disconnected) {
      this.disconnected = true;
      const session = await this.requireSession();
      await session.onDisconnected?.(this.context);
    }
    await this.cleanup();
  }

  async drainClose(): Promise<void> {
    this.closeReason = 'server_drain';
    await this.serial.executeFinal(async () => {
      if (this.disposed) return;
      this.stream.markTransportClosed();
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
        flowIfEnabled(this.options.dispatchErrors?.flow, ZLinkMessageFlowOutcome.Admitted)?.trace({
          outcome: ZLinkMessageFlowOutcome.Admitted,
          surface: ZLinkDispatchErrorSurface.StreamSession,
          messageKind: streamKind,
          packetName: inboundHeader.name,
          correlationId: streamCorr,
          sourceRid: this.context.routingId === undefined ? undefined : String(this.context.routingId)
        });
        releaseApplicationJobPermitBeforeHandler();
        flowIfEnabled(this.options.dispatchErrors?.flow, ZLinkMessageFlowOutcome.Dispatched)?.trace({
          outcome: ZLinkMessageFlowOutcome.Dispatched,
          surface: ZLinkDispatchErrorSurface.StreamSession,
          messageKind: streamKind,
          packetName: inboundHeader.name,
          correlationId: streamCorr,
          sourceRid: this.context.routingId === undefined ? undefined : String(this.context.routingId)
        });
        await session.onDispatch?.(
          createSessionDispatchContext(inboundHeader),
          wrapFrameworkPayloadMessage(
            dispatchPayload,
            this.options.messageSerializers,
            streamCodecContentType(inboundHeader.codec),
            inboundHeader.name
          )
        );
        if (streamKind === ZLinkDispatchMessageKind.Send) {
          flowIfEnabled(this.options.dispatchErrors?.flow, ZLinkMessageFlowOutcome.Completed)?.trace({
            outcome: ZLinkMessageFlowOutcome.Completed,
            surface: ZLinkDispatchErrorSurface.StreamSession,
            messageKind: streamKind,
            packetName: inboundHeader.name,
            correlationId: streamCorr,
            sourceRid: this.context.routingId === undefined ? undefined : String(this.context.routingId)
          });
        }
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
      await this.stream.writeControl(ZLINK_STREAM_HEARTBEAT_PONG);
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
      this.serial.executeInfrastructure(
        async () => this.runLivenessCheck(), {}, error => this.options.onError?.(error)
      );
    }, ZLINK_STREAM_HEARTBEAT_INTERVAL_MS);
  }

  private async runLivenessCheck(): Promise<void> {
    if (this.disposed || this.disconnected || !this.connected) {
      return;
    }
    const now = this.livenessClock.now();
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
    await this.stream.writeControl(ZLINK_STREAM_HEARTBEAT_PING);
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
    this.stream.markTransportClosed();
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
    if (this.context.dispatchHeader === header && this.context.replyClaimed) {
      // The handler already submitted the request's terminal reply; a second
      // error frame for the same requestSeq would duplicate the terminal
      // record (spec 26 §2.2) and confuse the peer's reply matching.
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
      this.traceFrameWritten(ZLinkStreamMessageKind.Error, header.name, header.correlationId);
    } catch (replyError) {
      this.options.onError?.(replyError);
    } finally {
      message.close();
    }
  }

  /**
   * Spec 26 §2.1/§2.2: a successful session write is the surface's terminal
   * boundary — `replied` for a request's response or error reply, `sent` for a
   * session-connected send. Exactly one record per write, matching the other
   * languages' write-path trace.
   */
  private traceFrameWritten(
    kind: ZLinkStreamMessageKind,
    packetName: string,
    correlationId: string | undefined
  ): void {
    const reply = kind === ZLinkStreamMessageKind.Response
      || kind === ZLinkStreamMessageKind.Error;
    const outcome = reply ? ZLinkMessageFlowOutcome.Replied : ZLinkMessageFlowOutcome.Sent;
    flowIfEnabled(this.options.dispatchErrors?.flow, outcome)?.trace({
      outcome,
      surface: ZLinkDispatchErrorSurface.StreamSession,
      messageKind: kind === ZLinkStreamMessageKind.Response
        ? ZLinkDispatchMessageKind.Response
        : kind === ZLinkStreamMessageKind.Error
          ? ZLinkDispatchMessageKind.Error
          : ZLinkDispatchMessageKind.Send,
      packetName: packetName.length === 0 ? undefined : packetName,
      correlationId,
      sourceRid: this.context.routingId === undefined ? undefined : String(this.context.routingId)
    });
  }

  private async complete(error: unknown, notifyDisconnected: boolean): Promise<void> {
    try {
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
    } finally {
      await this.cleanup();
    }
  }

  private queueDisconnect(error: unknown): void {
    if (this.disconnected || this.disconnectQueued) return;
    this.disconnectQueued = true;
    this.stopLivenessChecks();
    void this.serial.executeFinal(async () => {
      this.disconnectQueued = false;
      if (this.disconnected) return;
      this.stream.markTransportClosed();
      this.disconnected = true;
      await this.complete(error, true);
    }).catch(() => {
      this.disconnectQueued = false;
    });
  }

  private async cleanup(): Promise<void> {
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

  private enqueueControl(
    work: () => Promise<void>,
    onRejected?: () => void,
    onShutdownRejected?: () => void
  ): void {
    if (!this.serial.executeControl(work, {}, onRejected)) {
      // The serial lane only refuses new work after dispose() closed it.
      onShutdownRejected?.();
      onRejected?.();
    }
  }

  private enqueueInfrastructure(
    work: () => Promise<void>,
    onRejected?: () => void,
    onShutdownRejected?: () => void
  ): void {
    if (!this.serial.executeInfrastructure(work, {}, onRejected)) {
      // The serial lane only refuses new work after dispose() closed it.
      onShutdownRejected?.();
      onRejected?.();
    }
  }

  private enqueueApplication(
    work: () => Promise<void>,
    onRejected?: (error?: unknown) => void,
    onShutdownRejected?: () => void
  ): void {
    let claim: ZLinkApplicationWorkClaim | undefined;
    try {
      claim = this.options.claimApplicationWork?.();
    } catch (error) {
      onRejected?.(error);
      return;
    }
    if (!this.serial.executeApplication(async () => {
      try {
        await work();
      } finally {
        claim?.close();
      }
    }, {}, () => {
      claim?.close();
      onRejected?.();
    })) {
      claim?.close();
      // The serial lane only refuses new work after dispose() closed it.
      onShutdownRejected?.();
      onRejected?.();
    }
  }

  /**
   * Spec 26 §2.1/§3.1: a frame discarded because the session serial lane is
   * closed for shutdown is recorded as `phase=dropped` with the closed
   * reason `shutdown`. Drops are recorded from the Errors level up and are
   * never sampled out.
   */
  private traceShutdownDrop(header: ZLinkStreamFrameHeader): void {
    const flowEnabled = this.options.dispatchErrors?.flow.flowCreationEnabled() ?? true;
    flowIfEnabled(
      this.options.dispatchErrors?.flow,
      ZLinkMessageFlowOutcome.Dropped,
      undefined,
      flowEnabled ? header.flowId : undefined
    )?.trace({
      outcome: ZLinkMessageFlowOutcome.Dropped,
      surface: ZLinkDispatchErrorSurface.StreamSession,
      messageKind: streamShutdownDropMessageKind(header.kind),
      packetName: header.name === '' ? undefined : header.name,
      correlationId: header.correlationId,
      sourceRid: this.context.routingId === undefined ? undefined : String(this.context.routingId),
      flowId: flowEnabled ? header.flowId : undefined,
      flowOrigin: flowEnabled ? header.flowOrigin : undefined,
      errorReason: ZLinkDispatchErrorReason.Shutdown
    });
  }
}

function streamShutdownDropMessageKind(kind: ZLinkStreamMessageKind): ZLinkDispatchMessageKind {
  switch (kind) {
    case ZLinkStreamMessageKind.Request: return ZLinkDispatchMessageKind.Request;
    case ZLinkStreamMessageKind.Response: return ZLinkDispatchMessageKind.Response;
    case ZLinkStreamMessageKind.Error: return ZLinkDispatchMessageKind.Error;
    case ZLinkStreamMessageKind.Control: return ZLinkDispatchMessageKind.Control;
    default: return ZLinkDispatchMessageKind.Send;
  }
}

export class ZLinkStreamSessionNodeRuntime {
  private readonly sessions = new Map<string, ZLinkStreamSessionRuntime>();
  private readonly availablePackets: ZLinkBackendStreamPacket[] = [];
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
  private readonly receiveAbortController = new AbortController();
  private receiveLoop: Promise<void> | undefined;
  private readonly receiveIdleWaiter = new ZLinkStreamIdleWaiter(5);
  private receiveWorkSinceYield = 0;
  constructor(
    private readonly options: ZLinkStreamSessionNodeRuntimeOptions & ZLinkStreamLivenessOptions
  ) {}

  start(): void {
    if (this.stopped || this.receiveLoop !== undefined) {
      return;
    }
    this.options.monitor?.onEvent(event => this.onMonitorEvent(event));
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
      const sessions = [...this.sessions.values()];
      this.sessions.clear();
      for (const packet of this.availablePackets.splice(0)) packet.close();
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
      this.drainMonitorEvents();
      let permit: ApplicationJobPermitPort | undefined;
      let packet: ZLinkBackendStreamPacket | undefined;
      let packetTransferred = false;
      try {
        permit = await this.options.applicationJobQueue.acquire(signal);
        if (this.isReceiveStopped(signal)) {
          permit.releaseAfterInternalProcessing();
          break;
        }
        if (!this.options.readablePoller.wait(0)) {
          permit.releaseAfterInternalProcessing();
          await this.waitReceiveLoopIdle();
          continue;
        }
        packet = this.takePacket();
        if (!this.options.socket.recvPacket(packet, ZLINK_RECV_DONT_WAIT)) {
          permit.releaseAfterInternalProcessing();
          this.recyclePacket(packet);
          await this.waitReceiveLoopIdle();
          continue;
        }
        packetTransferred = true;
        this.acceptPacket(packet, permit);
        packet = undefined;
        permit = undefined;
      } catch (error) {
        if (!packetTransferred) permit?.releaseAfterInternalProcessing();
        if (packet !== undefined && !packetTransferred) this.recyclePacket(packet);
        if (!this.isReceiveStopped(signal) && !packetTransferred) {
          this.options.onError?.(error);
          this.receiveWorkSinceYield = 0;
          await this.waitReceiveLoopIdle();
        } else if (!this.isReceiveStopped(signal)) {
          this.receiveWorkSinceYield = 0;
          await this.waitReceiveLoopIdle();
        }
        continue;
      }

      this.receiveWorkSinceYield += 1;
      if (
        this.receiveWorkSinceYield >= ZLINK_STREAM_RECEIVE_FRAME_BATCH_LIMIT
        && !this.isReceiveStopped(signal)
      ) {
        this.receiveWorkSinceYield = 0;
        await new Promise<void>((resolve) => setImmediate(resolve));
      }
    }
  }

  private acceptPacket(
    packet: ZLinkBackendStreamPacket,
    permit: ApplicationJobPermitPort
  ): void {
    const routingId = packet.routingId;
    const header = packet.header;
    const body = packet.body;
    let releasePermitOnFailure = true;
    let packetTransferred = false;
    let payload: Message | undefined;
    let session: ZLinkStreamSessionRuntime | undefined;
    try {
      if (routingId === null || header === null || body === null) {
        throw new Error('STREAM packet did not provide routing id, header, and body.');
      }

      this.activityVersion += 1;
      session = this.getOrCreateSession(routingId);
      if (session === undefined) return;
      this.applyPendingConnectionMetadata(session);
      const decodedHeader = decodeStreamHeader(
        header.data(),
        this.options.dispatchErrors?.flow.flowCreationEnabled() ?? true
      );
      payload = ownedMessage(body.data());
      const terminalCompletion = decodedHeader.kind === ZLinkStreamMessageKind.Response
        || decodedHeader.kind === ZLinkStreamMessageKind.Error;
      const applicationPermit = terminalCompletion ? undefined : permit;
      if (terminalCompletion) {
        permit.releaseAfterInternalProcessing();
        releasePermitOnFailure = false;
      } else {
        if (
          decodedHeader.kind === ZLinkStreamMessageKind.Send
          || decodedHeader.kind === ZLinkStreamMessageKind.Request
        ) {
          permit.markApplicationQueued();
        }
      }
      const owner: ZLinkRetainedStreamOwner = {
        close: () => this.recyclePacket(packet)
      };
      session.enqueuePacket(payload, decodedHeader, owner, applicationPermit);
      releasePermitOnFailure = false;
      payload = undefined;
      packetTransferred = true;
    } catch (error) {
      const failure = error instanceof Error ? error : new Error(String(error));
      if (routingId !== null && session !== undefined) {
        this.handleMalformedPacket(routingId, session, failure);
      } else {
        this.options.onError?.(failure);
        if (routingId !== null) {
          try {
            this.options.socket.disconnectPeer(routingId);
          } catch (disconnectError) {
            this.options.onError?.(disconnectError);
          }
        }
      }
      throw failure;
    } finally {
      if (!packetTransferred) {
        if (releasePermitOnFailure) permit.releaseAfterInternalProcessing();
        payload?.close();
        this.recyclePacket(packet);
      }
    }
  }

  private takePacket(): ZLinkBackendStreamPacket {
    return this.availablePackets.pop() ?? this.options.createPacket();
  }

  private recyclePacket(packet: ZLinkBackendStreamPacket): void {
    packet.close();
    if (!this.stopped) this.availablePackets.push(packet);
  }

  private drainMonitorEvents(): void {
    this.options.monitor?.drain();
  }

  private handleMalformedPacket(
    routingId: unknown,
    session: ZLinkStreamSessionRuntime,
    error: Error
  ): void {
    this.options.onError?.(error);
    try {
      this.options.socket.disconnectPeer(routingId as RoutingId);
    } catch (disconnectError) {
      this.options.onError?.(disconnectError);
    }
    session.enqueueDisconnected(error);
  }

  private isReceiveStopped(signal: AbortSignal): boolean {
    return this.stopped || signal.aborted;
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
        this.options.nativeSessionRouteForMesh
      );
      void rejected.closeForDrain().catch((error) => this.options.onError?.(error));
      return undefined;
    }
    const created = new ZLinkStreamSessionRuntime(
      this.options,
      routingId,
      (sessionId, session) => {
        if (this.sessions.get(sessionId) === session) {
          this.sessions.delete(sessionId);
        }
      }
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
