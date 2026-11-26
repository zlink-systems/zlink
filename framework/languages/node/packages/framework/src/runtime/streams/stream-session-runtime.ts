import { requireOneWayCompletion } from '../messaging/submission-result';
import { diag } from '@opentelemetry/api';
import { logs, SeverityNumber } from '@opentelemetry/api-logs';
import { ZlinkStreamControlPacket } from '@zlink-systems/stream-wire';
import {
  type ZLinkDisconnectReason,
  ZLinkStreamSessionError
} from '../../contracts/Streams/ZLinkStreamError';
import type { RoutingId, ZLinkMessageSerializer, ZLinkSession } from '../../contracts';
import type { Message } from '../../contracts/Common/Message';
import type { ZLinkProviderResolver } from '../../contracts/Common/ZLinkProviderResolver';
import { DEFAULT_SESSION_REPLACEMENT_CALLBACK_TIMEOUT_MS } from '../../contracts/Configuration/Registration';
import {
  ZLinkFrameworkErrorKind,
  ZLinkFrameworkException
} from '../../contracts/Errors/ZLinkFrameworkException';
import {
  ZLinkRuntimeDispatchErrorAction as ZLinkDispatchErrorAction,
  ZLinkRuntimeDispatchErrorReason as ZLinkDispatchErrorReason,
  ZLinkDispatchErrorSurface,
  ZLinkDispatchMessageKind,
  ZLinkRuntimeMessageFlowOutcome as ZLinkMessageFlowOutcome
} from '../../contracts/Dispatch/ZLinkDispatchOptions';
import { awaitWithAbort, createAbortError, isAbortError, isPromiseLike } from '../abort';
import { type ZLinkApplicationWorkClaim, shouldCompactBackingArray } from '../admission';

import {
  releaseApplicationJobPermitBeforeHandler,
  runWithApplicationJobPermit
} from '../application-jobs/application-job-queue-scope';
import type {
  ApplicationJobPermitPort,
  ApplicationJobQueuePort
} from '../application-jobs/contracts';
import type {
  ZLinkBackendReadablePoller,
  ZLinkBackendSocketMonitor,
  ZLinkBackendSocketMonitorEvent,
  ZLinkBackendStreamPacket,
  ZLinkBackendStreamSocket
} from '../backend/contracts';
import type { ZLinkMeshCompletionTable } from '../backend/mesh-completion-table';
import { ZLinkReceiveBatchBudget } from '../backend/receive-batch-budget';
import { ZLinkDispatchErrorReporter, ZLinkRouteDisconnectedError } from '../channels';
import {
  type ZLinkRuntimeMetrics,
  flowIfEnabled,
  isRelocationDebugEnabled,
  relocationDebug,
  relocationDebugTerminal
} from '../diagnostics';
import { dispatchErrorDetails } from '../diagnostics/dispatch-error-details';
import { TELEMETRY_INSTRUMENTATION_NAME } from '../diagnostics/message-flow-record';

import { createInboundFlow, runWithFlow } from '../diagnostics/flow-context';
import { ZLinkSocketNativeEventType } from '../diagnostics/internal-event-contracts';
import { METRIC_NAMES } from '../diagnostics/runtime-metrics';
import type { StreamSessionService } from '../foundation/service-runtime-contracts';
import type { ServiceActorRef } from '../foundation/service-stateful-registry';
import type { ServiceRetiredBoundSessionRouteFence } from '../foundation/service-stateful-wire-codec';
import {
  createInternalFrameworkException,
  ZLinkFrameworkInternalErrorKind
} from '../framework-errors-internal';
import { wrapFrameworkPayloadMessage } from '../messaging/payload-codec';
import type { ZLinkAuthenticationAdmission } from './authentication-admission';
import { boundSessionErrorPayload } from './bound-session-response-target';
import {
  streamSessionIdFromRoutingId,
  ZLinkManagedStream,
  type ZLinkNativeSessionRoute
} from './managed-stream';
import {
  decodeStreamHeader,
  messageToBytes,
  streamCodecContentType,
  ZLINK_STREAM_HEARTBEAT_PING,
  ZLINK_STREAM_HEARTBEAT_PONG,
  ZLinkStreamCloseReasonCode,
  ZLinkStreamMessageKind,
  type ZLinkStreamFrameHeader
} from './protocol';
import { createSessionDispatchContext, DefaultZLinkSessionContext } from './session-context';
import { ZLinkSessionSerialExecutor } from './session-serial-executor';
import { ownedMessage } from './stream-message-utils';
import { normalizeStreamLiveness } from '../../contracts/Configuration/RegistrationNormalizers';

const ZLINK_RECV_DONT_WAIT = 1;
const ZLINK_STREAM_ACTOR_BINDING_REPLACEMENT_CLOSE_DELAY_MS = 100;
const ZLINK_STREAM_MONITOR_IDLE_MIN_DELAY_MS = 1;
const ZLINK_STREAM_MONITOR_IDLE_MAX_DELAY_MS = 20;
const ZLINK_STREAM_MONITOR_IDLE_MAX_STEPS = 7;
const ZLINK_REJECTED_STREAM_ADMISSION_LIMIT = 4096;

interface ZLinkStreamLivenessClock {
  now(): number;
  setTimer(callback: () => void, delayMs: number): unknown;
  clearTimer(timer: unknown): void;
}

interface ZLinkRetainedStreamOwner {
  close(): void;
}

interface ZLinkStreamSessionAdmission {
  readonly triggeringHeader?: ZLinkStreamFrameHeader;
}

interface ZLinkStreamSessionCreation {
  readonly result: Promise<ZLinkSession | undefined>;
  readonly rejected: boolean;
}

interface ZLinkStreamLivenessOptions {
  readonly livenessClock?: ZLinkStreamLivenessClock;
  readonly acceptNewSession?: () => boolean;
  readonly claimApplicationWork?: () => ZLinkApplicationWorkClaim;
}

const MAX_TIMER_DELAY_MS = 2_147_483_647;
const STREAM_RUNTIME_LOGGER = logs.getLogger(TELEMETRY_INSTRUMENTATION_NAME);

const systemLivenessClock: ZLinkStreamLivenessClock = {
  now: () => performance.now(),
  setTimer: (callback, delayMs) => setTimeout(callback, Math.min(delayMs, MAX_TIMER_DELAY_MS)),
  clearTimer: (timer) => clearTimeout(timer as ReturnType<typeof setTimeout>)
};

export interface ZLinkStreamSessionContextFactory {
  createSessionContext(
    stream: ZLinkManagedStream,
    close?: (signal?: AbortSignal) => Promise<void>,
    providerResolver?: ZLinkProviderResolver
  ): DefaultZLinkSessionContext;
}

export interface ZLinkStreamSessionRuntimeOptions {
  readonly nodeName?: string;
  readonly sessionTypeName?: string;
  readonly heartbeatIntervalMs?: number;
  readonly heartbeatTimeoutMs?: number;
  readonly idleTimeoutMs?: number;
  readonly socket: ZLinkBackendStreamSocket;
  readonly nativeSessionService?: StreamSessionService;
  readonly meshCompletions?: ZLinkMeshCompletionTable;
  readonly nativeSessionRouteForMesh?: (meshName: string) => ZLinkNativeSessionRoute | undefined;
  readonly sessionFactory: (
    context: DefaultZLinkSessionContext
  ) => ZLinkSession | Promise<ZLinkSession>;
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
  readonly monitor?: ZLinkBackendSocketMonitor;
  readonly applicationJobQueue: ApplicationJobQueuePort;
  readonly authenticationAdmission?: ZLinkAuthenticationAdmission;
}

class ZLinkStreamReaderLiveness {
  private permitResumedAt = -Infinity;

  isWaitingForPermit(): boolean {
    return this.permitResumedAt === Infinity;
  }

