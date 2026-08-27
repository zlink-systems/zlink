import {
  ZLinkFrameworkInternalErrorKind,
  createInternalFrameworkException
} from '../framework-errors-internal';
import type {
  ActorRef,
  RoutingId,
  ZLinkMessage,
  ZLinkMessageSerializer,
  ZLinkStream
} from '../../contracts';
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
import { RequestResult, isBackendNotConnectedError } from '../backend/runtime-values';
import { ZLinkBufferMessage as NativeMessage } from '../backend/runtime-message';
import type { StreamSessionService } from '../foundation/service-runtime-contracts';
import type { ServiceRetiredBoundSessionRouteFence } from '../foundation/service-stateful-wire-codec';
import type { ServiceActorRef } from '../foundation/service-stateful-registry';
import {
  closeMeshCompletion,
  type ZLinkMeshCompletion,
  type ZLinkMeshCompletionTable
} from '../backend/mesh-completion-table';
import {
  encodeSessionClosingFrame,
  encodeStreamControlFrame,
  ZLinkStreamCloseReasonCode
} from './protocol';

const ZLINK_SEND_DONT_WAIT = 1;
const NO_ACCEPTED_TERMINAL_RESULTS: ReadonlySet<number> = new Set();
const IDEMPOTENT_UNBIND_TERMINAL_RESULTS: ReadonlySet<number> = new Set([
  RequestResult.NotConnected,
  RequestResult.NotFound
]);

export interface ZLinkNativeSessionRoute {
  readonly service: StreamSessionService;
  readonly completions: ZLinkMeshCompletionTable;
}

export class ZLinkManagedStream implements ZLinkStream {
  private currentLocalAddr: string | undefined;
  private currentRemoteAddr: string | undefined;
  private transportClosed = false;
  private readonly nativeActorBindings = new Map<string, {
    readonly actor: ZLinkBackendActorRef;
    readonly bindingGeneration: bigint;
    readonly route?: ZLinkNativeSessionRoute;
  }>();

  constructor(
    private readonly socket: ZLinkBackendStreamSocket,
    private readonly backendSessionRoutingId: unknown,
    private readonly messageSerializers?: ReadonlyMap<string, ZLinkMessageSerializer>,
    private readonly nativeSessionService?: StreamSessionService,
    private readonly meshCompletions?: ZLinkMeshCompletionTable,
    private readonly publicSessionId = streamSessionIdFromRoutingId(backendSessionRoutingId),
    private readonly nativeSessionRouteForMesh?: (
      meshName: string
    ) => ZLinkNativeSessionRoute | undefined
  ) {}

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

  async submitRaw(
    payload: Message,
    signal?: AbortSignal,
    timeoutMs?: number
  ): Promise<ZLinkSubmitResult> {
    try {
      throwIfAborted(signal);
      await this.socket.sendAsync(this.backendRoutingId(), payload, timeoutMs);
      return { status: ZLinkSubmitStatus.Submitted };
    } catch (error) {
      if (isBackendNotConnectedError(error)) return { status: ZLinkSubmitStatus.Backpressured };
      throw error;
    }
  }

  async submitBoundActor(
    actorId: string,
    parts: readonly Message[],
    signal?: AbortSignal
  ): Promise<ZLinkSubmitResult> {
    throwIfAborted(signal);
    return await this.sendBoundActor(actorId, parts, ZLINK_SEND_DONT_WAIT)
      ? { status: ZLinkSubmitStatus.Submitted }
      : { status: ZLinkSubmitStatus.Backpressured };
  }


  async close(signal?: AbortSignal): Promise<void> {
    throwIfAborted(signal);
    this.markTransportClosed();
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
    this.markTransportClosed();
    const closing = NativeMessage.from(encodeSessionClosingFrame(diagnostic, reason));
    try {
      this.socket.send(this.backendRoutingId(), closing, 0);
    } finally {
      closing.close();
    }
    this.socket.disconnectPeer(this.backendRoutingId());
  }

