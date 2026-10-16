import type { ActorRef, RoutingId, ZLinkActor } from '../../contracts';
import type { ZLinkBackendActorSessionNode } from '../backend';
import {
  type ZLinkActorRoutedJoinTransport,
  type ZLinkRemoteActorPacketTarget,
  type ZLinkRemoteBoundSessionTarget
} from '../actors';
import type { DefaultZLinkActorManager } from '../actors';
import type {
  ZLinkRemoteBoundSessionPort,
  ZLinkStreamActorLookupPort
} from '../streams/stream-binding-runtime-ports';
import {
  actorSessionBindingRuntimeOwner,
  actorSessionBindingRuntimeOwnerIfRegistered
} from '../streams/actor-session-binding-runtime-owner';
import { ZLinkSubmitStatus } from '../messaging/submission-result';
import type { DefaultZLinkBoundSession } from '../streams/session-context';
import type { ZLinkActorResponseOptions } from '../spots/spot-actor-packet-dispatch';
import {
  decodeRemoteBoundSessionErrorPayload,
  decodeRemoteBoundSessionResponsePayload,
  decodeRemoteBoundSessionSendPayload,
  encodeRemoteBoundSessionErrorPayload,
  encodeRemoteBoundSessionResponsePayload,
  ZLinkRemoteBoundSessionFenceError
} from '../actors/bound-session-wire';
import { encodeRemoteActorPacketTarget } from '../actors/actor-packet-relay-wire';
import { routingIdsEqual } from '../routing-id';
import type { MeshRouterResolver } from './mesh-router-resolver';
import {
  encodeSessionRelocationRoute,
  serviceSessionRelocationIdentityKey,
  type ServiceSessionRelocationRoute,
  type ServiceSessionRelocationSeal,
  type ServiceSessionRelocationSealed
} from '../foundation/service-stateful-wire-codec';
import { ServiceWireProtocolError } from '../foundation/service-wire-m6a-codec';
import {
  ZLinkFrameworkInternalErrorKind,
  createInternalFrameworkException
} from '../framework-errors-internal';

export interface ZLinkRemoteBoundSessionRelayOptions {
  readonly requestTimeoutMs?: number;
  readonly sessionRelocationSealTimeoutMs: number;
  readonly routeTransport: ZLinkActorRoutedJoinTransport;
  readonly streamBindingRuntime: () => ZLinkRemoteBoundSessionPort & ZLinkStreamActorLookupPort;
  readonly actorManager: () => DefaultZLinkActorManager | undefined;
  readonly meshRouters: MeshRouterResolver;
  readonly actorSessionNode?: (actorId: string) => ZLinkBackendActorSessionNode | undefined;
  readonly destroyedActorRefs: ReadonlyMap<string, ActorRef>;
  readonly boundSessionFactory: (actorId: string) => DefaultZLinkBoundSession;
  readonly updateRemoteActorPacketTarget: (actorId: string, value: unknown) => Promise<void>;
  readonly actorPacketTargetForState: (actorId: string) => ZLinkRemoteActorPacketTarget | undefined;
  readonly reportOwnershipRefreshError?: (actorId: string, error: unknown) => void;
}

interface ActiveServiceWireSessionRelocation {
  readonly actorId: string;
  readonly sealPromise: Promise<ServiceSessionRelocationSealed>;
  sealTimer?: ReturnType<typeof setTimeout>;
  sealAbortCleanup?: () => void;
  terminal?: boolean;
  routeFingerprint?: string;
  routePromise?: Promise<void>;
}

type RemoteBoundSessionSend = ReturnType<typeof decodeRemoteBoundSessionSendPayload>;

export class ZLinkRemoteBoundSessionRelay {
  private readonly actorOwnershipGenerations = new Map<string, bigint>();
  private readonly activeServiceWireRelocations = new Map<
    string,
    ActiveServiceWireSessionRelocation
  >();

  constructor(private readonly options: ZLinkRemoteBoundSessionRelayOptions) {}

