import {
  ZLinkFrameworkInternalErrorKind,
  createInternalFrameworkException,
  internalFrameworkErrorKind
} from '../framework-errors-internal';
import type {
  ActorRef,
  RoutingId,
  ZLinkMessage,
  ZLinkMessageSerializer,
  ZLinkStream
} from '../../contracts';
import { ZLinkFrameworkException } from '../../contracts';
import {
  ZLinkSubmitStatus,
  type ZLinkSubmitResult
} from '../messaging/submission-result';
import type { Message } from '../../contracts/Common/Message';
import { throwIfAborted } from '../abort';
import { encodeFrameworkPayloadMessage } from '../messaging/payload-codec';
import type {
  ZLinkBackendActorRef,
  ZLinkBackendSendFlags,
  ZLinkBackendStreamSocket
} from '../backend/contracts';
import { RequestResult } from '../backend/runtime-values';
import { ZLinkBufferMessage as NativeMessage } from '../backend/runtime-message';
import type { StreamSessionService } from '../foundation/service-runtime-contracts';
import {
  closeMeshCompletion,
  type ZLinkMeshCompletionTable
} from '../backend/mesh-completion-table';
import {
  encodeSessionClosingFrame,
  encodeStreamControlFrame,
  ZLinkStreamCloseReasonCode
} from './protocol';
import { ZLinkAsyncSubmitter } from '../messaging';

const ZLINK_SEND_DONT_WAIT = 1;

export interface ZLinkNativeSessionRoute {
  readonly service: StreamSessionService;
  readonly completions: ZLinkMeshCompletionTable;
}

export class ZLinkManagedStream implements ZLinkStream {
  private currentLocalAddr: string | undefined;
  private currentRemoteAddr: string | undefined;
  private readonly nativeActorBindings = new Map<string, {
    readonly actor: ZLinkBackendActorRef;
    readonly bindingGeneration: bigint;
    readonly route?: ZLinkNativeSessionRoute;
  }>();
  private readonly submitter: ZLinkAsyncSubmitter;

  constructor(
    private readonly socket: ZLinkBackendStreamSocket,
    private readonly backendSessionRoutingId: unknown,
    private readonly messageSerializers?: ReadonlyMap<string, ZLinkMessageSerializer>,
    private readonly nativeSessionService?: StreamSessionService,
    private readonly meshCompletions?: ZLinkMeshCompletionTable,
    submitter?: ZLinkAsyncSubmitter,
    private readonly publicSessionId = streamSessionIdFromRoutingId(backendSessionRoutingId),
    private readonly nativeSessionRouteForMesh?: (
      meshName: string
    ) => ZLinkNativeSessionRoute | undefined
  ) {
    this.submitter = submitter ?? new ZLinkAsyncSubmitter(
      (handler) => socket.onSendReady(handler),
      {
        timeoutMs: socket.sendTimeoutMs > 0 ? socket.sendTimeoutMs : 1000,
        capacity: Math.max(1, socket.sendHighWaterMark)
      }
    );
  }

  get sessionId(): string {
    return this.publicSessionId;
  }

  get routingId(): RoutingId {
    return this.publicSessionId;
  }

  get actorBindingRoutingId(): RoutingId {
    return this.backendSessionRoutingId as RoutingId;
  }

  actorBindingGeneration(actorId: string): bigint | undefined {
    return this.nativeActorBindings.get(actorId)?.bindingGeneration;
  }

  get localAddr(): string | undefined {
    return this.currentLocalAddr;
  }

  get remoteAddr(): string | undefined {
    return this.currentRemoteAddr;
  }

  write(payload: ZLinkMessage, flags?: ZLinkBackendSendFlags): boolean {
    const message = encodeFrameworkPayloadMessage(payload, this.messageSerializers);
    try {
      return this.socket.send(this.backendRoutingId(), message, flags ?? 0);
    } finally {
      message.close();
    }
  }