  async bindActor(
    actor: ActorRef,
    timeoutMs: number,
    signal?: AbortSignal,
    onBindingReplaced?: (
      actor: ServiceActorRef,
      retiredSession: ServiceRetiredBoundSessionRouteFence
    ) => void
  ): Promise<void> {
    throwIfAborted(signal);
    if (this.transportClosed) {
      throw createInternalFrameworkException(
        ZLinkFrameworkInternalErrorKind.RouteNotConnected,
        `Stream session '${this.sessionId}' is disconnected.`
      );
    }
    const route = this.nativeRoute(actor.meshName);
    if (route !== undefined) {
      if (route.service.status().state === 1) {
        route.service.start();
      }
      const nativeActor = toNativeActorRef(actor);
      await this.ensureNativeActorRoute(route, actor, timeoutMs, signal);
      if (this.transportClosed) {
        throw createInternalFrameworkException(
          ZLinkFrameworkInternalErrorKind.RouteNotConnected,
          `Stream session '${this.sessionId}' is disconnected.`
        );
      }
      await this.requireSuccessfulCompletion(
        this.submitNativeSessionBind(
          route.service,
          route.completions,
          nativeActor,
          timeoutMs,
          signal,
          onBindingReplaced === undefined
            ? undefined
            : (_actorId, retiredSession, replacedActor) => {
                onBindingReplaced(replacedActor, retiredSession);
              }
        ),
        `Actor '${actor.actorId}' native session bind`,
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
      if (this.transportClosed) {
        try {
          await this.requireSuccessfulCompletion(
            route.completions.submit(
              () => route.service.unbindActor(
                this.backendRoutingId(),
                nativeActor as never,
                binding.bindingGeneration,
                timeoutMs
              ),
              signal
            ),
            `Actor '${actor.actorId}' native session unbind`,
            IDEMPOTENT_UNBIND_TERMINAL_RESULTS
          );
        } catch (error) {
          if (this.hasNativeBinding(route, actor.actorId, binding.bindingGeneration)) {
            throw error;
          }
        }
        throw createInternalFrameworkException(
          ZLinkFrameworkInternalErrorKind.RouteNotConnected,
          `Stream session '${this.sessionId}' is disconnected.`
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
    const completion = await route.completions.submit(
      () => route.service.lookupActor(actor.nodeRid, actor.actorId, timeoutMs),
      signal
    );
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
    if (this.transportClosed) {
      // The native stream session is released by transport teardown. Do not
      // submit a second unbind for a stale binding during reconnect cleanup.
      this.nativeActorBindings.delete(actorId);
      return;
    }
    const binding = this.nativeActorBindings.get(actorId);
    if (binding?.route !== undefined) {
      const route = binding.route;
      if (!this.hasNativeBinding(route, actorId, binding.bindingGeneration)) {
        this.nativeActorBindings.delete(actorId);
        return;
      }
      try {
        await this.requireSuccessfulCompletion(
          route.completions.submit(
            () => route.service.unbindActor(
              this.backendRoutingId(),
              binding.actor as never,
              binding.bindingGeneration,
              timeoutMs
            ),
            signal
          ),
          `Actor '${actorId}' native session unbind`,
          // Destroy can remove the actor registry entry before the old
          // session owner processes its cleanup. Both a disconnected route and
          // that stale actor route are idempotent unbind outcomes.
          IDEMPOTENT_UNBIND_TERMINAL_RESULTS
        );
      } catch (error) {
        // The service removes its local delivery before sending a remote
        // tombstone. A transport teardown can therefore report an internal
        // completion after the exact binding is already gone. Treat only that
        // exact missing binding as stale cleanup; preserve other failures.
        if (this.hasNativeBinding(route, actorId, binding.bindingGeneration)) {
          throw error;
        }
      }
      if (this.nativeActorBindings.get(actorId) === binding) {
        this.nativeActorBindings.delete(actorId);
      }
      return;
    }
    await this.socket.unbindActor(this.backendRoutingId(), actorId, timeoutMs, signal);
  }

  async sendBoundActor(
    actorId: string,
    parts: readonly Message[],
    flags?: ZLinkBackendSendFlags
  ): Promise<boolean> {
    if (this.transportClosed) {
      throw createInternalFrameworkException(
        ZLinkFrameworkInternalErrorKind.RouteNotConnected,
        `Stream session '${this.sessionId}' is disconnected.`
      );
    }
    const binding = this.nativeActorBindings.get(actorId);
    if (binding?.route !== undefined) {
      const nativeParts = parts.map((part) => NativeMessage.from(part.data()));
      try {
        return await binding.route.service.sendToActor(
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
    return this.socket.sendBoundActor(
      this.backendRoutingId(),
      actorId,
      parts,
      flags ?? 0
    );
  }

  updateAddresses(localAddr: string | undefined, remoteAddr: string | undefined): void {
    this.currentLocalAddr = localAddr;
    this.currentRemoteAddr = remoteAddr;
  }

  /** @internal Marks transport teardown before asynchronous lifecycle cleanup begins. */
  markTransportClosed(): void {
    this.transportClosed = true;
  }

  private hasNativeBinding(
    route: ZLinkNativeSessionRoute,
    actorId: string,
    bindingGeneration: bigint
  ): boolean {
    try {
      return route.service.bindings(this.backendRoutingId()).some(candidate =>
        candidate.actor.actorId === actorId
        && candidate.bindingGeneration === bindingGeneration);
    } catch {
      // A closed native service owns the teardown state. Keep the original
      // error when its binding snapshot is unavailable.
      return true;
    }
  }

  private backendRoutingId(): never {
    return this.backendSessionRoutingId as never;
  }

  private async requireSuccessfulCompletion(
    completionPromise: Promise<ZLinkMeshCompletion>,
    operationName: string,
    acceptedTerminalResults: ReadonlySet<number> = NO_ACCEPTED_TERMINAL_RESULTS
  ): Promise<void> {
    const completion = await completionPromise;
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
    completions: ZLinkMeshCompletionTable,
    actor: ZLinkBackendActorRef,
    timeoutMs: number,
    signal?: AbortSignal,
    onBindingReplaced?: (
      actor: string,
      retiredSession: ServiceRetiredBoundSessionRouteFence,
      replacedActor: ServiceActorRef
    ) => void
  ): Promise<ZLinkMeshCompletion> {
    const deadline = Date.now() + timeoutMs;
    for (;;) {
      throwIfAborted(signal);
      try {
        return completions.submit(
          () => service.bindActor(
            this.backendRoutingId(),
            actor,
            Math.max(0, deadline - Date.now()),
            onBindingReplaced
          ),
          signal
        );
      } catch (error) {
        if (!isBackendNotConnectedError(error)) {
          throw error;
        }
        if (Date.now() >= deadline) {
          const status = service.status();
          throw createInternalFrameworkException(
            ZLinkFrameworkInternalErrorKind.DeadlineExceeded,
            `STREAM session '${this.sessionId}' was not admitted before the actor bind deadline `
            + `(active sessions ${status.sessionCount}).`,
            error
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