  async receiveRoutedBoundSession(
    actorId: string,
    message: unknown,
    packetName: string | undefined,
    metadata: ReadonlyMap<string, string>,
    actorRef?: ActorRef,
    actorPacketTarget?: unknown
  ): Promise<void> {
    const ownershipGeneration = (
      actorRef as (ActorRef & { ownershipGeneration?: bigint }) | undefined
    )?.ownershipGeneration;
    const currentGeneration = this.actorOwnershipGenerations.get(actorId);
    if (
      currentGeneration !== undefined &&
      (ownershipGeneration === undefined || ownershipGeneration < currentGeneration)
    )
      return;
    if (ownershipGeneration !== undefined) {
      this.actorOwnershipGenerations.set(actorId, ownershipGeneration);
    }
    await this.options.updateRemoteActorPacketTarget(actorId, actorPacketTarget);
    if (
      ownershipGeneration !== undefined &&
      this.actorOwnershipGenerations.get(actorId) !== ownershipGeneration
    )
      return;
    if (actorRef !== undefined) {
      await this.options.streamBindingRuntime().rebindActor(actorRef);
    }
    if (
      ownershipGeneration !== undefined &&
      this.actorOwnershipGenerations.get(actorId) !== ownershipGeneration
    )
      return;
    const sent = await this.options
      .streamBindingRuntime()
      .sendLocalBoundSession(actorId, message, packetName, metadata);
    if (sent) {
      return;
    }
    if (
      ownershipGeneration !== undefined &&
      this.actorOwnershipGenerations.get(actorId) !== ownershipGeneration
    )
      return;
    if (this.options.actorManager()?.getState(actorId)?.remoteBoundSessionTarget === undefined) {
      return;
    }
    const call = this.options.boundSessionFactory(actorId).send(message);
    if (packetName !== undefined) {
      call.packetName(packetName);
    }
    for (const [key, value] of metadata) {
      call.metadata(key, value);
    }
    await call.submit();
  }

  async receiveRoutedBoundSessionResponse(
    actorId: string,
    packetName: string,
    requestSeq: bigint,
    message: unknown,
    replyOptions: ZLinkActorResponseOptions,
    actorPacketTarget?: unknown
  ): Promise<void> {
    await this.options.updateRemoteActorPacketTarget(actorId, actorPacketTarget);
    await this.options
      .streamBindingRuntime()
      .sendLocalBoundSessionResponse(
        actorId,
        packetName,
        requestSeq,
        message,
        replyOptions.metadata,
        replyOptions.compressPayload
      );
  }

  async receiveRoutedBoundSessionError(
    actorId: string,
    packetName: string,
    requestSeq: bigint,
    error: unknown,
    metadata: ReadonlyMap<string, string>,
    actorPacketTarget?: unknown
  ): Promise<void> {
    await this.options.updateRemoteActorPacketTarget(actorId, actorPacketTarget);
    await this.options
      .streamBindingRuntime()
      .sendLocalBoundSessionError(actorId, packetName, requestSeq, error, metadata);
  }

  async receiveRemoteBoundSessionSend(payload: unknown): Promise<{ readonly ok: boolean }> {
    const send = decodeRemoteBoundSessionSendPayload(payload);
    await this.options.updateRemoteActorPacketTarget(send.actorId, send.actorPacketTarget);
    return { ok: await this.deliverRemoteBoundSessionSend(send) };
  }

  private async deliverRemoteBoundSessionSend(send: RemoteBoundSessionSend): Promise<boolean> {
    const metadata = new Map(Object.entries(send.metadata ?? {}));
    if (
      await this.options
        .streamBindingRuntime()
        .sendLocalBoundSession(send.actorId, send.message, send.boundPacketName, metadata)
    ) {
      return true;
    }
    if (
      this.options.actorManager()?.getState(send.actorId)?.remoteBoundSessionTarget === undefined
    ) {
      return false;
    }
    const call = this.options.boundSessionFactory(send.actorId).send(send.message);
    if (send.boundPacketName !== undefined) {
      call.packetName(send.boundPacketName);
    }
    for (const [key, value] of metadata) {
      call.metadata(key, value);
    }
    await call.submit();
    return true;
  }