  writeRaw(payload: Message, flags?: ZLinkBackendSendFlags): boolean {
    return this.socket.send(this.backendRoutingId(), payload, flags ?? 0);
  }

  async submitRaw(payload: Message, signal?: AbortSignal): Promise<ZLinkSubmitResult> {
    return this.submitOperation(
      () => this.socket.send(this.backendRoutingId(), payload, ZLINK_SEND_DONT_WAIT),
      signal
    );
  }

  async submitBoundActor(
    actorId: string,
    parts: readonly Message[],
    signal?: AbortSignal
  ): Promise<ZLinkSubmitResult> {
    return this.submitOperation(
      () => this.sendBoundActor(actorId, parts, ZLINK_SEND_DONT_WAIT),
      signal
    );
  }

  private async submitOperation(
    attempt: () => boolean,
    signal?: AbortSignal
  ): Promise<ZLinkSubmitResult> {
    try {
      await this.submitter.submitCommand(attempt, signal);
      return { status: ZLinkSubmitStatus.Submitted };
    } catch (error) {
      if (error instanceof ZLinkFrameworkException) {
        switch (internalFrameworkErrorKind(error)) {
          case ZLinkFrameworkInternalErrorKind.DeadlineExceeded:
          case ZLinkFrameworkInternalErrorKind.WorkerTimedOut:
            return { status: ZLinkSubmitStatus.TimedOut };
          case ZLinkFrameworkInternalErrorKind.WorkerQueueFull:
            return { status: ZLinkSubmitStatus.Backpressured };
          case ZLinkFrameworkInternalErrorKind.RuntimeShutdown:
            return { status: ZLinkSubmitStatus.Shutdown };
        }
      }
      throw error;
    }
  }

  async close(signal?: AbortSignal): Promise<void> {
    throwIfAborted(signal);
    this.socket.disconnectPeer(this.backendRoutingId());
  }

  async closeForDrain(signal?: AbortSignal): Promise<void> {
    await this.closeForReason(ZLinkStreamCloseReasonCode.ServerDrain, 'server drain', signal);
  }

  writeControl(name: string): boolean {
    const control = NativeMessage.from(encodeStreamControlFrame(name));
    try {
      return this.socket.send(this.backendRoutingId(), control, 0);
    } finally {
      control.close();
    }
  }

  async closeForReason(
    reason: ZLinkStreamCloseReasonCode,
    diagnostic: string,
    signal?: AbortSignal
  ): Promise<void> {
    throwIfAborted(signal);
    const closing = NativeMessage.from(encodeSessionClosingFrame(diagnostic, reason));
    try {
      this.socket.send(this.backendRoutingId(), closing, 0);
    } finally {
      closing.close();
    }
    this.socket.disconnectPeer(this.backendRoutingId());
  }

  async bindActor(actor: ActorRef, timeoutMs: number, signal?: AbortSignal): Promise<void> {
    throwIfAborted(signal);
    const route = this.nativeRoute(actor.meshName);
    if (route !== undefined) {
      if (route.service.status().state === 1) {
        route.service.start();
      }
      const nativeActor = toNativeActorRef(actor);
      await this.ensureNativeActorRoute(route, actor, timeoutMs, signal);
      const operation = await this.submitNativeSessionBind(route.service, nativeActor, timeoutMs, signal);
      await this.requireSuccessfulCompletion(
        route.completions,
        operation,
        `Actor '${actor.actorId}' native session bind`,
        signal
      );
      const binding = route.service.bindings(this.backendRoutingId())
        .find((candidate) =>
          candidate.actor.actorId === nativeActor.actorId
          && candidate.actor.generation === nativeActor.generation);
      if (binding === undefined) {
        throw createInternalFrameworkException(
          ZLinkFrameworkInternalErrorKind.RouteNotConnected,
          `Actor '${actor.actorId}' native session bind completed without a binding snapshot.`
        );
      }
      this.nativeActorBindings.set(actor.actorId, {
        actor: nativeActor,
        bindingGeneration: binding.bindingGeneration,
        route
      });
      return;
    }
    await this.socket.bindActor(this.backendRoutingId(), toBackendActorRef(actor), timeoutMs, signal);
  }