  acquire(
    queue: ApplicationJobQueuePort,
    signal: AbortSignal,
    clock: ZLinkStreamLivenessClock
  ): Promise<ApplicationJobPermitPort> {
    return new Promise((resolve, reject) => {
      const previousResume = this.permitResumedAt;
      const rejectAcquire = (error: unknown): void => {
        this.permitResumedAt = previousResume;
        reject(error);
      };
      // The queue can call a grant or rejection callback before returning.
      this.permitResumedAt = Infinity;
      try {
        const permit = queue.acquireOrResume(
          (granted) => {
            this.permitResumedAt = clock.now();
            resolve(granted);
          },
          rejectAcquire,
          signal,
          'remote'
        );
        if (permit !== undefined) {
          this.permitResumedAt = previousResume;
          resolve(permit);
        }
      } catch (error) {
        rejectAcquire(error);
      }
    });
  }

  inboundDeadline(lastReceivedAt: number, timeoutMs: number): number {
    return timeoutMs > 0 ? Math.max(lastReceivedAt, this.permitResumedAt) + timeoutMs : Infinity;
  }
}

export class ZLinkStreamSessionRuntime {
  readonly stream: ZLinkManagedStream;
  readonly context: DefaultZLinkSessionContext;
  private creation: ZLinkStreamSessionCreation;
  private readonly serial = new ZLinkSessionSerialExecutor();
  private authenticationTerminals: Set<() => void> | undefined;
  private connected = false;
  private disconnected = false;
  private disconnectQueued = false;
  private disposed = false;
  private closeReason = 'client_close';
  private metricsClosed = false;
  private readonly livenessClock: ZLinkStreamLivenessClock;
  private readonly liveness: ReturnType<typeof normalizeStreamLiveness>;
  private lastApplicationActivityAt = 0;
  private lastInboundFrameAt = 0;
  private nextHeartbeatPingAt = 0;
  private livenessTimer: unknown;