  async receiveRemoteBoundSessionResponse(payload: unknown): Promise<{ readonly ok: boolean }> {
    const response = decodeRemoteBoundSessionResponsePayload(payload);
    await this.options.updateRemoteActorPacketTarget(response.actorId, response.actorPacketTarget);
    const sent = await this.options
      .streamBindingRuntime()
      .sendLocalBoundSessionResponse(
        response.actorId,
        response.boundPacketName,
        response.requestSeq,
        response.message,
        new Map(Object.entries(response.metadata ?? {})),
        response.compressPayload
      );
    return { ok: sent };
  }

  async receiveRemoteBoundSessionError(payload: unknown): Promise<{ readonly ok: boolean }> {
    const response = decodeRemoteBoundSessionErrorPayload(payload);
    await this.options.updateRemoteActorPacketTarget(response.actorId, response.actorPacketTarget);
    const sent = await this.options
      .streamBindingRuntime()
      .sendLocalBoundSessionError(
        response.actorId,
        response.boundPacketName,
        response.requestSeq,
        response.error,
        new Map(Object.entries(response.metadata ?? {}))
      );
    return { ok: sent };
  }

  async receiveServiceWireSessionRelocationSeal(
    value: ServiceSessionRelocationSeal,
    signal?: AbortSignal,
    onInstalled?: () => void
  ): Promise<ServiceSessionRelocationSealed | undefined> {
    const runtime = this.options.streamBindingRuntime();
    const owner = actorSessionBindingRuntimeOwner(runtime);
    const sealController = new AbortController();
    const sealSignal =
      signal === undefined
        ? sealController.signal
        : AbortSignal.any([signal, sealController.signal]);
    const prepared = await owner.prepareSessionRelocationSeal(value, sealSignal);
    if (prepared === undefined) return undefined;
    if (prepared.response.sealResult === 'bindingAbsent') return prepared.response;
    const key = serviceWireBarrierKey(value);
    const existing = this.activeServiceWireRelocations.get(key);
    if (existing !== undefined) return await existing.sealPromise;
    const actorId = value.actor.actor.actorId;
    const state: ActiveServiceWireSessionRelocation = {
      actorId,
      sealPromise: prepared.ready.then(() => prepared.response)
    };
    this.activeServiceWireRelocations.set(key, state);
    onInstalled?.();
    const expire = async (error: unknown): Promise<void> => {
      if (state.terminal === true || state.routePromise !== undefined) return;
      state.terminal = true;
      sealController.abort(error);
      if (this.activeServiceWireRelocations.get(key) === state) {
        this.activeServiceWireRelocations.delete(key);
      }
      await owner.clearRelocation(actorId, error, key);
      void runtime
        .closeSessionBinding(actorId, value.session)
        .catch((disconnectError) =>
          this.options.reportOwnershipRefreshError?.(actorId, disconnectError)
        );
      state.sealAbortCleanup?.();
    };
    const timeoutError = () =>
      createInternalFrameworkException(
        ZLinkFrameworkInternalErrorKind.DeadlineExceeded,
        `Actor '${actorId}' Session relocation seal exceeded its absolute deadline.`,
        true
      );
    state.sealTimer = setTimeout(() => {
      void expire(timeoutError());
    }, this.options.sessionRelocationSealTimeoutMs);
    state.sealTimer.unref();
    if (signal !== undefined) {
      const onAbort = () => {
        void expire(signal.reason ?? timeoutError());
      };
      state.sealAbortCleanup = () => signal.removeEventListener('abort', onAbort);
      if (signal.aborted) onAbort();
      else signal.addEventListener('abort', onAbort, { once: true });
    }
    try {
      return await state.sealPromise;
    } catch (error) {
      clearTimeout(state.sealTimer);
      state.sealAbortCleanup?.();
      if (this.activeServiceWireRelocations.get(key) === state) {
        this.activeServiceWireRelocations.delete(key);
      }
      if (state.terminal !== true && (await owner.relocationSnapshot(actorId, key)) !== undefined) {
        await owner.clearRelocation(actorId, error, key);
        void runtime
          .closeSessionBinding(actorId, value.session)
          .catch((disconnectError) =>
            this.options.reportOwnershipRefreshError?.(actorId, disconnectError)
          );
      }
      throw sealController.signal.aborted ? sealController.signal.reason : error;
    }
  }

