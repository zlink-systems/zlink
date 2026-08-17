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
import {
  decodeRoutingId as decodeWireRoutingId,
  routingIdsEqual
} from '../routing-id';
import type { MeshRouterResolver } from './mesh-router-resolver';
import {
  encodeSessionRelocationRoute,
  encodeSessionRelocationSeal,
  type ServiceSessionRelocationRoute,
  type ServiceSessionRelocationSeal,
  type ServiceSessionRelocationSealed
} from '../foundation/service-stateful-wire-codec';
import { ServiceWireProtocolError } from '../foundation/service-wire-m6a-codec';
import { BoundedReplayMap } from './bounded-replay-map';
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
  readonly updateRemoteActorPacketTarget: (actorId: string, value: unknown) => void;
  readonly actorPacketTargetForState: (
    actorId: string,
    routerChannelIdHint?: string
  ) => ZLinkRemoteActorPacketTarget | undefined;
  readonly reportOwnershipRefreshError?: (actorId: string, error: unknown) => void;
}

const SERVICE_CONTROL_TERMINAL_CAPACITY = 4096;

interface ActiveServiceWireSessionRelocation {
  readonly actorId: string;
  readonly sealFingerprint: string;
  readonly sealPromise: Promise<ServiceSessionRelocationSealed>;
  sealed?: ServiceSessionRelocationSealed;
  sealTimer?: ReturnType<typeof setTimeout>;
  sealController?: AbortController;
  sealAbortCleanup?: () => void;
  terminal?: boolean;
  routeRequestFingerprint?: string;
  routeFingerprint?: string;
  routePromise?: Promise<void>;
}

interface TerminalServiceWireSessionRelocation {
  readonly actorId: string;
  readonly sealFingerprint: string;
  readonly sealed: ServiceSessionRelocationSealed;
  /** Absent only when the Session seal timed out before command 44 arrived. */
  readonly routeRequestFingerprint?: string;
  readonly routeFingerprint?: string;
}

type RemoteBoundSessionSend = ReturnType<typeof decodeRemoteBoundSessionSendPayload>;

export class ZLinkRemoteBoundSessionRelay {
  private readonly actorOwnershipGenerations = new Map<string, bigint>();
  private readonly activeServiceWireRelocations =
    new Map<string, ActiveServiceWireSessionRelocation>();
  private readonly terminalServiceWireRelocations =
    new BoundedReplayMap<string, TerminalServiceWireSessionRelocation>(
      SERVICE_CONTROL_TERMINAL_CAPACITY
    );

  constructor(private readonly options: ZLinkRemoteBoundSessionRelayOptions) {
  }