  private async ensureNativeActorRoute(
    route: ZLinkNativeSessionRoute,
    actor: ActorRef,
    timeoutMs: number,
    signal?: AbortSignal
  ): Promise<void> {
    const operation = route.service.lookupActor(
      actor.nodeRid,
      actor.actorId,
      timeoutMs
    );
    const completion = await route.completions.wait(operation, signal);
    try {
      const resolved = completion.kindData?.kind === 'actorLookupCompletion'
        ? completion.kindData.location.actor
        : undefined;
      if (
        completion.terminalResult !== 0
        || completion.failureErrno !== 0
        || resolved === undefined
        || resolved.actorId !== actor.actorId
        || resolved.generation !== actor.objectGeneration
        || String(resolved.nodeRid) !== String(actor.nodeRid)
      ) {
        throw createInternalFrameworkException(
          ZLinkFrameworkInternalErrorKind.ActorRouteNotFound,
          `Actor '${actor.actorId}' native route fence does not match its ActorRef `
          + `(expected ${String(actor.nodeRid)}/${actor.objectGeneration}, `
          + `resolved ${resolved === undefined ? 'none' : `${String(resolved.nodeRid)}/${resolved.generation}`}, `
          + `terminal=${completion.terminalResult}, failure=${completion.failureErrno}).`
        );
      }
    } finally {
      closeMeshCompletion(completion);
    }
  }

  async unbindActor(actorId: string, timeoutMs: number, signal?: AbortSignal): Promise<void> {
    throwIfAborted(signal);
    const binding = this.nativeActorBindings.get(actorId);
    if (binding?.route !== undefined) {
      const operation = binding.route.service.unbindActor(
        this.backendRoutingId(),
        binding.actor as never,
        binding.bindingGeneration,
        timeoutMs
      );
      await this.requireSuccessfulCompletion(
        binding.route.completions,
        operation,
        `Actor '${actorId}' native session unbind`,
        signal,
        // Destroy can remove the actor registry entry before the old
        // session owner processes its cleanup. Both a disconnected route and
        // that stale actor route are idempotent unbind outcomes.
        new Set([RequestResult.NotConnected, RequestResult.NotFound])
      );
      this.nativeActorBindings.delete(actorId);
      return;
    }
    await this.socket.unbindActor(this.backendRoutingId(), actorId, timeoutMs, signal);
  }

  sendBoundActor(actorId: string, parts: readonly Message[], flags?: ZLinkBackendSendFlags): boolean {
    const binding = this.nativeActorBindings.get(actorId);
    if (binding?.route !== undefined) {
      const nativeParts = parts.map((part) => NativeMessage.from(Buffer.from(part.data())));
      try {
        return binding.route.service.sendToActor(
          this.backendRoutingId(),
          binding.actor as never,
          nativeParts,
          { flags: flags ?? 0 }
        ) === 0;
      } finally {
        for (const part of nativeParts) {
          part.close();
        }
      }
    }
    return this.socket.sendBoundActor(this.backendRoutingId(), actorId, parts, flags ?? 0);
  }

  updateAddresses(localAddr: string | undefined, remoteAddr: string | undefined): void {
    this.currentLocalAddr = localAddr;
    this.currentRemoteAddr = remoteAddr;
  }

  private backendRoutingId(): never {
    return this.backendSessionRoutingId as never;
  }