  async receiveServiceWireSessionRelocationRoute(
    value: ServiceSessionRelocationRoute
  ): Promise<void> {
    const key = serviceWireBarrierKey(value);
    const fingerprint = encodeSessionRelocationRoute(value).toString('base64');
    const accepted = await actorSessionBindingRuntimeOwner(
      this.options.streamBindingRuntime()
    ).acceptSessionRelocationRoute(value);
    const state = this.activeServiceWireRelocations.get(key);
    if (!accepted || state === undefined) {
      this.warnLateSessionRouteUpdate(value);
      return;
    }
    if (state.routeFingerprint !== undefined) {
      if (state.routeFingerprint !== fingerprint || state.routePromise === undefined) {
        throw new ServiceWireProtocolError(
          `Session relocation '${key}' repeated command 44 with different bytes.`
        );
      }
      this.warnLateSessionRouteUpdate(value);
      await state.routePromise;
      return;
    }
    state.routeFingerprint = fingerprint;
    if (state.sealTimer !== undefined) clearTimeout(state.sealTimer);
    state.sealAbortCleanup?.();
    const routePromise = state.sealPromise.then(async (sealed) => {
      validateSessionRelocationRouteAgainstSeal(value, sealed);
      await this.applyServiceWireSessionRelocationRoute(key, value);
    });
    state.routePromise = routePromise;
    try {
      await routePromise;
      if (this.activeServiceWireRelocations.get(key) === state) {
        this.activeServiceWireRelocations.delete(key);
      }
      state.terminal = true;
    } catch (error) {
      // Command 44 is one-way. Once an exact update wins the timeout race,
      // an apply failure cannot be repaired by waiting for an ACK-driven
      // retry. Close the physical Session and terminalize this identity so a
      // late duplicate cannot reactivate the binding or its held messages.
      state.terminal = true;
      if (this.activeServiceWireRelocations.get(key) === state) {
        this.activeServiceWireRelocations.delete(key);
      }
      await actorSessionBindingRuntimeOwner(this.options.streamBindingRuntime()).clearRelocation(
        state.actorId,
        error,
        key
      );
      await this.options
        .streamBindingRuntime()
        .closeSessionBinding(state.actorId, value.session)
        .catch((disconnectError) =>
          this.options.reportOwnershipRefreshError?.(state.actorId, disconnectError)
        );
      throw error;
    }
  }

  async clearServiceWireSessionRelocations(): Promise<void> {
    for (const state of this.activeServiceWireRelocations.values()) {
      if (state.sealTimer !== undefined) clearTimeout(state.sealTimer);
      state.sealAbortCleanup?.();
    }
    this.activeServiceWireRelocations.clear();
    await actorSessionBindingRuntimeOwnerIfRegistered(
      this.options.streamBindingRuntime()
    )?.clearSessionRelocationResults();
  }

  private warnLateSessionRouteUpdate(value: ServiceSessionRelocationRoute): void {
    console.warn(`late_session_route_update session=${value.session.sessionRid}`);
  }