  async receiveRoutedBoundSession(
    actorId: string,
    message: unknown,
    packetName: string | undefined,
    metadata: ReadonlyMap<string, string>,
    actorRef?: ActorRef,
    actorPacketTarget?: unknown
  ): Promise<void> {
    const ownershipGeneration = (actorRef as (ActorRef & { ownershipGeneration?: bigint }) | undefined)
      ?.ownershipGeneration;
    const currentGeneration = this.actorOwnershipGenerations.get(actorId);
    if (
      currentGeneration !== undefined &&
      (ownershipGeneration === undefined || ownershipGeneration < currentGeneration)
    ) return;
    this.options.updateRemoteActorPacketTarget(actorId, actorPacketTarget);
    if (actorRef !== undefined) {
      await this.options.streamBindingRuntime().rebindActor(actorRef);
    }
    if (ownershipGeneration !== undefined) {
      this.actorOwnershipGenerations.set(actorId, ownershipGeneration);
    }
    const sent = this.options.streamBindingRuntime().sendLocalBoundSession(actorId, message, packetName, metadata);
    if (sent) {
      return;
    }
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
    this.options.updateRemoteActorPacketTarget(actorId, actorPacketTarget);
    this.options.streamBindingRuntime().sendLocalBoundSessionResponse(
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
    this.options.updateRemoteActorPacketTarget(actorId, actorPacketTarget);
    this.options.streamBindingRuntime().sendLocalBoundSessionError(
      actorId,
      packetName,
      requestSeq,
      error,
      metadata
    );
  }

  async receiveRemoteBoundSessionSend(payload: unknown): Promise<{ readonly ok: boolean }> {
    const send = decodeRemoteBoundSessionSendPayload(payload);
    this.options.updateRemoteActorPacketTarget(send.actorId, send.actorPacketTarget);
    const owner = actorSessionBindingRuntimeOwnerIfRegistered(
      this.options.streamBindingRuntime()
    );
    const retained = owner?.retainRelocationOutbound(send.actorId, {
      deliver: () => this.deliverRemoteBoundSessionSend(send),
      fail: error => this.options.reportOwnershipRefreshError?.(send.actorId, error)
    });
    if (retained === 'retained') return { ok: true };
    if (retained === 'rejected') return { ok: false };
    return { ok: await this.deliverRemoteBoundSessionSend(send) };
  }

  private async deliverRemoteBoundSessionSend(send: RemoteBoundSessionSend): Promise<boolean> {
    const metadata = new Map(Object.entries(send.metadata ?? {}));
    if (this.options.streamBindingRuntime().sendLocalBoundSession(
      send.actorId,
      send.message,
      send.boundPacketName,
      metadata
    )) {
      return true;
    }
    if (this.options.actorManager()?.getState(send.actorId)?.remoteBoundSessionTarget === undefined) {
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
    this.options.updateRemoteActorPacketTarget(response.actorId, response.actorPacketTarget);
    const sent = this.options.streamBindingRuntime().sendLocalBoundSessionResponse(
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
    this.options.updateRemoteActorPacketTarget(response.actorId, response.actorPacketTarget);
    const sent = this.options.streamBindingRuntime().sendLocalBoundSessionError(
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
    signal?: AbortSignal
  ): Promise<ServiceSessionRelocationSealed> {
    const key = serviceWireBarrierKey(value);
    const fingerprint = encodeSessionRelocationSeal(value).toString('base64');
    const terminal = this.terminalServiceWireRelocations.get(key);
    if (terminal !== undefined) {
      if (terminal.sealFingerprint !== fingerprint) {
        throw new ServiceWireProtocolError(
          `Session relocation '${key}' repeated command 42 with different bytes.`
        );
      }
      this.terminalServiceWireRelocations.touch(key);
      return terminal.sealed;
    }
    const active = this.activeServiceWireRelocations.get(key);
    if (active !== undefined) {
      if (active.sealFingerprint !== fingerprint) {
        throw new ServiceWireProtocolError(
          `Session relocation '${key}' repeated command 42 with different bytes.`
        );
      }
      return await active.sealPromise;
    }
    const actorId = value.actor.actor.actorId;

    // Register the identity before the first asynchronous owner operation.
    // Concurrent identical commands join this promise; different bytes for
    // the same identity fail before they can install another seal.
    let state!: ActiveServiceWireSessionRelocation;
    const sealController = new AbortController();
    const sealPromise = Promise.resolve().then(async () => {
      this.validateServiceWireSessionRoute(value);
      await actorSessionBindingRuntimeOwner(this.options.streamBindingRuntime()).sealRelocation(
        {
          actorId,
          actorGeneration: value.actor.actor.generation,
          bindingGeneration: value.session.bindingGeneration,
          sessionIdentity: value.session.sessionRid,
          actorNodeRid: value.actor.actor.nodeRid,
          actorNodeGeneration: value.actor.targetNodeGeneration,
          sealId: key
        },
        {
          objectGeneration: value.actor.actor.generation,
          bindingGeneration: value.session.bindingGeneration
        },
        sealController.signal
      );
      return {
        relocation: value.relocation,
        coordinator: value.coordinator,
        actor: value.actor,
        session: value.session
      } satisfies ServiceSessionRelocationSealed;
    });
    state = {
      actorId,
      sealFingerprint: fingerprint,
      sealPromise,
      sealController
    };
    this.activeServiceWireRelocations.set(key, state);
    const expire = (error: unknown): void => {
      if (state.terminal === true || state.routePromise !== undefined) return;
      state.terminal = true;
      sealController.abort(error);
      if (this.activeServiceWireRelocations.get(key) === state) {
        this.activeServiceWireRelocations.delete(key);
      }
      if (state.sealed !== undefined) {
        this.terminalServiceWireRelocations.remember(key, {
          actorId,
          sealFingerprint: state.sealFingerprint,
          sealed: state.sealed
        });
      }
      const owner = actorSessionBindingRuntimeOwner(this.options.streamBindingRuntime());
      owner.clearRelocation(actorId, error);
      void this.options.streamBindingRuntime().disconnectBoundSession(actorId)
        .catch(disconnectError => this.options.reportOwnershipRefreshError?.(actorId, disconnectError));
      state.sealAbortCleanup?.();
    };
    const timeoutError = () => createInternalFrameworkException(
      ZLinkFrameworkInternalErrorKind.DeadlineExceeded,
      `Actor '${actorId}' Session relocation seal exceeded its absolute deadline.`,
      true
    );
    state.sealTimer = setTimeout(
      () => expire(timeoutError()),
      this.options.sessionRelocationSealTimeoutMs ?? 3_000
    );
    state.sealTimer.unref?.();
    if (signal !== undefined) {
      const onAbort = () => expire(signal.reason ?? timeoutError());
      state.sealAbortCleanup = () => signal.removeEventListener('abort', onAbort);
      if (signal.aborted) onAbort();
      else signal.addEventListener('abort', onAbort, { once: true });
    }
    try {
      const sealed = await sealPromise;
      state.sealed = sealed;
      return sealed;
    } catch (error) {
      if (state.sealTimer !== undefined) clearTimeout(state.sealTimer);
      state.sealAbortCleanup?.();
      if (this.activeServiceWireRelocations.get(key) === state) {
        this.activeServiceWireRelocations.delete(key);
      }
      if (state.terminal !== true) {
        const owner = actorSessionBindingRuntimeOwner(this.options.streamBindingRuntime());
        if (owner.relocationSnapshot(actorId, key) !== undefined) {
          owner.clearRelocation(actorId, error);
          void this.options.streamBindingRuntime().disconnectBoundSession(actorId)
            .catch(disconnectError => this.options.reportOwnershipRefreshError?.(actorId, disconnectError));
        }
      }
      throw sealController.signal.aborted ? sealController.signal.reason : error;
    }
  }

  async receiveServiceWireSessionRelocationRoute(
    value: ServiceSessionRelocationRoute
  ): Promise<void> {
    const key = serviceWireBarrierKey(value);
    const routeRequestFingerprint = encodeSessionRelocationRoute(value).toString('base64');
    const fingerprint = routeRequestFingerprint;
    const terminal = this.terminalServiceWireRelocations.get(key);
    if (terminal !== undefined) {
      if (terminal.routeFingerprint === undefined) {
        // The Session owner already closed this binding at the seal deadline.
        // A late command 44 is terminal and cannot reactivate the route.
        this.terminalServiceWireRelocations.touch(key);
        return;
      }
      if (terminal.routeFingerprint !== fingerprint) {
        throw new ServiceWireProtocolError(
          `Session relocation '${key}' repeated command 44 with different bytes.`
        );
      }
      this.terminalServiceWireRelocations.touch(key);
      return;
    }
    const state = this.activeServiceWireRelocations.get(key);
    if (state === undefined) {
      actorSessionBindingRuntimeOwner(this.options.streamBindingRuntime()).discardRelocationOutbound(
        value.actor.actorId,
        key,
        new ZLinkRemoteBoundSessionFenceError(
          `Actor '${value.actor.actorId}' command 44 did not match an active command 42.`
        )
      );
      return;
    }
    if (
      state.routeRequestFingerprint !== undefined
      && state.routeRequestFingerprint !== routeRequestFingerprint
    ) {
      throw new ServiceWireProtocolError(
        `Session relocation '${key}' repeated command 44 with different bytes.`
      );
    }
    if (state.routeFingerprint !== undefined) {
      if (state.routeFingerprint !== fingerprint || state.routePromise === undefined) {
        throw new ServiceWireProtocolError(
          `Session relocation '${key}' repeated command 44 with different bytes.`
        );
      }
      await state.routePromise;
      return;
    }
    state.routeRequestFingerprint = routeRequestFingerprint;
    state.routeFingerprint = fingerprint;
    if (state.sealTimer !== undefined) clearTimeout(state.sealTimer);
    state.sealAbortCleanup?.();
    const routePromise = state.sealPromise.then(async sealed => {
      validateSessionRelocationRouteAgainstSeal(value, sealed);
      await this.applyServiceWireSessionRelocationRoute(
        key,
        value
      );
    });
    state.routePromise = routePromise;
    try {
      await routePromise;
      this.terminalServiceWireRelocations.remember(key, {
        actorId: state.actorId,
        sealFingerprint: state.sealFingerprint,
        sealed: state.sealed ?? await state.sealPromise,
        routeRequestFingerprint,
        routeFingerprint: fingerprint
      });
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
      const sealed = state.sealed ?? await state.sealPromise;
      this.terminalServiceWireRelocations.remember(key, {
        actorId: state.actorId,
        sealFingerprint: state.sealFingerprint,
        sealed,
        routeRequestFingerprint,
        routeFingerprint: fingerprint
      });
      actorSessionBindingRuntimeOwner(this.options.streamBindingRuntime())
        .clearRelocation(state.actorId, error);
      await this.options.streamBindingRuntime().disconnectBoundSession(state.actorId)
        .catch(disconnectError => this.options.reportOwnershipRefreshError?.(
          state.actorId,
          disconnectError
        ));
      throw error;
    }
  }

  clearServiceWireSessionRelocations(): void {
    for (const state of this.activeServiceWireRelocations.values()) {
      if (state.sealTimer !== undefined) clearTimeout(state.sealTimer);
    }
    this.activeServiceWireRelocations.clear();
    this.terminalServiceWireRelocations.clear();
  }

  private async applyServiceWireSessionRelocationRoute(
    key: string,
    value: ServiceSessionRelocationRoute
  ): Promise<void> {
    const runtime = this.options.streamBindingRuntime();
    const owner = actorSessionBindingRuntimeOwner(runtime);
    const fingerprint = encodeSessionRelocationRoute(value).toString('base64');
    if (value.route.action === 'abort') {
      await owner.applyRelocation(
        value.actor.actorId,
        key,
        fingerprint,
        'abort',
        async () => {
          if (!runtime.abortActorRouteSeal(value.actor.actorId, key)) {
            throw new ZLinkRemoteBoundSessionFenceError(
              `Actor '${value.actor.actorId}' command 44 abort lost its Session seal.`
            );
          }
        }
      );
    } else {
      const committedRoute = value.route;
      const current = runtime.find(value.actor.actorId)?.ref;
      if (current === undefined || current.objectGeneration !== value.actor.generation) {
        throw new ZLinkRemoteBoundSessionFenceError(
          `Actor '${value.actor.actorId}' command 44 has no exact Session binding.`
        );
      }
      const targetActorRef = {
        actorId: value.actor.actorId,
        objectGeneration: value.actor.generation,
        meshName: current.meshName,
        nodeRid: decodeWireRoutingId(committedRoute.targetNodeRid, undefined),
        bindingGeneration: value.session.bindingGeneration,
        ownershipGeneration: committedRoute.targetAuthorityOwnerGeneration,
        ownerNodeGeneration: committedRoute.targetNodeGeneration
      } as ActorRef;
      await owner.applyRelocation(
        value.actor.actorId,
        key,
        fingerprint,
        'commit',
        async () => {
          await runtime.commitActorRoute(targetActorRef, undefined, {
            confirmRemoteSessionBinding: 'send',
            releaseSeal: { sealId: key }
          });
          this.actorOwnershipGenerations.set(
            value.actor.actorId,
            committedRoute.targetAuthorityOwnerGeneration
          );
        }
      );
    }
    owner.observeRelocationTerminal(
      value.actor.actorId,
      key,
      fingerprint
    );
  }

  private validateServiceWireSessionRoute(value: ServiceSessionRelocationSeal): void {
    const current = this.options.streamBindingRuntime().sessionRouteFence(
      value.actor.actor.actorId
    );
    if (
      current === undefined
      || current.actor.objectGeneration !== value.actor.actor.generation
      || !routingIdsEqual(current.actor.nodeRid, value.actor.actor.nodeRid)
      || current.sessionRid.toString() !== value.session.sessionRid
      || current.bindingGeneration !== value.session.bindingGeneration
    ) {
      throw new ZLinkRemoteBoundSessionFenceError(
        `Actor '${value.actor.actor.actorId}' command 42 was fenced by its exact Session route.`
      );
    }
  }

  rememberRemoteBoundSessionTarget(actorId: string, target: ZLinkRemoteBoundSessionTarget | undefined): void {
    this.options.actorManager()?.getState(actorId)?.setRemoteBoundSessionTarget(target);
  }

  resolveRemoteBoundSessionTarget(
    sourceNodeRid: RoutingId,
    _sourceSessionRid: RoutingId
  ): ZLinkRemoteBoundSessionTarget | undefined {
    return this.options.meshRouters.remoteBoundSessionTargetForSource(sourceNodeRid);
  }

  actorPacketTargetForState(
    actorId: string,
    routerChannelIdHint?: string
  ): ZLinkRemoteActorPacketTarget | undefined {
    return this.options.actorPacketTargetForState(actorId, routerChannelIdHint);
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
    for (const [key, value] of this.terminalServiceWireRelocations) {
      if (value.actorId === actorId) this.terminalServiceWireRelocations.delete(key);
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
    if (this.options.streamBindingRuntime().sendLocalBoundSessionResponse(
      actor.context.actorId,
      packetName,
      requestSeq,
      response,
      replyOptions.metadata,
      replyOptions.compressPayload
    )) {
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
        replyOptions,
        signal
      );
      return;
    }
    if (actorRef === undefined) {
      throw new Error(`Actor '${actor.context.actorId}' does not have a native actor ref.`);
    }
    const actorSessionNode = this.options.actorSessionNode?.(actor.context.actorId);
    if (actorSessionNode === undefined) {
      throw new Error('Native bound-session response requires the RouteMesh stream-session service.');
    }
    await this.options.streamBindingRuntime().sendNativeBoundSessionResponse(
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
    if (this.options.streamBindingRuntime().sendLocalBoundSessionError(
      actorId,
      packetName,
      requestSeq,
      error,
      metadata
    )) {
      return;
    }
    const state = this.options.actorManager()?.getState(actorId);
    const actorRef = currentActorRef(state, fallbackActorRef)
      ?? this.options.destroyedActorRefs.get(actorId);
    const remoteTarget = fallbackBoundSessionTarget ?? state?.remoteBoundSessionTarget;
    if (remoteTarget !== undefined) {
      await this.sendRemoteBoundSessionError(
        remoteTarget,
        actorId,
        packetName,
        requestSeq,
        error,
        metadata,
        signal
      );
      return;
    }
    if (actorRef === undefined) {
      throw new Error(`Actor '${actorId}' does not have a native actor ref.`);
    }
    const actorSessionNode = this.options.actorSessionNode?.(actorId);
    if (actorSessionNode === undefined) {
      throw new Error('Native bound-session error response requires the RouteMesh stream-session service.');
    }
    await this.options.streamBindingRuntime().sendNativeBoundSessionError(
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
    replyOptions: ZLinkActorResponseOptions,
    signal?: AbortSignal
  ): Promise<void> {
    const actorPacketTarget = encodeRemoteActorPacketTarget(
      this.options.actorPacketTargetForState(actorId, target.routerChannelId)
    );
    await this.sendRemoteBoundSessionControl(target, encodeRemoteBoundSessionResponsePayload({
      actorId,
      boundPacketName: packetName,
      requestSeq,
      message,
      metadata: replyOptions.metadata,
      compressPayload: replyOptions.compressPayload,
      actorPacketTarget
    }), signal);
  }

  private async sendRemoteBoundSessionError(
    target: ZLinkRemoteBoundSessionTarget,
    actorId: string,
    packetName: string,
    requestSeq: bigint,
    error: unknown,
    metadata: ReadonlyMap<string, string>,
    signal?: AbortSignal
  ): Promise<void> {
    await this.sendRemoteBoundSessionControl(target, encodeRemoteBoundSessionErrorPayload({
      actorId,
      boundPacketName: packetName,
      requestSeq,
      error,
      metadata,
      actorPacketTarget: encodeRemoteActorPacketTarget(
        this.options.actorPacketTargetForState(actorId, target.routerChannelId)
      )
    }), signal);
  }

  private async sendRemoteBoundSessionControl(
    target: ZLinkRemoteBoundSessionTarget,
    payload: Record<string, unknown>,
    signal?: AbortSignal
  ): Promise<void> {
    const packetName = payload.packetName;
    if (typeof packetName !== 'string') {
      throw new Error('Remote bound session control payload does not declare a packet name.');
    }
    const submit = this.options.routeTransport.submitInfrastructure
      ?? this.options.routeTransport.submit;
    if (submit === undefined) {
      throw new Error('Remote bound session node-direct transport is not available.');
    }
    const result = await submit.call(
      this.options.routeTransport,
      target.routerChannelId,
      String(target.targetNodeRid),
      packetName,
      payload,
      signal
    );
    if (result.status !== ZLinkSubmitStatus.Submitted) {
      throw new Error(
        `Remote bound session control '${packetName}' was not admitted`
        + ` on '${target.routerChannelId}' for '${String(target.targetNodeRid)}'`
        + ` (status ${result.status}).`
      );
    }
  }
}

function currentActorRef(
  state: {
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
  } | undefined,
  fallback: ActorRef | undefined
): ActorRef | undefined {
  const native = state?.nativeActorRef;
  if (native === undefined) return fallback;
  const objectGeneration = native.generation
    ?? native.objectGeneration
    ?? fallback?.objectGeneration;
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
  const actor = 'targetNodeGeneration' in value.actor
    ? value.actor.actor
    : value.actor;
  return [
    value.relocation.high.toString(),
    value.relocation.low.toString(),
    actor.actorId,
    actor.generation.toString(),
    value.session.sessionRid,
    value.session.bindingGeneration.toString()
  ].join(':');
}

function validateSessionRelocationRouteAgainstSeal(
  route: ServiceSessionRelocationRoute,
  sealed: ServiceSessionRelocationSealed
): void {
  const sameCoordinator =
    route.coordinator.ownerId === sealed.coordinator.ownerId
    && route.coordinator.leaseGeneration === sealed.coordinator.leaseGeneration
    && route.coordinator.nodeRid === sealed.coordinator.nodeRid
    && route.coordinator.nodeGeneration === sealed.coordinator.nodeGeneration
    && route.coordinator.expectedAuthorityStoreVersion
      === sealed.coordinator.expectedAuthorityStoreVersion;
  const sameSession =
    route.session.sessionOwnerNodeRid === sealed.session.sessionOwnerNodeRid
    && route.session.sessionOwnerNodeGeneration === sealed.session.sessionOwnerNodeGeneration
    && route.session.sessionOwnerId === sealed.session.sessionOwnerId
    && route.session.sessionOwnerLeaseGeneration
      === sealed.session.sessionOwnerLeaseGeneration
    && route.session.sessionRid === sealed.session.sessionRid
    && route.session.bindingGeneration === sealed.session.bindingGeneration;
  if (
    route.relocation.high !== sealed.relocation.high
    || route.relocation.low !== sealed.relocation.low
    || !sameCoordinator
    || !sameSession
    || route.actor.actorId !== sealed.actor.actor.actorId
    || route.actor.generation !== sealed.actor.actor.generation
  ) {
    throw new ServiceWireProtocolError(
      `Session relocation '${serviceWireBarrierKey(route)}' changed its command 42 fence.`
    );
  }
  if (route.route.action === 'commit') {
    if (route.route.previousAuthorityOwnerGeneration
      !== sealed.actor.authorityOwnerGeneration) {
      throw new ServiceWireProtocolError(
        `Session relocation '${serviceWireBarrierKey(route)}' changed its source authority fence.`
      );
    }
    return;
  }
  if (route.route.currentAuthorityOwnerGeneration !== sealed.actor.authorityOwnerGeneration) {
    throw new ServiceWireProtocolError(
      `Session relocation '${serviceWireBarrierKey(route)}' abort changed its authority fence.`
    );
  }
}