  constructor(
    private readonly options: ZLinkStreamSessionRuntimeOptions & ZLinkStreamLivenessOptions,
    routingId: unknown,
    private readonly removeSession: (
      sessionId: string,
      session: ZLinkStreamSessionRuntime,
      creationFailed: boolean
    ) => void = () => {},
    admission?: ZLinkStreamSessionAdmission,
    private readonly readerLiveness = new ZLinkStreamReaderLiveness(),
    private readonly wakeReader?: () => void
  ) {
    this.liveness = normalizeStreamLiveness(options);
    this.livenessClock = options.livenessClock ?? systemLivenessClock;
    this.stream = new ZLinkManagedStream(
      options.socket,
      routingId,
      options.messageSerializers,
      options.nativeSessionService,
      options.meshCompletions,
      undefined,
      options.nativeSessionRouteForMesh,
      () => this.completeAuthenticationRequests()
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
    this.creation = this.createSession(admission);
  }

  get session(): Promise<ZLinkSession | undefined> {
    return this.creation.result;
  }

  get isCreationRejected(): boolean {
    return this.creation.rejected;
  }

  shutdownSnapshot(): Record<string, unknown> {
    return {
      sessionId: this.stream.sessionId,
      connected: this.connected,
      disconnected: this.disconnected,
      disconnectQueued: this.disconnectQueued,
      disposed: this.disposed,
      transportClosed: this.stream.shutdownTransportClosed,
      ...this.serial.shutdownSnapshot(),
      routingId: this.context.routingId === undefined ? undefined : String(this.context.routingId),
      boundActors: [...this.context.boundActors].map((actor) => actor.actorId)
    };
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

  private createSession(admission?: ZLinkStreamSessionAdmission): ZLinkStreamSessionCreation {
    let session: ZLinkSession | Promise<ZLinkSession>;
    try {
      session = this.options.sessionFactory(this.context);
      if (!isPromiseLike(session)) {
        return { result: Promise.resolve(this.completeSessionCreation(session)), rejected: false };
      }
    } catch (error) {
      if (admission === undefined) throw error;
      return this.rejectCreation(error, admission);
    }

    const complete = (created: ZLinkSession) => this.completeSessionCreation(created);
    if (admission === undefined) {
      return {
        result: Promise.resolve(session).then(complete, (error) => {
          this.context.completeConfiguration();
          throw error;
        }),
        rejected: false
      };
    }
    const result = Promise.resolve(session)
      .then(complete)
      .catch((error) => {
        this.creation = this.rejectCreation(error, admission);
        return this.creation.result;
      });
    return { result, rejected: false };
  }

  private rejectCreation(
    error: unknown,
    admission: ZLinkStreamSessionAdmission
  ): ZLinkStreamSessionCreation {
    // Commit the rejected admission before diagnostics or transport callbacks.
    return {
      result: Promise.resolve().then(() =>
        this.failSessionCreation(error, admission.triggeringHeader)
      ),
      rejected: true
    };
  }

  private async failSessionCreation(
    error: unknown,
    header?: ZLinkStreamFrameHeader
  ): Promise<undefined> {
    this.context.completeConfiguration();
    const diagnostic = 'STREAM session creation failed.';
    try {
      try {
        const details = dispatchErrorDetails(error);
        STREAM_RUNTIME_LOGGER.emit({
          eventName: 'zlink.stream.session_creation_failed',
          severityNumber: SeverityNumber.ERROR,
          severityText: 'ERROR',
          body: diagnostic,
          attributes: {
            node_name: this.options.nodeName,
            session_type: this.options.sessionTypeName,
            stream_session_id: this.stream.sessionId,
            'exception.type': details.errorType,
            'exception.message': details.errorMessage,
            'exception.stacktrace': error instanceof Error ? error.stack : undefined
          }
        });
      } finally {
        if (header !== undefined) {
          const request = header.kind === ZLinkStreamMessageKind.Request;
          const flowEnabled = this.options.dispatchErrors?.flow.flowCreationEnabled() ?? false;
          await runWithFlow(
            createInboundFlow(header.flowId, header.flowOrigin, flowEnabled, this.stream.sessionId),
            async () => {
              this.options.dispatchErrors?.report({
                surface: ZLinkDispatchErrorSurface.StreamSession,
                messageKind: streamDispatchMessageKind(header.kind),
                reason: ZLinkDispatchErrorReason.SessionCreationFailed,
                action: request
                  ? ZLinkDispatchErrorAction.ReplyError
                  : ZLinkDispatchErrorAction.Drop,
                packetName: header.name,
                sourceRid: this.stream.sessionId,
                correlationId: header.correlationId,
                error
              });
              if (request) {
                await this.replyDispatchError(
                  header,
                  new ZLinkFrameworkException(ZLinkFrameworkErrorKind.InternalFailure, diagnostic)
                );
              }
            }
          );
        }
        this.options.onError?.(error);
      }
    } catch (diagnosticError) {
      diag.error('STREAM session creation failure reporting failed.', diagnosticError);
    } finally {
      try {
        if (!this.stream.shutdownTransportClosed) {
          await this.stream.closeForReason(ZLinkStreamCloseReasonCode.TransportError, diagnostic);
        }
      } catch (closeError) {
        diag.error(
          'Could not close a STREAM connection after session creation failed.',
          closeError
        );
      } finally {
        this.enqueueDisconnected(undefined, 'transport_error');
      }
    }
    return undefined;
  }

  enqueueConnected(
    localAddr?: string,
    remoteAddr?: string,
    connectedAt = this.livenessClock.now()
  ): void {
    this.serial.executeControl(async () => this.markConnected(connectedAt, localAddr, remoteAddr));
  }

  recordInbound(): void {
    this.lastInboundFrameAt = Math.max(this.lastInboundFrameAt, this.livenessClock.now());
  }

  enqueuePacket(
    payload: Message,
    decodedHeader: ZLinkStreamFrameHeader,
    terminalOwner?: ZLinkRetainedStreamOwner,
    applicationJobPermit?: ApplicationJobPermitPort,
    releaseAuthentication?: () => void
  ): void {
    this.recordInbound();
    this.lastApplicationActivityAt = this.lastInboundFrameAt;
    const completeAdmission =
      releaseAuthentication === undefined
        ? undefined
        : this.trackAuthentication(releaseAuthentication);
    let terminalReleased = false;
    const releaseTerminal = (): void => {
      if (terminalReleased) return;
      terminalReleased = true;
      completeAdmission?.();
      terminalOwner?.close();
    };
    if (
      decodedHeader.kind === ZLinkStreamMessageKind.Response ||
      decodedHeader.kind === ZLinkStreamMessageKind.Error
    ) {
      this.enqueueControl(
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
          const dispatch = () => this.dispatchPacket(payload, decodedHeader, completeAdmission);
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

  private trackAuthentication(release: () => void): () => void {
    const terminals = (this.authenticationTerminals ??= new Set());
    const complete = (): void => {
      if (terminals.delete(complete)) release();
    };
    terminals.add(complete);
    if (this.stream.shutdownTransportClosed) complete();
    return complete;
  }

  private completeAuthenticationRequests(): void {
    if (this.authenticationTerminals === undefined) return;
    for (const complete of this.authenticationTerminals) complete();
  }

  enqueueDisconnected(
    error?: unknown,
    closeReason?: string,
    disconnectReason?: ZLinkDisconnectReason
  ): void {
    if (closeReason !== undefined) this.closeReason = closeReason;
    this.queueDisconnect(error, disconnectReason);
  }

  /**
   * Retains the session turn through the callback's terminal result.
   */
  enqueueActorBindingReplaced(
    actor: ServiceActorRef,
    retiredSession: ServiceRetiredBoundSessionRouteFence
  ): void {
    this.serial.executeControl(async () => {
      if (this.disposed || this.disconnected) return;
      if (!this.context.beginActorBindingReplacement(actor, retiredSession)) return;
      const session = await this.session;
      if (session === undefined || this.stream.isShutdownForced()) return;
      let callbackDeadlineTimer: unknown;
      let forcedClose = false;
      const scheduleClose = (): void => {
        if (forcedClose) return;
        this.livenessClock.setTimer(() => {
          if (
            this.disposed ||
            this.disconnected ||
            !this.context.isExactRetiredActorBinding(actor, retiredSession)
          ) {
            return;
          }
          void this.close().catch((error) => this.options.onError?.(error));
        }, ZLINK_STREAM_ACTOR_BINDING_REPLACEMENT_CLOSE_DELAY_MS);
      };
      callbackDeadlineTimer = this.livenessClock.setTimer(() => {
        if (
          this.disposed ||
          this.disconnected ||
          !this.context.isExactRetiredActorBinding(actor, retiredSession)
        ) {
          return;
        }
        forcedClose = true;
        void this.close().catch((error) => this.options.onError?.(error));
      }, this.options.replacementCallbackTimeoutMs ?? DEFAULT_SESSION_REPLACEMENT_CALLBACK_TIMEOUT_MS);

      try {
        await session.onActorBindingReplaced?.(this.context, actor.actorId);
      } catch (error) {
        this.options.onError?.(error);
      } finally {
        if (callbackDeadlineTimer !== undefined) {
          this.livenessClock.clearTimer(callbackDeadlineTimer);
        }
        scheduleClose();
      }
    });
  }

  async close(signal?: AbortSignal): Promise<void> {
    await this.stream.close(signal);
    if (this.stream.isShutdownForced() || this.disconnected || this.disconnectQueued) {
      return;
    }
    this.queueDisconnect(undefined);
  }

  async dispose(shutdownSignal?: AbortSignal): Promise<void> {
    if (this.disposed) {
      return;
    }
    this.disposed = true;
    this.stream.shutdownSignal ??= shutdownSignal;
    this.completeAuthenticationRequests();
    this.stopLivenessChecks();
    await this.waitForShutdown(this.serial.dispose());
    this.stream.markTransportClosed();
    if (!this.disconnected && !this.stream.isShutdownForced()) {
      this.disconnected = true;
      await this.waitForShutdown(this.complete(undefined));
      if (!this.stream.isShutdownForced()) return;
    }
    await this.cleanup();
  }

  async drainClose(shutdownSignal?: AbortSignal): Promise<void> {
    this.stream.shutdownSignal ??= shutdownSignal;
    if (isRelocationDebugEnabled())
      relocationDebug('session.drainClose.enter', { ...this.shutdownSnapshot() });
    let failure: { error: unknown } | undefined;
    try {
      this.closeReason = 'server_drain';
      if (isRelocationDebugEnabled())
        relocationDebug('drainClose.final.submit', { ...this.shutdownSnapshot() });
      let failure: { error: unknown } | undefined;
      try {
        const final = this.serial.executeFinal(async () => {
          if (isRelocationDebugEnabled())
            relocationDebug('drainClose.final.enter', { ...this.shutdownSnapshot() });
          let failure: { error: unknown } | undefined;
          try {
            if (
              this.disposed ||
              this.stream.isShutdownForced() ||
              (await this.session) === undefined
            )
              return;
            this.stopLivenessChecks();
            await this.stream.closeForDrain();
            if (!this.disconnected && !this.stream.isShutdownForced()) {
              this.disconnected = true;
              await this.complete(undefined);
            }
          } catch (error) {
            failure = { error };
            throw error;
          } finally {
            if (isRelocationDebugEnabled())
              relocationDebugTerminal(
                'drainClose.final.exit',
                { ...this.shutdownSnapshot() },
                failure
              );
          }
        });
        await this.waitForShutdown(final);
      } catch (error) {
        failure = { error };
        throw error;
      } finally {
        relocationDebugTerminal(
          'drainClose.final.terminal',
          { sessionId: this.stream.sessionId },
          failure
        );
      }
    } catch (error) {
      failure = { error };
      throw error;
    } finally {
      if (isRelocationDebugEnabled())
        relocationDebugTerminal('session.drainClose.exit', { ...this.shutdownSnapshot() }, failure);
    }
  }

  private async waitForShutdown(operation: Promise<void>): Promise<void> {
    try {
      await awaitWithAbort(operation, this.stream.shutdownSignal, () => {
        this.stream.markTransportClosed();
        this.stopLivenessChecks();
        this.serial.discardPending(createAbortError());
      });
    } catch (error) {
      if (!this.stream.isShutdownForced() || !isAbortError(error)) throw error;
    }
  }

  private async markConnected(
    connectedAt: number,
    localAddr?: string,
    remoteAddr?: string
  ): Promise<void> {
    this.stream.updateAddresses(localAddr, remoteAddr);
    if (this.connected) {
      return;
    }
    const session = await this.session;
    if (session === undefined || this.stream.isShutdownForced()) return;
    this.connected = true;
    this.lastApplicationActivityAt = this.livenessClock.now();
    this.nextHeartbeatPingAt = connectedAt + this.liveness.heartbeatIntervalMs;
    this.lastInboundFrameAt = Math.max(this.lastInboundFrameAt, connectedAt);
    this.scheduleLivenessCheck();
    this.options.metrics?.change(METRIC_NAMES.StreamConnectionsActive, 1, { transport: 'tcp' });
    this.options.metrics?.count(METRIC_NAMES.StreamConnectionsOpened, 1, { transport: 'tcp' });
    await session.onConnected?.(this.context);
  }

  private async dispatchPacket(
    payload: Message,
    decodedHeader: ZLinkStreamFrameHeader,
    completeAdmission?: () => void
  ): Promise<void> {
    let dispatchPayload = payload;
    let enteredDispatch = false;
    try {
      const session = await this.session;
      if (session === undefined || this.stream.isShutdownForced()) return;
      dispatchPayload = this.context.payloadForHeader(decodedHeader, payload);
      const dispatchActor = this.context.actorForSlot(decodedHeader.actorSlot);
      if (decodedHeader.actorSlot !== undefined && dispatchActor === undefined) {
        const staleFlowEnabled = this.options.dispatchErrors?.flow.flowCreationEnabled() ?? true;
        await runWithFlow(
          createInboundFlow(
            decodedHeader.flowId,
            decodedHeader.flowOrigin,
            staleFlowEnabled,
            staleFlowEnabled ? this.stream.sessionId : undefined
          ),
          async () => {
            if (decodedHeader.kind === ZLinkStreamMessageKind.Request) {
              const error = createInternalFrameworkException(
                ZLinkFrameworkInternalErrorKind.InvalidOperation,
                'Actor slot is not a current binding.'
              );
              this.options.dispatchErrors?.report({
                surface: ZLinkDispatchErrorSurface.StreamSession,
                messageKind: ZLinkDispatchMessageKind.Request,
                reason: ZLinkDispatchErrorReason.StaleTarget,
                action: ZLinkDispatchErrorAction.ReplyError,
                packetName: decodedHeader.name,
                sourceRid:
                  this.context.routingId === undefined ? undefined : String(this.context.routingId),
                correlationId: decodedHeader.correlationId,
                flowId: decodedHeader.flowId,
                flowOrigin: decodedHeader.flowOrigin,
                error
              });
              await this.replyDispatchError(decodedHeader, error);
            } else {
              flowIfEnabled(
                this.options.dispatchErrors?.flow,
                ZLinkMessageFlowOutcome.Dropped
              )?.trace({
                outcome: ZLinkMessageFlowOutcome.Dropped,
                surface: ZLinkDispatchErrorSurface.StreamSession,
                messageKind: ZLinkDispatchMessageKind.Send,
                packetName: decodedHeader.name,
                correlationId: decodedHeader.correlationId,
                sourceRid:
                  this.context.routingId === undefined ? undefined : String(this.context.routingId),
                errorReason: ZLinkDispatchErrorReason.StaleTarget
              });
            }
          }
        );
        return;
      }
      this.context.enterDispatch(decodedHeader, completeAdmission);
      enteredDispatch = true;
      const streamKind =
        decodedHeader.kind === ZLinkStreamMessageKind.Request
          ? ZLinkDispatchMessageKind.Request
          : ZLinkDispatchMessageKind.Send;
      const streamCorr = decodedHeader.correlationId;
      const inboundHeader = decodedHeader;
      const flowEnabled = this.options.dispatchErrors?.flow.flowCreationEnabled() ?? true;
      await runWithFlow(
        createInboundFlow(
          inboundHeader.flowId,
          inboundHeader.flowOrigin,
          flowEnabled,
          flowEnabled ? this.stream.sessionId : undefined
        ),
        async () => {
          flowIfEnabled(this.options.dispatchErrors?.flow, ZLinkMessageFlowOutcome.Received)?.trace(
            {
              outcome: ZLinkMessageFlowOutcome.Received,
              surface: ZLinkDispatchErrorSurface.StreamSession,
              messageKind: streamKind,
              packetName: inboundHeader.name,
              correlationId: streamCorr,
              sourceRid:
                this.context.routingId === undefined ? undefined : String(this.context.routingId)
            }
          );
          flowIfEnabled(this.options.dispatchErrors?.flow, ZLinkMessageFlowOutcome.Admitted)?.trace(
            {
              outcome: ZLinkMessageFlowOutcome.Admitted,
              surface: ZLinkDispatchErrorSurface.StreamSession,
              messageKind: streamKind,
              packetName: inboundHeader.name,
              correlationId: streamCorr,
              sourceRid:
                this.context.routingId === undefined ? undefined : String(this.context.routingId)
            }
          );
          flowIfEnabled(
            this.options.dispatchErrors?.flow,
            ZLinkMessageFlowOutcome.Dispatched
          )?.trace({
            outcome: ZLinkMessageFlowOutcome.Dispatched,
            surface: ZLinkDispatchErrorSurface.StreamSession,
            messageKind: streamKind,
            packetName: inboundHeader.name,
            correlationId: streamCorr,
            sourceRid:
              this.context.routingId === undefined ? undefined : String(this.context.routingId)
          });
          if (session.onDispatch !== undefined) {
            const sessionDispatchContext = createSessionDispatchContext(
              inboundHeader,
              dispatchActor
            );
            const frameworkPayload = wrapFrameworkPayloadMessage(
              dispatchPayload,
              this.options.messageSerializers,
              streamCodecContentType(inboundHeader.codec),
              inboundHeader.name
            );
            releaseApplicationJobPermitBeforeHandler();
            await session.onDispatch(sessionDispatchContext, frameworkPayload);
          }
          if (streamKind === ZLinkDispatchMessageKind.Send) {
            flowIfEnabled(
              this.options.dispatchErrors?.flow,
              ZLinkMessageFlowOutcome.Completed
            )?.trace({
              outcome: ZLinkMessageFlowOutcome.Completed,
              surface: ZLinkDispatchErrorSurface.StreamSession,
              messageKind: streamKind,
              packetName: inboundHeader.name,
              correlationId: streamCorr,
              sourceRid:
                this.context.routingId === undefined ? undefined : String(this.context.routingId)
            });
          }
        }
      );
    } catch (error) {
      completeAdmission?.();
      if (this.stream.isShutdownForced()) return;
      this.options.dispatchErrors?.report({
        surface: ZLinkDispatchErrorSurface.StreamSession,
        messageKind:
          decodedHeader.kind === ZLinkStreamMessageKind.Request
            ? ZLinkDispatchMessageKind.Request
            : ZLinkDispatchMessageKind.Send,
        reason: ZLinkDispatchErrorReason.HandlerException,
        action:
          decodedHeader.requestSeq === undefined
            ? ZLinkDispatchErrorAction.Drop
            : ZLinkDispatchErrorAction.ReplyError,
        packetName: decodedHeader.name,
        sourceRid:
          this.context.routingId === undefined ? undefined : String(this.context.routingId),
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

  private scheduleLivenessCheck(): void {
    if (
      this.wakeReader === undefined ||
      this.disposed ||
      this.disconnected ||
      this.livenessTimer !== undefined
    ) {
      return;
    }
    const now = this.livenessClock.now();
    const due = Math.min(
      this.nextHeartbeatPingAt,
      this.readerLiveness.inboundDeadline(
        this.lastInboundFrameAt,
        this.liveness.heartbeatTimeoutMs
      ),
      this.readerLiveness.inboundDeadline(
        this.lastApplicationActivityAt,
        this.liveness.idleTimeoutMs
      )
    );
    this.livenessTimer = this.livenessClock.setTimer(
      () => {
        this.livenessTimer = undefined;
        this.wakeReader?.();
      },
      Math.max(0, due - now)
    );
  }

  checkLivenessAfterReceive(now: number): Promise<void> | undefined {
    if (this.livenessTimer !== undefined || this.disposed || this.disconnected || !this.connected) {
      return;
    }
    if (
      now >=
      this.readerLiveness.inboundDeadline(this.lastInboundFrameAt, this.liveness.heartbeatTimeoutMs)
    ) {
      return this.closeForLiveness(
        ZLinkStreamCloseReasonCode.HeartbeatTimeout,
        'heartbeat_timeout',
        'Heartbeat timeout.'
      );
    }
    if (
      now >=
      this.readerLiveness.inboundDeadline(
        this.lastApplicationActivityAt,
        this.liveness.idleTimeoutMs
      )
    ) {
      return this.closeForLiveness(
        ZLinkStreamCloseReasonCode.IdleTimeout,
        'idle_timeout',
        'Application idle timeout.'
      );
    }
    let submission: Promise<void> | undefined;
    if (now >= this.nextHeartbeatPingAt) {
      const interval = this.liveness.heartbeatIntervalMs;
      this.nextHeartbeatPingAt = now + (interval - ((now - this.nextHeartbeatPingAt) % interval));
      submission = this.stream.writeControl(ZLINK_STREAM_HEARTBEAT_PING);
    }
    this.scheduleLivenessCheck();
    return submission;
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
    if (this.stream.isShutdownForced()) return;
    const bindingsCleanup = this.context.cleanupBindings();
    void Promise.all([
      bindingsCleanup,
      this.serial.executeFinal(async () => this.complete(undefined, bindingsCleanup))
    ]).catch((error) => this.options.onError?.(error));
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
    try {
      const result = await this.context.submitReply(
        header,
        ZLinkStreamMessageKind.Error,
        new Map(),
        false,
        boundSessionErrorPayload(error)
      );
      requireOneWayCompletion(result, 'Client stream error reply');
      this.traceFrameWritten(ZLinkStreamMessageKind.Error, header.name, header.correlationId);
    } catch (replyError) {
      this.options.onError?.(replyError);
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
    const reply = kind === ZLinkStreamMessageKind.Response || kind === ZLinkStreamMessageKind.Error;
    const outcome = reply ? ZLinkMessageFlowOutcome.Replied : ZLinkMessageFlowOutcome.Sent;
    flowIfEnabled(this.options.dispatchErrors?.flow, outcome)?.trace({
      outcome,
      surface: ZLinkDispatchErrorSurface.StreamSession,
      messageKind:
        kind === ZLinkStreamMessageKind.Response
          ? ZLinkDispatchMessageKind.Response
          : kind === ZLinkStreamMessageKind.Error
            ? ZLinkDispatchMessageKind.Error
            : ZLinkDispatchMessageKind.Send,
      packetName: packetName.length === 0 ? undefined : packetName,
      correlationId,
      sourceRid: this.context.routingId === undefined ? undefined : String(this.context.routingId),
      streamSessionId: this.stream.sessionId
    });
  }

  private async complete(
    error: unknown,
    bindingsCleanup?: Promise<void>,
    disconnectReason?: ZLinkDisconnectReason
  ): Promise<void> {
    let creationFailed = false;
    try {
      if (error !== undefined && this.closeReason !== 'server_drain') {
        this.closeReason = 'transport_error';
      }
      const session = await this.session;
      creationFailed = session === undefined;
      if (session === undefined || this.stream.isShutdownForced()) return;
      if (error !== undefined) {
        await session.onError?.(this.context, {
          error: ZLinkStreamSessionError.TransportError,
          message: error instanceof Error ? error.message : String(error),
          ...(disconnectReason === undefined ? {} : { disconnectReason })
        });
        if (this.stream.isShutdownForced()) return;
      }

      relocationDebug('complete.onDisconnected.enter', { sessionId: this.stream.sessionId });
      let failure: { error: unknown } | undefined;
      try {
        await session.onDisconnected?.(this.context);
      } catch (error) {
        failure = { error };
        throw error;
      } finally {
        relocationDebugTerminal(
          'complete.onDisconnected.exit',
          { sessionId: this.stream.sessionId },
          failure
        );
      }
    } finally {
      if (!this.stream.isShutdownForced()) await this.cleanup(bindingsCleanup, creationFailed);
    }
  }

  private queueDisconnect(error: unknown, disconnectReason?: ZLinkDisconnectReason): void {
    if (this.stream.isShutdownForced() || this.disconnected || this.disconnectQueued) return;
    this.disconnectQueued = true;
    this.stopLivenessChecks();
    this.stream.markTransportClosed();
    const bindingsCleanup = this.context.cleanupBindings();
    void Promise.all([
      bindingsCleanup,
      this.serial.executeFinal(async () => {
        this.disconnectQueued = false;
        if (this.disconnected) return;
        this.disconnected = true;
        await this.complete(error, bindingsCleanup, disconnectReason);
      })
    ]).catch((failure) => this.options.onError?.(failure));
  }

  private async cleanup(bindingsCleanup?: Promise<void>, creationFailed = false): Promise<void> {
    if (isRelocationDebugEnabled())
      relocationDebug('session.cleanup.enter', { ...this.shutdownSnapshot() });
    let failure: { error: unknown } | undefined;
    try {
      if (this.connected && !this.metricsClosed) {
        this.metricsClosed = true;
        this.options.metrics?.change(METRIC_NAMES.StreamConnectionsActive, -1, {
          transport: 'tcp'
        });
        this.options.metrics?.count(METRIC_NAMES.StreamConnectionsClosed, 1, {
          transport: 'tcp',
          close_reason: normalizeStreamCloseReason(this.closeReason)
        });
      }
      await (bindingsCleanup ?? this.context.cleanupBindings());
      this.removeSession(this.stream.sessionId, this, creationFailed);
    } catch (error) {
      failure = { error };
      throw error;
    } finally {
      if (isRelocationDebugEnabled())
        relocationDebugTerminal('session.cleanup.exit', { ...this.shutdownSnapshot() }, failure);
    }
  }

  private enqueueControl(
    work: () => Promise<void>,
    onRejected?: () => void,
    onShutdownRejected?: () => void
  ): void {
    if (
      !this.serial.executeControl(work, {}, (error) => {
        if (isAbortError(error) && this.stream.isShutdownForced()) onShutdownRejected?.();
        onRejected?.();
      })
    ) {
      // A closed serial lane rejects new work before submission.
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
    if (
      !this.serial.executeApplication(
        async () => {
          try {
            await work();
          } finally {
            claim?.close();
          }
        },
        {},
        (error) => {
          claim?.close();
          if (isAbortError(error) && this.stream.isShutdownForced()) onShutdownRejected?.();
          onRejected?.();
        }
      )
    ) {
      claim?.close();
      // A closed serial lane rejects new work before submission.
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
      messageKind: streamDispatchMessageKind(header.kind),
      packetName: header.name === '' ? undefined : header.name,
      correlationId: header.correlationId,
      sourceRid: this.context.routingId === undefined ? undefined : String(this.context.routingId),
      streamSessionId: this.stream.sessionId,
      flowId: flowEnabled ? header.flowId : undefined,
      flowOrigin: flowEnabled ? header.flowOrigin : undefined,
      errorReason: ZLinkDispatchErrorReason.Shutdown
    });
  }
}

function streamDispatchMessageKind(kind: ZLinkStreamMessageKind): ZLinkDispatchMessageKind {
  switch (kind) {
    case ZLinkStreamMessageKind.Request:
      return ZLinkDispatchMessageKind.Request;
    case ZLinkStreamMessageKind.Response:
      return ZLinkDispatchMessageKind.Response;
    case ZLinkStreamMessageKind.Error:
      return ZLinkDispatchMessageKind.Error;
    case ZLinkStreamMessageKind.Control:
      return ZLinkDispatchMessageKind.Control;
    default:
      return ZLinkDispatchMessageKind.Send;
  }
}

interface StreamConnectionMetadata {
  readonly localAddr?: string;
  readonly remoteAddr?: string;
  readonly connectedAt: number;
}

export class ZLinkStreamSessionNodeRuntime {
  // PACKET readiness identifies the listener. recvPacket identifies the peer.
  private readonly readerLiveness = new ZLinkStreamReaderLiveness();
  // A rejected routing ID occupies its admission slot until bounded retirement.
  private readonly sessions = new Map<string, ZLinkStreamSessionRuntime | null>();
  private readonly availablePackets: ZLinkBackendStreamPacket[] = [];
  private readonly pendingConnectionMetadata: Array<StreamConnectionMetadata | undefined> = [];
  private pendingConnectionMetadataHead = 0;
  private pendingConnectionMetadataCount = 0;
  private readonly unaddressedMonitorSessions: Array<string | undefined> = [];
  private unaddressedMonitorSessionHead = 0;
  private unaddressedMonitorSessionCount = 0;
  private pendingEndpointlessDisconnect:
    | {
        readonly session: ZLinkStreamSessionRuntime;
        cancelled: boolean;
        readonly error: Error;
        readonly disconnectReason: ZLinkDisconnectReason;
        readonly activityVersion: number;
      }
    | undefined;
  private activityVersion = 0;
  private stopped = false;
  private readonly receiveAbortController = new AbortController();
  private receiveLoop: Promise<void> | undefined;
  private monitorLoop: Promise<void> | undefined;
  private livenessWake = false;
  constructor(
    private readonly options: ZLinkStreamSessionNodeRuntimeOptions & ZLinkStreamLivenessOptions
  ) {}

  start(): void {
    if (this.stopped || this.receiveLoop !== undefined) {
      return;
    }
    const monitor = this.options.monitor;
    monitor?.onEvent((event) => this.onMonitorEvent(event));
    const running = this.runReceiveLoop(this.receiveAbortController.signal).catch((error) => {
      if (!this.stopped) {
        this.options.onError?.(error);
      }
    });
    this.receiveLoop = running;
    if (monitor !== undefined) {
      this.monitorLoop = this.runMonitorLoop(monitor, this.receiveAbortController.signal).catch(
        (error) => {
          if (!this.stopped) {
            this.options.onError?.(error);
          }
        }
      );
    }
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
    return this.sessions.get(streamSessionIdFromRoutingId(routingId)) ?? undefined;
  }

  async dispose(shutdownSignal?: AbortSignal): Promise<void> {
    this.stopped = true;
    this.receiveAbortController.abort();
    this.wakeReceiveLoop();
    try {
      await this.receiveLoop;
      await this.monitorLoop;
      const sessions = [...this.liveSessions()];
      this.sessions.clear();
      for (const packet of this.availablePackets.splice(0)) packet.close();
      this.unaddressedMonitorSessions.length = 0;
      this.unaddressedMonitorSessionHead = 0;
      this.unaddressedMonitorSessionCount = 0;
      this.pendingConnectionMetadata.length = 0;
      this.pendingConnectionMetadataHead = 0;
      this.pendingConnectionMetadataCount = 0;
      for (const session of sessions) {
        await session.dispose(shutdownSignal);
      }
    } finally {
      this.options.readablePoller.dispose();
    }
  }

  shutdownSnapshot(): readonly Record<string, unknown>[] {
    return [...this.liveSessions()].map((session) => session.shutdownSnapshot());
  }

  async drainCloseSessions(shutdownSignal?: AbortSignal): Promise<void> {
    if (isRelocationDebugEnabled())
      relocationDebug('shutdown.sessions.enter', { sessions: this.shutdownSnapshot() });
    let failure: { error: unknown } | undefined;
    try {
      await Promise.allSettled(
        [...this.liveSessions()].map((session) => session.drainClose(shutdownSignal))
      );
    } catch (error) {
      failure = { error };
      throw error;
    } finally {
      if (isRelocationDebugEnabled())
        relocationDebugTerminal(
          'shutdown.sessions.exit',
          { sessions: this.shutdownSnapshot() },
          failure
        );
    }
  }

  // Spec server 04-session §2/§7: a session exists once the handshake succeeds,
  // and connect/disconnect callbacks are its base surface. A stream node learns
  // of both from the socket monitor, which is a source of its own with no
  // event-loop readiness callback. Draining it from the receive loop would tie
  // a connection event to inbound traffic, so a session that never sends - a
  // push-only console - would stay unknown to the node until it happened to
  // write something. The .NET runtime already drains the monitor in its own
  // loop (ZLinkStreamNodeRuntime.RunMonitorLoopAsync); this is the same rule.
  private async runMonitorLoop(
    monitor: ZLinkBackendSocketMonitor,
    signal: AbortSignal
  ): Promise<void> {
    let misses = 0;
    while (!this.isReceiveStopped(signal)) {
      if (monitor.drain() > 0) {
        misses = 0;
        // Yield so a burst of monitor events cannot starve the event loop.
        await this.monitorIdleDelay(0, signal);
        continue;
      }
      misses = Math.min(misses + 1, ZLINK_STREAM_MONITOR_IDLE_MAX_STEPS);
      await this.monitorIdleDelay(monitorIdleDelayMs(misses), signal);
    }
  }

  private monitorIdleDelay(delayMs: number, signal: AbortSignal): Promise<void> {
    if (signal.aborted) return Promise.resolve();
    return new Promise((resolve) => {
      const onAbort = (): void => {
        clearTimeout(timer);
        resolve();
      };
      const timer = setTimeout(() => {
        signal.removeEventListener('abort', onAbort);
        resolve();
      }, delayMs);
      // The bound socket already holds the event loop open for this node. The
      // idle poll must not become a second reason for a process to stay alive.
      timer.unref();
      signal.addEventListener('abort', onAbort, { once: true });
    });
  }

  private async handleControl(
    routingId: unknown,
    header: ZLinkStreamFrameHeader,
    payload: Message
  ): Promise<void> {
    const session = this.sessions.get(streamSessionIdFromRoutingId(routingId));
    if (session === null || session?.isCreationRejected === true) return;
    session?.recordInbound();
    const stream =
      session?.stream ??
      new ZLinkManagedStream(
        this.options.socket,
        routingId,
        this.options.messageSerializers,
        this.options.nativeSessionService,
        this.options.meshCompletions,
        undefined,
        this.options.nativeSessionRouteForMesh
      );
    const payloadBytes = messageToBytes(payload);
    if (header.name === ZLINK_STREAM_HEARTBEAT_PING) {
      if (payloadBytes.length !== 0) {
        this.logIgnoredControl(stream, header.name, payloadBytes.length);
      }
      await stream.writeControl(ZLINK_STREAM_HEARTBEAT_PONG);
      return;
    }
    if (header.name === ZLINK_STREAM_HEARTBEAT_PONG) {
      if (payloadBytes.length !== 0) {
        this.logIgnoredControl(stream, header.name, payloadBytes.length);
      }
      return;
    }
    if (
      header.name === ZlinkStreamControlPacket.ActorBound ||
      header.name === ZlinkStreamControlPacket.ActorUnbound
    ) {
      await stream.closeForReason(
        ZLinkStreamCloseReasonCode.ProtocolError,
        `Unexpected stream actor control frame: ${header.name}`
      );
      session?.enqueueDisconnected(undefined, 'protocol_error');
      return;
    }
    if (header.name.startsWith('$zlink.')) {
      this.logIgnoredControl(stream, header.name, payloadBytes.length);
      return;
    }
    await stream.closeForReason(
      ZLinkStreamCloseReasonCode.ProtocolError,
      `Unknown stream control frame: ${header.name}`
    );
    session?.enqueueDisconnected(undefined, 'protocol_error');
  }

  private logIgnoredControl(stream: ZLinkManagedStream, name: string, payloadLength: number): void {
    STREAM_RUNTIME_LOGGER.emit({
      eventName: 'zlink.stream.control_ignored',
      timestamp: new Date(),
      severityNumber: SeverityNumber.DEBUG,
      severityText: 'DEBUG',
      body: `Ignored STREAM control name=${name} payloadBytes=${payloadLength} peerSessionRid=${stream.sessionId}`,
      attributes: {
        control_name: name,
        payload_bytes: payloadLength,
        peer_session_rid: stream.sessionId
      }
    });
  }

  private async runReceiveLoop(signal: AbortSignal): Promise<void> {
    const batch = new ZLinkReceiveBatchBudget();
    while (!this.isReceiveStopped(signal)) {
      let permit: ApplicationJobPermitPort | undefined;
      let packet: ZLinkBackendStreamPacket | undefined;
      let packetTransferred = false;
      let bytes: number;
      try {
        // A timer may wake the reader before the binding's readable callback runs.
        if (!this.options.readablePoller.wait(0, this.livenessWake)) {
          this.finishReceiveRound();
          await this.waitReceiveLoopWake(signal);
          batch.reset();
          continue;
        }
        const acquiring = this.readerLiveness.acquire(
          this.options.applicationJobQueue,
          signal,
          this.options.livenessClock ?? systemLivenessClock
        );
        if (this.readerLiveness.isWaitingForPermit()) {
          const wake = () => this.wakeReceiveLoop();
          void acquiring.then(wake, wake);
          while (this.readerLiveness.isWaitingForPermit() && !this.isReceiveStopped(signal)) {
            // Permit wait suspends both inbound deadlines. The reader can still send pings.
            this.finishReceiveRound();
            await this.options.readablePoller.waitForReadable(signal, true);
          }
          batch.reset();
        }
        permit = await acquiring;
        if (this.isReceiveStopped(signal)) {
          permit.releaseAfterInternalProcessing();
          break;
        }
        packet = this.takePacket();
        if (!this.options.socket.recvPacket(packet, ZLINK_RECV_DONT_WAIT)) {
          this.options.readablePoller.markDrained();
          permit.releaseAfterInternalProcessing();
          this.recyclePacket(packet);
          this.finishReceiveRound();
          await this.waitReceiveLoopWake(signal);
          batch.reset();
          continue;
        }
        packetTransferred = true;
        bytes = this.acceptPacket(packet, permit);
        packet = undefined;
        permit = undefined;
      } catch (error) {
        if (!packetTransferred) permit?.releaseAfterInternalProcessing();
        if (packet !== undefined && !packetTransferred) this.recyclePacket(packet);
        if (!this.isReceiveStopped(signal)) {
          if (!packetTransferred) this.options.onError?.(error);
          this.finishReceiveRound();
          await batch.yieldAndReset();
        }
        continue;
      }

      if (batch.record(bytes) && !this.isReceiveStopped(signal)) {
        this.finishReceiveRound();
        await batch.yieldAndReset();
      }
    }
  }

  private finishReceiveRound(): void {
    if (!this.livenessWake) return;
    this.livenessWake = false;
    const now = (this.options.livenessClock ?? systemLivenessClock).now();
    for (const session of this.liveSessions()) {
      void session.checkLivenessAfterReceive(now)?.catch((error) => this.options.onError?.(error));
    }
  }

  private acceptPacket(packet: ZLinkBackendStreamPacket, permit: ApplicationJobPermitPort): number {
    const routingId = packet.routingId;
    const header = packet.header;
    const body = packet.body;
    let releasePermitOnFailure = true;
    let packetTransferred = false;
    let payload: Message | undefined;
    let releaseAuthentication: (() => void) | undefined;
    let session: ZLinkStreamSessionRuntime | undefined;
    try {
      if (routingId === null || header === null || body === null) {
        throw new Error('STREAM packet did not provide routing id, header, and body.');
      }

      const bytes = header.size() + body.size();
      this.activityVersion += 1;
      let decodedHeader: ZLinkStreamFrameHeader;
      try {
        decodedHeader = decodeStreamHeader(
          header.data(),
          this.options.dispatchErrors?.flow.flowCreationEnabled() ?? true
        );
      } catch (error) {
        session = this.getOrCreateSession(routingId);
        throw error;
      }
      const admission = this.options.authenticationAdmission;
      if (admission?.isAuthentication(decodedHeader) === true) {
        releaseAuthentication = admission.tryAcquire();
        if (releaseAuthentication === undefined) {
          this.findSession(routingId)?.recordInbound();
          permit.releaseAfterInternalProcessing();
          releasePermitOnFailure = false;
          void admission
            .replyRejected(this.options.socket, routingId, decodedHeader)
            .catch((error) => {
              this.options.onError?.(error);
            });
          return bytes;
        }
      }
      payload = ownedMessage(body.data());
      if (decodedHeader.kind === ZLinkStreamMessageKind.Control) {
        const controlPayload = payload;
        void this.handleControl(routingId, decodedHeader, controlPayload)
          .catch((error) => this.options.onError?.(error))
          .finally(() => {
            permit.releaseAfterInternalProcessing();
            controlPayload.close();
            this.recyclePacket(packet);
          });
        payload = undefined;
        releasePermitOnFailure = false;
        packetTransferred = true;
        return bytes;
      }
      session = this.getOrCreateSession(routingId, decodedHeader);
      if (session === undefined || session.stream.shutdownTransportClosed) return bytes;
      this.applyPendingConnectionMetadata(session);
      const terminalCompletion =
        decodedHeader.kind === ZLinkStreamMessageKind.Response ||
        decodedHeader.kind === ZLinkStreamMessageKind.Error;
      const applicationPermit = terminalCompletion ? undefined : permit;
      if (terminalCompletion) {
        permit.releaseAfterInternalProcessing();
        releasePermitOnFailure = false;
      } else {
        if (
          decodedHeader.kind === ZLinkStreamMessageKind.Send ||
          decodedHeader.kind === ZLinkStreamMessageKind.Request
        ) {
          permit.markApplicationQueued();
        }
      }
      const owner: ZLinkRetainedStreamOwner = {
        close: () => this.recyclePacket(packet)
      };
      session.enqueuePacket(
        payload,
        decodedHeader,
        owner,
        applicationPermit,
        releaseAuthentication
      );
      releaseAuthentication = undefined;
      releasePermitOnFailure = false;
      payload = undefined;
      packetTransferred = true;
      return bytes;
    } catch (error) {
      const failure = error instanceof Error ? error : new Error(String(error));
      if (routingId !== null) session ??= this.findSession(routingId);
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
        releaseAuthentication?.();
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

  private handleMalformedPacket(
    routingId: unknown,
    session: ZLinkStreamSessionRuntime,
    error: Error
  ): void {
    if (session.stream.shutdownTransportClosed) return;
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
      case ZLinkSocketNativeEventType.ConnectionReady: {
        if (!event.readyEdge) return;
        const connectedAt = (this.options.livenessClock ?? systemLivenessClock).now();
        this.activityVersion += 1;
        if (event.routingId === undefined) {
          const endpointKey = streamMonitorEndpointKey(event.localAddr, event.remoteAddr);
          const unaddressed = this.firstUnaddressedSession();
          if (unaddressed !== undefined) {
            unaddressed.enqueueConnected(event.localAddr, event.remoteAddr, connectedAt);
            this.enqueueUnaddressedMonitorSession(unaddressed.stream.sessionId);
            return;
          }
          if (this.hasPendingConnectionMetadata(endpointKey)) {
            return;
          }
          this.pendingConnectionMetadata.push({
            localAddr: event.localAddr,
            remoteAddr: event.remoteAddr,
            connectedAt
          });
          this.pendingConnectionMetadataCount += 1;
          return;
        }
        this.getOrCreateSession(event.routingId)?.enqueueConnected(
          event.localAddr,
          event.remoteAddr,
          connectedAt
        );
        return;
      }
      case ZLinkSocketNativeEventType.Disconnected:
        {
          const endpointKey = streamMonitorEndpointKey(event.localAddr, event.remoteAddr);
          this.removePendingConnectionMetadata(endpointKey);
          const session = this.resolveMonitorSession(event);
          const error = new Error(`Stream disconnected: ${event.nativeEvent}/${event.value}`);
          const disconnectReason = Number(event.value) as ZLinkDisconnectReason;
          if (session !== undefined) {
            session.enqueueDisconnected(error, undefined, disconnectReason);
            return;
          }
          if (event.routingId === undefined && !streamMonitorHasEndpoint(event)) {
            this.enqueueEndpointlessDisconnect(error, disconnectReason);
          }
        }
        return;
      default:
        return;
    }
  }

  private applyPendingConnectionMetadata(session: ZLinkStreamSessionRuntime): void {
    if (
      session.stream.localAddr !== undefined ||
      session.stream.remoteAddr !== undefined ||
      this.pendingConnectionMetadataCount === 0
    ) {
      return;
    }
    const metadata = this.takePendingConnectionMetadata();
    if (metadata === undefined) return;
    this.removePendingConnectionMetadata(
      streamMonitorEndpointKey(metadata.localAddr, metadata.remoteAddr)
    );
    session.enqueueConnected(metadata.localAddr, metadata.remoteAddr, metadata.connectedAt);
    this.enqueueUnaddressedMonitorSession(session.stream.sessionId);
  }

  private hasPendingConnectionMetadata(endpointKey: string): boolean {
    for (
      let index = this.pendingConnectionMetadataHead;
      index < this.pendingConnectionMetadata.length;
      index += 1
    ) {
      const metadata = this.pendingConnectionMetadata[index];
      if (
        metadata !== undefined &&
        streamMonitorEndpointKey(metadata.localAddr, metadata.remoteAddr) === endpointKey
      ) {
        return true;
      }
    }
    return false;
  }

  private takePendingConnectionMetadata(): StreamConnectionMetadata | undefined {
    while (
      this.pendingConnectionMetadataHead < this.pendingConnectionMetadata.length &&
      this.pendingConnectionMetadata[this.pendingConnectionMetadataHead] === undefined
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
    } else if (
      shouldCompactBackingArray(
        this.pendingConnectionMetadataHead,
        this.pendingConnectionMetadata.length
      )
    ) {
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
      this.unaddressedMonitorSessionHead < this.unaddressedMonitorSessions.length &&
      this.unaddressedMonitorSessions[this.unaddressedMonitorSessionHead] === undefined
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
    } else if (
      shouldCompactBackingArray(
        this.unaddressedMonitorSessionHead,
        this.unaddressedMonitorSessions.length
      )
    ) {
      this.unaddressedMonitorSessions.splice(0, this.unaddressedMonitorSessionHead);
      this.unaddressedMonitorSessionHead = 0;
    }
    return sessionId;
  }

  private firstUnaddressedSession(): ZLinkStreamSessionRuntime | undefined {
    for (const session of this.liveSessions()) {
      if (
        !session.isDisconnected &&
        session.stream.localAddr === undefined &&
        session.stream.remoteAddr === undefined
      ) {
        return session;
      }
    }
    return undefined;
  }

  private getActiveSession(sessionId: string): ZLinkStreamSessionRuntime | undefined {
    const session = this.sessions.get(sessionId);
    if (session == null || session.isDisconnected) {
      return undefined;
    }
    return session;
  }

  private *liveSessions(): Iterable<ZLinkStreamSessionRuntime> {
    for (const session of this.sessions.values()) {
      if (session !== null) yield session;
    }
  }

  private removePendingConnectionMetadata(endpointKey: string): void {
    for (
      let index = this.pendingConnectionMetadataHead;
      index < this.pendingConnectionMetadata.length;
      index += 1
    ) {
      const metadata = this.pendingConnectionMetadata[index];
      if (metadata === undefined) continue;
      if (streamMonitorEndpointKey(metadata.localAddr, metadata.remoteAddr) === endpointKey) {
        this.pendingConnectionMetadata[index] = undefined;
        this.pendingConnectionMetadataCount -= 1;
      }
    }
    this.compactPendingConnectionMetadata();
  }

  private resolveMonitorSession(
    event: ZLinkBackendSocketMonitorEvent
  ): ZLinkStreamSessionRuntime | undefined {
    if (event.routingId !== undefined) {
      const session = this.getActiveSession(streamSessionIdFromRoutingId(event.routingId));
      if (session !== undefined) {
        return session;
      }
    }
    if (streamMonitorHasEndpoint(event)) {
      let match: ZLinkStreamSessionRuntime | undefined;
      let matchCount = 0;
      for (const session of this.liveSessions()) {
        if (
          session.isDisconnected ||
          session.stream.localAddr !== event.localAddr ||
          session.stream.remoteAddr !== event.remoteAddr
        ) {
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

  private enqueueEndpointlessDisconnect(
    error: Error,
    disconnectReason: ZLinkDisconnectReason
  ): void {
    const sessionId = this.takeUnaddressedMonitorSession();
    if (sessionId !== undefined) {
      this.getActiveSession(sessionId)?.enqueueDisconnected(error, undefined, disconnectReason);
      return;
    }
    let session: ZLinkStreamSessionRuntime | undefined;
    let activeCount = 0;
    for (const candidate of this.liveSessions()) {
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
      disconnectReason,
      activityVersion: this.activityVersion
    };
    this.pendingEndpointlessDisconnect = pending;
    // A pending endpointless disconnect must yield to a recv that is already
    // queued. Wake the recv loop instead of waiting for its idle backoff.
    this.wakeReceiveLoop();
    setImmediate(() => {
      if (
        pending.cancelled ||
        this.stopped ||
        this.pendingEndpointlessDisconnect !== pending ||
        this.activityVersion !== pending.activityVersion ||
        this.sessions.get(pending.session.stream.sessionId) !== pending.session
      ) {
        return;
      }
      this.pendingEndpointlessDisconnect = undefined;
      pending.session.enqueueDisconnected(pending.error, undefined, pending.disconnectReason);
    });
  }

  private waitReceiveLoopWake(signal: AbortSignal): Promise<boolean> {
    return this.options.readablePoller.waitForReadable(signal);
  }

  private wakeReceiveLoop(): void {
    this.options.readablePoller.wake();
  }

  private getOrCreateSession(
    routingId: unknown,
    triggeringHeader?: ZLinkStreamFrameHeader
  ): ZLinkStreamSessionRuntime | undefined {
    const sessionId = streamSessionIdFromRoutingId(routingId);
    const existing = this.sessions.get(sessionId);
    if (existing !== undefined) {
      return existing ?? undefined;
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
      (sessionId, session, creationFailed) =>
        this.retireAdmission(sessionId, session, creationFailed),
      { triggeringHeader },
      this.readerLiveness,
      () => {
        this.livenessWake = true;
        this.wakeReceiveLoop();
      }
    );
    this.sessions.set(sessionId, created);
    return created;
  }

  private retireAdmission(
    sessionId: string,
    session: ZLinkStreamSessionRuntime,
    creationFailed: boolean
  ): void {
    if (this.sessions.get(sessionId) !== session) return;
    this.sessions.delete(sessionId);
    if (!creationFailed) return;

    // Keep failed attempts after close because ready events and packets can
    // already have arrived at their separate Core observation boundaries.
    this.sessions.set(sessionId, null);
    if (this.sessions.size <= ZLINK_REJECTED_STREAM_ADMISSION_LIMIT) return;

    let oldestRejection: string | undefined;
    let rejections = 0;
    for (const [id, entry] of this.sessions) {
      if (entry !== null) continue;
      oldestRejection ??= id;
      if (++rejections > ZLINK_REJECTED_STREAM_ADMISSION_LIMIT) {
        this.sessions.delete(oldestRejection);
        return;
      }
    }
  }
}

function monitorIdleDelayMs(misses: number): number {
  const scaled = ZLINK_STREAM_MONITOR_IDLE_MIN_DELAY_MS * 2 ** (misses - 1);
  return Math.min(scaled, ZLINK_STREAM_MONITOR_IDLE_MAX_DELAY_MS);
}

function streamMonitorEndpointKey(
  localAddr: string | undefined,
  remoteAddr: string | undefined
): string {
  return `${localAddr ?? ''}\n${remoteAddr ?? ''}`;
}

function streamMonitorHasEndpoint(event: ZLinkBackendSocketMonitorEvent): boolean {
  const endpoint = event as { readonly localAddr?: string; readonly remoteAddr?: string };
  return endpoint.localAddr !== undefined || endpoint.remoteAddr !== undefined;
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