  private async applyServiceWireSessionRelocationRoute(
    key: string,
    value: ServiceSessionRelocationRoute
  ): Promise<void> {
    const runtime = this.options.streamBindingRuntime();
    const owner = actorSessionBindingRuntimeOwner(runtime);
    const fingerprint = encodeSessionRelocationRoute(value).toString('base64');
    if (value.route.action === 'abort') {
      await owner.applyRelocation(value.actor.actorId, key, fingerprint, async () => {
        if (!(await runtime.abortActorRouteSeal(value.actor.actorId, key))) {
          throw new ZLinkRemoteBoundSessionFenceError(
            `Actor '${value.actor.actorId}' command 44 abort lost its Session seal.`
          );
        }
      });
    } else {
      const committedRoute = value.route;
      const current = (await runtime.find(value.actor.actorId))?.ref;
      if (current === undefined || current.objectGeneration !== value.actor.generation) {
        throw new ZLinkRemoteBoundSessionFenceError(
          `Actor '${value.actor.actorId}' command 44 has no exact Session binding.`
        );
      }
      const targetActorRef = {
        actorId: value.actor.actorId,
        objectGeneration: value.actor.generation,
        meshName: current.meshName,
        nodeRid: committedRoute.targetNodeRid,
        bindingGeneration: value.session.bindingGeneration,
        ownershipGeneration: committedRoute.targetAuthorityOwnerGeneration,
        ownerNodeGeneration: committedRoute.targetNodeGeneration,
        ownerLeaseGeneration: committedRoute.targetOwnerLeaseGeneration
      } as ActorRef;
      await owner.applyRelocation(value.actor.actorId, key, fingerprint, async () => {
        await runtime.commitActorRoute(targetActorRef, undefined, {
          confirmRemoteSessionBinding: 'send',
          releaseSeal: { sealId: key }
        });
        const currentGeneration = this.actorOwnershipGenerations.get(value.actor.actorId);
        if (
          currentGeneration === undefined ||
          currentGeneration < committedRoute.targetAuthorityOwnerGeneration
        ) {
          this.actorOwnershipGenerations.set(
            value.actor.actorId,
            committedRoute.targetAuthorityOwnerGeneration
          );
        }
      });
    }
    await owner.observeRelocationTerminal(value.actor.actorId, key, fingerprint);
  }

  resolveRemoteBoundSessionTarget(
    sourceNodeRid: RoutingId,
    sourceSessionRid: RoutingId
  ): ZLinkRemoteBoundSessionTarget | undefined {
    const target = this.options.meshRouters.remoteBoundSessionTargetForSource(sourceNodeRid);
    if (target === undefined) return undefined;
    // Spec 48 §116/§153/§324-332: the source Session's identity travels with
    // the bind refresh so a later refresh can tell a same-Session
    // coordinate-only update apart from a successor binding (a new Session
    // never inherits the previous Session's staged relocation fence). The
    // topology resolver stays identity-agnostic; identity is attached here,
    // at the bind-refresh producer.
    return {
      ...target,
      sessionNodeRid: sourceNodeRid,
      sessionRid: sourceSessionRid
    };
  }

  actorPacketTargetForState(actorId: string): ZLinkRemoteActorPacketTarget | undefined {
    return this.options.actorPacketTargetForState(actorId);
  }

  clearOwnership(actorId: string): void {
    this.actorOwnershipGenerations.delete(actorId);
    actorSessionBindingRuntimeOwnerIfRegistered(
      this.options.streamBindingRuntime()
    )?.clearRelocation(actorId, new Error(`Actor '${actorId}' ownership state was cleared.`));
    for (const [key, value] of this.activeServiceWireRelocations) {
      if (value.actorId === actorId) {
        if (value.sealTimer !== undefined) clearTimeout(value.sealTimer);
        this.activeServiceWireRelocations.delete(key);
      }
    }
  }