  private async requireSuccessfulCompletion(
    completions: ZLinkMeshCompletionTable,
    operation: { readonly high: bigint; readonly low: bigint },
    operationName: string,
    signal?: AbortSignal,
    acceptedTerminalResults: ReadonlySet<number> = new Set()
  ): Promise<void> {
    const completion = await completions.wait(operation, signal);
    try {
      if (
        completion.terminalResult !== 0
        && !acceptedTerminalResults.has(completion.terminalResult)
      ) {
        throw createInternalFrameworkException(
          ZLinkFrameworkInternalErrorKind.RouteNotConnected,
          `${operationName} failed with result ${completion.terminalResult} (errno ${completion.failureErrno}).`
        );
      }
    } finally {
      closeMeshCompletion(completion);
    }
  }

  private async submitNativeSessionBind(
    service: StreamSessionService,
    actor: ZLinkBackendActorRef,
    timeoutMs: number,
    signal?: AbortSignal
  ): Promise<{ readonly high: bigint; readonly low: bigint }> {
    const deadline = Date.now() + timeoutMs;
    for (;;) {
      throwIfAborted(signal);
      try {
        return service.bindActor(
          this.backendRoutingId(),
          actor,
          Math.max(0, deadline - Date.now())
        );
      } catch (error) {
        if (
          nativeErrno(error) !== 107
          && (!(error instanceof Error) || !error.message.includes('Transport endpoint is not connected'))
        ) {
          throw error;
        }
        if (Date.now() >= deadline) {
          const status = service.status();
          throw createInternalFrameworkException(
            ZLinkFrameworkInternalErrorKind.RouteNotConnected,
            `STREAM session '${this.sessionId}' was not admitted before the actor bind deadline `
            + `(active sessions ${status.sessionCount}).`
          );
        }
        await waitForSessionAdmission(signal);
      }
    }
  }

  private nativeRoute(meshName: string): ZLinkNativeSessionRoute | undefined {
    return this.nativeSessionRouteForMesh?.(meshName)
      ?? (
        this.nativeSessionService !== undefined && this.meshCompletions !== undefined
          ? {
              service: this.nativeSessionService,
              completions: this.meshCompletions
            }
          : undefined
      );
  }
}

export function streamSessionIdFromRoutingId(routingId: unknown): string {
  if (typeof routingId === 'string') {
    return routingId;
  }
  if (
    typeof routingId === 'object'
    && routingId !== null
    && 'toHex' in routingId
    && typeof (routingId as { toHex?: unknown }).toHex === 'function'
  ) {
    return (routingId as { toHex(): string }).toHex();
  }
  if (
    typeof routingId === 'object'
    && routingId !== null
    && 'toString' in routingId
    && typeof (routingId as { toString?: unknown }).toString === 'function'
  ) {
    return (routingId as { toString(): string }).toString();
  }
  throw createInternalFrameworkException(
    ZLinkFrameworkInternalErrorKind.RouteNotConnected,
    'Stream session routing id is invalid.'
  );
}

function toBackendActorRef(actor: ActorRef): ZLinkBackendActorRef {
  return {
    nodeRid: actor.nodeRid,
    actorId: actor.actorId,
    generation: actor.objectGeneration
  };
}

function toNativeActorRef(actor: ActorRef): ZLinkBackendActorRef {
  return {
    nodeRid: actor.nodeRid,
    actorId: actor.actorId,
    generation: actor.objectGeneration
  };
}

function nativeErrno(error: unknown): number | undefined {
  if (typeof error !== 'object' || error === null || !('nativeErrno' in error)) {
    return undefined;
  }
  const value = error.nativeErrno;
  return typeof value === 'number' ? value : undefined;
}

function waitForSessionAdmission(signal?: AbortSignal): Promise<void> {
  return new Promise((resolve, reject) => {
    const timer = setTimeout(() => {
      signal?.removeEventListener('abort', aborted);
      resolve();
    }, 5);
    const aborted = () => {
      clearTimeout(timer);
      reject(signal?.reason);
    };
    signal?.addEventListener('abort', aborted, { once: true });
  });
}