  async sendActorResponse(
    actor: ZLinkActor,
    packetName: string,
    requestSeq: bigint,
    response: unknown,
    replyOptions: ZLinkActorResponseOptions,
    fallbackBoundSessionTarget?: ZLinkRemoteBoundSessionTarget,
    fallbackActorRef?: ActorRef,
    signal?: AbortSignal
  ): Promise<void> {
    const state = this.options.actorManager()?.getState(actor.context.actorId);
    if (
      await this.options
        .streamBindingRuntime()
        .sendLocalBoundSessionResponse(
          actor.context.actorId,
          packetName,
          requestSeq,
          response,
          replyOptions.metadata,
          replyOptions.compressPayload
        )
    ) {
      return;
    }
    const actorRef = currentActorRef(state, fallbackActorRef);
    const remoteTarget = fallbackBoundSessionTarget ?? state?.remoteBoundSessionTarget;
    if (remoteTarget !== undefined) {
      await this.sendRemoteBoundSessionResponse(
        remoteTarget,
        actor.context.actorId,
        packetName,
        requestSeq,
        response,
        replyOptions
      );
      return;
    }
    if (actorRef === undefined) {
      throw new Error(`Actor '${actor.context.actorId}' does not have a native actor ref.`);
    }
    const actorSessionNode = this.options.actorSessionNode?.(actor.context.actorId);
    if (actorSessionNode === undefined) {
      throw new Error(
        'Native bound-session response requires the RouteMesh stream-session service.'
      );
    }
    await this.options
      .streamBindingRuntime()
      .sendNativeBoundSessionResponse(
        actorSessionNode,
        actorRef,
        packetName,
        requestSeq,
        response,
        replyOptions.metadata,
        replyOptions.compressPayload,
        signal
      );
  }

  async sendActorError(
    actorId: string,
    packetName: string,
    requestSeq: bigint,
    error: unknown,
    metadata: ReadonlyMap<string, string>,
    fallbackBoundSessionTarget?: ZLinkRemoteBoundSessionTarget,
    fallbackActorRef?: ActorRef,
    signal?: AbortSignal
  ): Promise<void> {
    if (
      await this.options
        .streamBindingRuntime()
        .sendLocalBoundSessionError(actorId, packetName, requestSeq, error, metadata)
    ) {
      return;
    }
    const state = this.options.actorManager()?.getState(actorId);
    const actorRef =
      currentActorRef(state, fallbackActorRef) ?? this.options.destroyedActorRefs.get(actorId);
    const remoteTarget = fallbackBoundSessionTarget ?? state?.remoteBoundSessionTarget;
    if (remoteTarget !== undefined) {
      await this.sendRemoteBoundSessionError(
        remoteTarget,
        actorId,
        packetName,
        requestSeq,
        error,
        metadata
      );
      return;
    }
    if (actorRef === undefined) {
      throw new Error(`Actor '${actorId}' does not have a native actor ref.`);
    }
    const actorSessionNode = this.options.actorSessionNode?.(actorId);
    if (actorSessionNode === undefined) {
      throw new Error(
        'Native bound-session error response requires the RouteMesh stream-session service.'
      );
    }
    await this.options
      .streamBindingRuntime()
      .sendNativeBoundSessionError(
        actorSessionNode,
        actorRef,
        packetName,
        requestSeq,
        error,
        metadata,
        signal
      );
  }

  private async sendRemoteBoundSessionResponse(
    target: ZLinkRemoteBoundSessionTarget,
    actorId: string,
    packetName: string,
    requestSeq: bigint,
    message: unknown,
    replyOptions: ZLinkActorResponseOptions
  ): Promise<void> {
    const actorPacketTarget = encodeRemoteActorPacketTarget(
      this.options.actorPacketTargetForState(actorId)
    );
    await this.sendRemoteBoundSessionControl(
      target,
      encodeRemoteBoundSessionResponsePayload({
        actorId,
        boundPacketName: packetName,
        requestSeq,
        message,
        metadata: replyOptions.metadata,
        compressPayload: replyOptions.compressPayload,
        actorPacketTarget
      })
    );
  }

  private async sendRemoteBoundSessionError(
    target: ZLinkRemoteBoundSessionTarget,
    actorId: string,
    packetName: string,
    requestSeq: bigint,
    error: unknown,
    metadata: ReadonlyMap<string, string>
  ): Promise<void> {
    await this.sendRemoteBoundSessionControl(
      target,
      encodeRemoteBoundSessionErrorPayload({
        actorId,
        boundPacketName: packetName,
        requestSeq,
        error,
        metadata,
        actorPacketTarget: encodeRemoteActorPacketTarget(
          this.options.actorPacketTargetForState(actorId)
        )
      })
    );
  }

  private async sendRemoteBoundSessionControl(
    target: ZLinkRemoteBoundSessionTarget,
    payload: Record<string, unknown>
  ): Promise<void> {
    const packetName = payload.packetName;
    if (typeof packetName !== 'string') {
      throw new Error('Remote bound session control payload does not declare a packet name.');
    }
    const submit =
      this.options.routeTransport.submitInfrastructure ?? this.options.routeTransport.submit;
    if (submit === undefined) {
      throw new Error('Remote bound session node-direct transport is not available.');
    }
    const result = await submit.call(
      this.options.routeTransport,
      target.routerChannelId,
      String(target.targetNodeRid),
      packetName,
      payload
    );
    if (result.status !== ZLinkSubmitStatus.Submitted) {
      throw new Error(
        `Remote bound session control '${packetName}' was not admitted` +
          ` on '${target.routerChannelId}' for '${String(target.targetNodeRid)}'` +
          ` (status ${result.status}).`
      );
    }
  }
}

function currentActorRef(
  state:
    | {
        readonly nativeActorRef?: {
          readonly actorId: string;
          readonly nodeRid: ActorRef['nodeRid'];
          readonly generation?: bigint;
          readonly objectGeneration?: bigint;
        };
        readonly meshName?: string;
        readonly boundSessionBindingGeneration: bigint;
        readonly locationGeneration?: bigint;
        readonly ownerLeaseGeneration?: bigint;
      }
    | undefined,
  fallback: ActorRef | undefined
): ActorRef | undefined {
  const native = state?.nativeActorRef;
  if (native === undefined) return fallback;
  const objectGeneration =
    native.generation ?? native.objectGeneration ?? fallback?.objectGeneration;
  const meshName = state?.meshName ?? fallback?.meshName;
  if (objectGeneration === undefined || meshName === undefined) return fallback;
  return {
    actorId: native.actorId,
    objectGeneration,
    meshName,
    nodeRid: native.nodeRid,
    ownershipGeneration: state?.locationGeneration,
    ownerLeaseGeneration: state?.ownerLeaseGeneration,
    bindingGeneration: state?.boundSessionBindingGeneration
  } as ActorRef;
}

function serviceWireBarrierKey(
  value: ServiceSessionRelocationSeal | ServiceSessionRelocationRoute
): string {
  return serviceSessionRelocationIdentityKey(value);
}

function validateSessionRelocationRouteAgainstSeal(
  route: ServiceSessionRelocationRoute,
  sealed: ServiceSessionRelocationSealed
): void {
  const sameSession =
    routingIdsEqual(route.session.sessionOwnerNodeRid, sealed.session.sessionOwnerNodeRid) &&
    route.session.sessionOwnerNodeGeneration === sealed.session.sessionOwnerNodeGeneration &&
    route.session.sessionOwnerId === sealed.session.sessionOwnerId &&
    route.session.sessionOwnerLeaseGeneration === sealed.session.sessionOwnerLeaseGeneration &&
    routingIdsEqual(route.session.sessionRid, sealed.session.sessionRid) &&
    route.session.bindingGeneration === sealed.session.bindingGeneration;
  if (
    route.relocation.high !== sealed.relocation.high ||
    route.relocation.low !== sealed.relocation.low ||
    !sameSession ||
    route.actor.actorId !== sealed.actor.actor.actorId ||
    route.actor.generation !== sealed.actor.actor.generation
  ) {
    throw new ServiceWireProtocolError(
      `Session relocation '${serviceWireBarrierKey(route)}' changed its command 42 fence.`
    );
  }
}
