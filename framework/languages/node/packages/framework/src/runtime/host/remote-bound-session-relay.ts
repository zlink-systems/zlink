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
import type { ZLinkActorSessionRelocationSnapshot } from '../streams/actor-session-binding-registry';
import {
  actorSessionBindingRuntimeOwner,
  actorSessionBindingRuntimeOwnerIfRegistered
} from '../streams/actor-session-binding-runtime-owner';
import { ZLinkSubmitStatus } from '../messaging/submission-result';
import type { DefaultZLinkBoundSession } from '../streams/session-context';
import type { ZLinkActorResponseOptions } from '../spots/spot-actor-packet-dispatch';
import {
  decodeRemoteBoundSessionErrorPayload,
  decodeRemoteBoundSessionSealPayload,
  encodeRemoteBoundSessionOwnershipAck,
  decodeRemoteBoundSessionOwnershipPayload,
  decodeRemoteBoundSessionResponsePayload,
  decodeRemoteBoundSessionSendPayload,
  encodeRemoteBoundSessionErrorPayload,
  encodeRemoteBoundSessionResponsePayload,
  ZLINK_REMOTE_BOUND_SESSION_ABORT_SEAL_PACKET,
  ZLINK_REMOTE_BOUND_SESSION_OWNERSHIP_PACKET,
  ZLINK_REMOTE_BOUND_SESSION_SEAL_PACKET,
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
  encodeSessionRelocationRouted,
  encodeSessionRelocationSeal,
  type ServiceSessionRelocationRoute,
  type ServiceSessionRelocationRouted,
  type ServiceSessionRelocationSeal,
  type ServiceSessionRelocationSealed
} from '../foundation/service-stateful-wire-codec';
import { ServiceWireProtocolError } from '../foundation/service-wire-m6a-codec';
import { BoundedReplayMap } from './bounded-replay-map';

export interface ZLinkRemoteBoundSessionRelayOptions {
  readonly requestTimeoutMs?: number;
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
const SERVICE_CONTROL_ACTIVE_CAPACITY = 4096;
const SERVICE_CONTROL_ACTIVE_PER_ACTOR = 2;

interface ActiveServiceWireSessionRelocation {
  readonly actorId: string;
  readonly sealFingerprint: string;
  readonly sealPromise: Promise<ServiceSessionRelocationSealed>;
  sealed?: ServiceSessionRelocationSealed;
  routeRequestFingerprint?: string;
  targetOwnerLeaseGeneration?: bigint;
  routeFingerprint?: string;
  routePromise?: Promise<ServiceSessionRelocationRouted>;
}

interface TerminalServiceWireSessionRelocation {
  readonly actorId: string;
  readonly sealFingerprint: string;
  readonly sealed: ServiceSessionRelocationSealed;
  readonly routeRequestFingerprint: string;
  readonly targetOwnerLeaseGeneration?: bigint;
  readonly routeFingerprint: string;
  readonly routed: ServiceSessionRelocationRouted;
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
    this.options.updateRemoteActorPacketTarget(actorId, actorPacketTarget);
    const ownershipGeneration = (actorRef as (ActorRef & { ownershipGeneration?: bigint }) | undefined)
      ?.ownershipGeneration;
    const currentGeneration = this.actorOwnershipGenerations.get(actorId);
    if (
      currentGeneration !== undefined &&
      (ownershipGeneration === undefined || ownershipGeneration < currentGeneration)
    ) return;
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
      deliver: async () => await this.deliverRemoteBoundSessionSend(send),
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

  async receiveRemoteBoundSessionOwnership(
    payload: unknown,
    releaseMatchingSeal = false,
    sessionIdentity?: string,
    deferTerminalReceipt = false,
    targetNodeGeneration?: bigint
  ): Promise<{
    readonly actorId: string;
    readonly actorGeneration: string;
    readonly actorOwnershipGeneration: string;
    readonly bindingGeneration: string;
    readonly targetOwnerLeaseGeneration: string;
    readonly acceptedHighWater: string;
    readonly sealId: string;
  }> {
    const value = decodeRemoteBoundSessionOwnershipPayload(payload);
    const previousOwnershipGeneration = BigInt(value.previousActorOwnershipGeneration);
    const ownershipGeneration = BigInt(value.actorOwnershipGeneration);
    const bindingGeneration = BigInt(value.bindingGeneration);
    const previousOwnerLeaseGeneration = BigInt(value.previousOwnerLeaseGeneration);
    const targetOwnerLeaseGeneration = BigInt(value.targetOwnerLeaseGeneration);
    const acceptedHighWater = BigInt(value.acceptedHighWater);
    if (
      previousOwnershipGeneration < 0n ||
      ownershipGeneration <= previousOwnershipGeneration ||
      bindingGeneration <= 0n ||
      previousOwnerLeaseGeneration <= 0n ||
      targetOwnerLeaseGeneration <= 0n ||
      acceptedHighWater < 0n
    ) {
      throw new Error(`Actor '${value.actorId}' bound-session ownership fence is invalid.`);
    }
    const runtime = this.options.streamBindingRuntime();
    const owner = actorSessionBindingRuntimeOwner(runtime);
    const rememberedSeal = owner.relocationSnapshot(value.actorId, value.sealId);
    if (rememberedSeal !== undefined) {
      assertRemoteBoundSessionOwnershipClaim(
        rememberedSeal,
        value,
        sessionIdentity,
        !releaseMatchingSeal
      );
    }
    if (releaseMatchingSeal) {
      if (rememberedSeal === undefined) {
        throw new ZLinkRemoteBoundSessionFenceError(
          `Actor '${value.actorId}' command 44 did not match its command 42 Session seal.`
        );
      }
      const applyFingerprint = remoteBoundSessionOwnershipTerminalFingerprint(value);
      await owner.applyRelocation(
        value.actorId,
        value.sealId,
        acceptedHighWater,
        applyFingerprint,
        'commit',
        async () => await this.applyRemoteBoundSessionOwnership(
          value,
          true,
          targetNodeGeneration
        ),
        targetNodeGeneration === undefined || sessionIdentity === undefined
          ? undefined
          : {
              actorId: value.actorId,
              objectGeneration: BigInt(value.actorGeneration),
              actorNodeRid: String(decodeWireRoutingId(
                value.actorNodeRid,
                value.actorNodeRidHex
              )),
              actorNodeGeneration: targetNodeGeneration,
              authorityOwnerGeneration: ownershipGeneration,
              ownerLeaseGeneration: targetOwnerLeaseGeneration,
              sessionIdentity,
              bindingGeneration
            }
      );
      if (!deferTerminalReceipt) {
        owner.observeRelocationTerminal(
          value.actorId,
          value.sealId,
          acceptedHighWater,
          applyFingerprint
        );
      }
    } else {
      await this.applyRemoteBoundSessionOwnership(value, false);
      if (rememberedSeal?.phase === 'sealed') {
        owner.advanceRelocationOwner(
          value.actorId,
          value.sealId,
          previousOwnershipGeneration,
          previousOwnerLeaseGeneration,
          ownershipGeneration,
          targetOwnerLeaseGeneration
        );
      }
    }
    return encodeRemoteBoundSessionOwnershipAck(value);
  }

  private async applyRemoteBoundSessionOwnership(
    value: ReturnType<typeof decodeRemoteBoundSessionOwnershipPayload>,
    releaseMatchingSeal: boolean,
    targetNodeGeneration?: bigint
  ): Promise<void> {
    const previousOwnershipGeneration = BigInt(value.previousActorOwnershipGeneration);
    const ownershipGeneration = BigInt(value.actorOwnershipGeneration);
    const bindingGeneration = BigInt(value.bindingGeneration);
    const previousOwnerLeaseGeneration = BigInt(value.previousOwnerLeaseGeneration);
    const targetOwnerLeaseGeneration = BigInt(value.targetOwnerLeaseGeneration);
    const acceptedHighWater = BigInt(value.acceptedHighWater);
    const activeSeal = this.options.streamBindingRuntime().validateActorRouteSeal(
      value.actorId,
      value.sealId,
      acceptedHighWater
    );
    const rememberedSeal = actorSessionBindingRuntimeOwner(
      this.options.streamBindingRuntime()
    ).relocationSnapshot(
      value.actorId,
      value.sealId
    );
    const releasedSeal = rememberedSeal?.acceptedHighWater === acceptedHighWater
      && (rememberedSeal.phase === 'applied' || rememberedSeal.phase === 'terminal');
    if (!activeSeal && !releasedSeal) {
      throw new ZLinkRemoteBoundSessionFenceError(
        `Actor '${value.actorId}' command 44 did not match its command 42 Session seal.`
      );
    }
    const current = this.options.streamBindingRuntime().find(value.actorId)?.ref;
    const actorRef = {
      actorId: value.actorId,
      objectGeneration: BigInt(value.actorGeneration),
      meshName: value.meshName.length > 0 ? value.meshName : current?.meshName ?? '',
      nodeRid: decodeWireRoutingId(value.actorNodeRid, value.actorNodeRidHex),
      bindingGeneration,
      ownershipGeneration,
      ownerLeaseGeneration: targetOwnerLeaseGeneration,
      ...(targetNodeGeneration === undefined
        ? {}
        : { ownerNodeGeneration: targetNodeGeneration }),
      acceptedHighWater
    } as ActorRef;
    Object.defineProperty(actorRef, 'generation', {
      configurable: false,
      enumerable: false,
      value: actorRef.objectGeneration
    });
    if (
      current === undefined
      || current.objectGeneration !== actorRef.objectGeneration
    ) {
      throw new Error(`Actor '${value.actorId}' bound-session ownership update has no matching binding.`);
    }
    const currentFence = current as ActorRef & {
      readonly bindingGeneration?: bigint;
      readonly ownershipGeneration?: bigint;
      readonly ownerLeaseGeneration?: bigint;
    };
    const authorityFence = this.options.streamBindingRuntime().authorityFence(value.actorId);
    const currentOwnershipGeneration = this.actorOwnershipGenerations.get(value.actorId)
      ?? authorityFence?.authorityOwnerGeneration
      ?? currentFence.ownershipGeneration;
    const currentOwnerLeaseGeneration = authorityFence?.ownerLeaseGeneration
      ?? currentFence.ownerLeaseGeneration;
    const rememberedHighWaterMatches = rememberedSeal?.acceptedHighWater === acceptedHighWater;
    const targetAlreadyPublished =
      routingIdsEqual(current.nodeRid, actorRef.nodeRid) &&
      currentOwnershipGeneration === ownershipGeneration &&
      currentFence.bindingGeneration === bindingGeneration &&
      currentOwnerLeaseGeneration === targetOwnerLeaseGeneration &&
      (activeSeal || rememberedHighWaterMatches);
    if (targetAlreadyPublished) {
      if (releaseMatchingSeal && activeSeal
        && !this.options.streamBindingRuntime().abortActorRouteSeal(
          value.actorId,
          value.sealId
        )) {
        throw new ZLinkRemoteBoundSessionFenceError(
          `Actor '${value.actorId}' route switch lost its relocation seal.`
        );
      }
      this.options.updateRemoteActorPacketTarget(value.actorId, value.actorPacketTarget);
      return;
    }
    if (!activeSeal) {
      throw new ZLinkRemoteBoundSessionFenceError(
        `Actor '${value.actorId}' released Session seal cannot publish a different route.`
      );
    }
    if (
      currentOwnershipGeneration !== previousOwnershipGeneration ||
      currentFence.bindingGeneration !== bindingGeneration ||
      currentOwnerLeaseGeneration !== previousOwnerLeaseGeneration
    ) {
      throw new ZLinkRemoteBoundSessionFenceError(
        `Actor '${value.actorId}' bound-session ownership update was fenced by its binding identity ` +
        `(ownership=${currentOwnershipGeneration?.toString() ?? 'none'}/${previousOwnershipGeneration.toString()}, ` +
        `binding=${currentFence.bindingGeneration?.toString() ?? 'none'}/${bindingGeneration.toString()}, ` +
        `ownerLease=${currentOwnerLeaseGeneration?.toString() ?? 'none'}/${previousOwnerLeaseGeneration.toString()}).`
      );
    }
    try {
      await this.options.streamBindingRuntime().commitActorRoute(actorRef, undefined, {
        confirmRemoteSessionBinding: 'send',
        ...(releaseMatchingSeal
          ? {
              releaseSeal: {
                sealId: value.sealId,
                acceptedHighWater
              }
            }
          : {})
      });
    } catch (error) {
      this.options.reportOwnershipRefreshError?.(value.actorId, error);
      throw error;
    }
    this.options.updateRemoteActorPacketTarget(value.actorId, value.actorPacketTarget);
    this.actorOwnershipGenerations.set(value.actorId, ownershipGeneration);
  }

  async receiveRemoteBoundSessionSeal(
    payload: unknown,
    sessionIdentity?: string,
    deferTerminalReceipt = false,
    actorNodeRid?: string,
    actorNodeGeneration?: bigint
  ): Promise<{
    readonly actorId: string;
    readonly sealId: string;
    readonly acceptedHighWater: string;
  }> {
    const value = decodeRemoteBoundSessionSealPayload(payload);
    const runtime = this.options.streamBindingRuntime();
    const owner = actorSessionBindingRuntimeOwner(runtime);
    if (value.abort) {
      const remembered = owner.relocationSnapshot(
        value.actorId,
        value.sealId
      );
      if (remembered === undefined) {
        if (!runtime.abortActorRouteSeal(value.actorId, value.sealId)) {
          throw new ZLinkRemoteBoundSessionFenceError(
            `Actor '${value.actorId}' session route seal abort was fenced.`
          );
        }
        return { actorId: value.actorId, sealId: value.sealId, acceptedHighWater: '0' };
      }
      assertRemoteBoundSessionAbortClaim(remembered, value, sessionIdentity);
      const applyFingerprint = remoteBoundSessionAbortTerminalFingerprint(value);
      await owner.applyRelocation(
        value.actorId,
        value.sealId,
        remembered.acceptedHighWater,
        applyFingerprint,
        'abort',
        async () => {
          if (!runtime.abortActorRouteSeal(
            value.actorId,
            value.sealId
          )) {
            throw new ZLinkRemoteBoundSessionFenceError(
              `Actor '${value.actorId}' session route seal abort was fenced.`
            );
          }
        }
      );
      if (!deferTerminalReceipt) {
        owner.observeRelocationTerminal(
          value.actorId,
          value.sealId,
          remembered.acceptedHighWater,
          applyFingerprint
        );
      }
      return { actorId: value.actorId, sealId: value.sealId, acceptedHighWater: '0' };
    }
    const actorGeneration = BigInt(value.actorGeneration);
    const actorOwnershipGeneration = BigInt(value.actorOwnershipGeneration);
    const bindingGeneration = BigInt(value.bindingGeneration);
    const ownerLeaseGeneration = BigInt(value.ownerLeaseGeneration);
    const acceptedHighWater = await owner.sealRelocation(
      {
        actorId: value.actorId,
        actorGeneration,
        actorOwnershipGeneration,
        bindingGeneration,
        ownerLeaseGeneration,
        sealId: value.sealId,
        sessionIdentity,
        ...(actorNodeRid === undefined ? {} : { actorNodeRid }),
        ...(actorNodeGeneration === undefined ? {} : { actorNodeGeneration })
      },
      {
        objectGeneration: actorGeneration,
        authorityOwnerGeneration: actorOwnershipGeneration,
        bindingGeneration,
        ownerLeaseGeneration
      }
    );
    return {
      actorId: value.actorId,
      sealId: value.sealId,
      acceptedHighWater: acceptedHighWater.toString()
    };
  }

  async receiveServiceWireSessionRelocationSeal(
    value: ServiceSessionRelocationSeal
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
    let activeForActor = 0;
    for (const relocation of this.activeServiceWireRelocations.values()) {
      if (relocation.actorId === actorId) activeForActor += 1;
    }
    if (activeForActor >= SERVICE_CONTROL_ACTIVE_PER_ACTOR) {
      throw new ServiceWireProtocolError(
        `Actor '${actorId}' Session relocation successor capacity was exhausted.`
      );
    }
    if (this.activeServiceWireRelocations.size >= SERVICE_CONTROL_ACTIVE_CAPACITY) {
      throw new ServiceWireProtocolError(
        'Session relocation active control capacity was exhausted.'
      );
    }

    // Register the identity before the first asynchronous owner operation.
    // Concurrent identical commands join this promise; different bytes for
    // the same identity fail before they can install another seal.
    const sealPromise = Promise.resolve().then(async () => {
      this.validateServiceWireSessionRoute(value);
      const ack = await this.receiveRemoteBoundSessionSeal({
        packetName: ZLINK_REMOTE_BOUND_SESSION_SEAL_PACKET,
        actorId: value.actor.actor.actorId,
        actorGeneration: value.actor.actor.generation.toString(),
        actorOwnershipGeneration: value.actor.authorityOwnerGeneration.toString(),
        bindingGeneration: value.session.bindingGeneration.toString(),
        ownerLeaseGeneration: value.actor.ownerLeaseGeneration.toString(),
        sealId: key
      }, value.session.sessionRid, false, value.actor.actor.nodeRid, value.actor.targetNodeGeneration);
      return {
        relocation: value.relocation,
        coordinator: value.coordinator,
        actor: value.actor,
        session: value.session,
        lastAcceptedSessionSequence: BigInt(ack.acceptedHighWater)
      } satisfies ServiceSessionRelocationSealed;
    });
    const state: ActiveServiceWireSessionRelocation = {
      actorId,
      sealFingerprint: fingerprint,
      sealPromise
    };
    this.activeServiceWireRelocations.set(key, state);
    try {
      const sealed = await sealPromise;
      state.sealed = sealed;
      return sealed;
    } catch (error) {
      if (this.activeServiceWireRelocations.get(key) === state) {
        this.activeServiceWireRelocations.delete(key);
      }
      throw error;
    }
  }

  async receiveServiceWireSessionRelocationRoute(
    value: ServiceSessionRelocationRoute,
    targetOwnerLeaseGeneration?: bigint
  ): Promise<ServiceSessionRelocationRouted> {
    const key = serviceWireBarrierKey(value);
    const routeRequestFingerprint = encodeSessionRelocationRoute(value).toString('base64');
    const fingerprint = JSON.stringify([
      routeRequestFingerprint,
      targetOwnerLeaseGeneration?.toString() ?? null
    ]);
    const terminal = this.terminalServiceWireRelocations.get(key);
    if (terminal !== undefined) {
      if (terminal.routeFingerprint !== fingerprint) {
        throw new ServiceWireProtocolError(
          `Session relocation '${key}' repeated command 44 with different bytes or target proof.`
        );
      }
      this.terminalServiceWireRelocations.touch(key);
      return { ...terminal.routed, result: 'alreadyApplied' };
    }
    const state = this.activeServiceWireRelocations.get(key);
    if (state === undefined) {
      const current = this.options.streamBindingRuntime().sessionRouteFence(
        value.actor.actorId
      );
      actorSessionBindingRuntimeOwner(this.options.streamBindingRuntime()).discardRelocationOutbound(
        value.actor.actorId,
        key,
        new ZLinkRemoteBoundSessionFenceError(
          `Actor '${value.actor.actorId}' command 44 did not match an active command 42.`
        )
      );
      return {
        relocation: value.relocation,
        coordinator: value.coordinator,
        actor: value.actor,
        session: value.session,
        action: value.route.action,
        result: current === undefined ? 'sessionOrBindingClosed' : 'stale',
        // Command 44 never supplies evidence for an accepted boundary. Use
        // only the actual owner state when no matching command 42 exists.
        currentAuthorityOwnerGeneration: current?.authorityOwnerGeneration
          ?? (value.route.action === 'commit'
            ? value.route.previousAuthorityOwnerGeneration
            : value.route.currentAuthorityOwnerGeneration),
        lastAcceptedSessionSequence: current?.acceptedHighWater ?? 0n
      };
    }
    if (
      state.routeRequestFingerprint !== undefined
      && (
        state.routeRequestFingerprint !== routeRequestFingerprint
        || state.targetOwnerLeaseGeneration !== targetOwnerLeaseGeneration
      )
    ) {
      throw new ServiceWireProtocolError(
        `Session relocation '${key}' repeated command 44 with different bytes or target proof.`
      );
    }
    if (state.routeFingerprint !== undefined) {
      if (state.routeFingerprint !== fingerprint || state.routePromise === undefined) {
        throw new ServiceWireProtocolError(
          `Session relocation '${key}' repeated command 44 with different bytes or target proof.`
        );
      }
      const routed = await state.routePromise;
      return { ...routed, result: 'alreadyApplied' };
    }
    state.routeRequestFingerprint = routeRequestFingerprint;
    state.targetOwnerLeaseGeneration = targetOwnerLeaseGeneration;
    state.routeFingerprint = fingerprint;
    const routePromise = state.sealPromise.then(async sealed => {
      validateSessionRelocationRouteAgainstSeal(value, sealed);
      return await this.applyServiceWireSessionRelocationRoute(
        key,
        value,
        sealed,
        targetOwnerLeaseGeneration
      );
    });
    state.routePromise = routePromise;
    try {
      return await routePromise;
    } catch (error) {
      if (state.routePromise === routePromise) {
        state.routeFingerprint = undefined;
        state.routePromise = undefined;
      }
      throw error;
    }
  }

  serviceWireSessionRelocationRouteProof(
    value: ServiceSessionRelocationRoute
  ): bigint | undefined {
    const key = serviceWireBarrierKey(value);
    const routeRequestFingerprint = encodeSessionRelocationRoute(value).toString('base64');
    const active = this.activeServiceWireRelocations.get(key);
    if (
      active?.routeRequestFingerprint === routeRequestFingerprint
      && active.targetOwnerLeaseGeneration !== undefined
    ) {
      return active.targetOwnerLeaseGeneration;
    }
    const terminal = this.terminalServiceWireRelocations.get(key);
    if (
      terminal?.routeRequestFingerprint === routeRequestFingerprint
      && terminal.targetOwnerLeaseGeneration !== undefined
    ) {
      this.terminalServiceWireRelocations.touch(key);
      return terminal.targetOwnerLeaseGeneration;
    }
    return undefined;
  }

  async receiveServiceWireSessionRelocationRoutedReceipt(
    value: ServiceSessionRelocationRouted
  ): Promise<void> {
    const key = serviceWireBarrierKey(value);
    const terminal = this.terminalServiceWireRelocations.get(key);
    if (terminal !== undefined) {
      assertServiceWireRoutedReceipt(terminal.routed, value);
      this.terminalServiceWireRelocations.touch(key);
      return;
    }
    const state = this.activeServiceWireRelocations.get(key);
    if (state === undefined) {
      if (value.result === 'stale' || value.result === 'sessionOrBindingClosed') return;
      throw new ServiceWireProtocolError(
        `Session relocation '${key}' command 45 did not match an active command 44.`
      );
    }
    if (
      state.routePromise === undefined
      || state.routeFingerprint === undefined
      || state.routeRequestFingerprint === undefined
    ) {
      throw new ServiceWireProtocolError(
        `Session relocation '${key}' command 45 arrived before command 44 apply.`
      );
    }
    const routeRequestFingerprint = state.routeRequestFingerprint;
    const routed = await state.routePromise;
    assertServiceWireRoutedReceipt(routed, value);
    const sealed = state.sealed ?? await state.sealPromise;
    const owner = actorSessionBindingRuntimeOwner(this.options.streamBindingRuntime());
    const relocation = owner.relocationSnapshot(state.actorId, key);
    if (relocation?.applyFingerprint === undefined) {
      throw new ZLinkRemoteBoundSessionFenceError(
        `Actor '${state.actorId}' command 45 did not match its command 44 route apply.`
      );
    }
    owner.observeRelocationTerminal(
      state.actorId,
      key,
      sealed.lastAcceptedSessionSequence,
      relocation.applyFingerprint
    );
    const completed: TerminalServiceWireSessionRelocation = {
      actorId: state.actorId,
      sealFingerprint: state.sealFingerprint,
      sealed,
      routeRequestFingerprint,
      targetOwnerLeaseGeneration: state.targetOwnerLeaseGeneration,
      routeFingerprint: state.routeFingerprint,
      routed
    };
    if (this.activeServiceWireRelocations.get(key) === state) {
      this.activeServiceWireRelocations.delete(key);
    }
    this.terminalServiceWireRelocations.remember(key, completed);
  }

  clearServiceWireSessionRelocations(): void {
    this.activeServiceWireRelocations.clear();
    this.terminalServiceWireRelocations.clear();
  }

  private async applyServiceWireSessionRelocationRoute(
    key: string,
    value: ServiceSessionRelocationRoute,
    sealed: ServiceSessionRelocationSealed,
    targetOwnerLeaseGeneration?: bigint
  ): Promise<ServiceSessionRelocationRouted> {
    let currentAuthorityOwnerGeneration: bigint;
    if (value.route.action === 'abort') {
      await this.receiveRemoteBoundSessionSeal({
        packetName: ZLINK_REMOTE_BOUND_SESSION_ABORT_SEAL_PACKET,
        actorId: value.actor.actorId,
        actorGeneration: value.actor.generation.toString(),
        actorOwnershipGeneration: value.route.currentAuthorityOwnerGeneration.toString(),
        bindingGeneration: value.session.bindingGeneration.toString(),
        ownerLeaseGeneration: sealed.actor.ownerLeaseGeneration.toString(),
        sealId: key
      }, value.session.sessionRid, true);
      currentAuthorityOwnerGeneration = value.route.currentAuthorityOwnerGeneration;
    } else {
      if (targetOwnerLeaseGeneration === undefined || targetOwnerLeaseGeneration <= 0n) {
        throw new ServiceWireProtocolError(
          'Session relocation commit is missing the target owner lease generation.'
        );
      }
      const current = this.options.streamBindingRuntime().find(value.actor.actorId)?.ref;
      await this.receiveRemoteBoundSessionOwnership({
        packetName: ZLINK_REMOTE_BOUND_SESSION_OWNERSHIP_PACKET,
        actorId: value.actor.actorId,
        meshName: current?.meshName ?? '',
        actorNodeRid: value.route.targetNodeRid,
        actorGeneration: value.actor.generation.toString(),
        previousActorOwnershipGeneration:
          value.route.previousAuthorityOwnerGeneration.toString(),
        actorOwnershipGeneration: value.route.targetAuthorityOwnerGeneration.toString(),
        bindingGeneration: value.session.bindingGeneration.toString(),
        previousOwnerLeaseGeneration: sealed.actor.ownerLeaseGeneration.toString(),
        targetOwnerLeaseGeneration: targetOwnerLeaseGeneration.toString(),
        acceptedHighWater: value.route.replayedHighWater.toString(),
        sealId: key,
        acceptedJournalReference: '',
        acceptedJournalChecksumCrc32c: 0
      }, true, value.session.sessionRid, true, value.route.targetNodeGeneration);
      currentAuthorityOwnerGeneration = value.route.targetAuthorityOwnerGeneration;
    }
    const routed: ServiceSessionRelocationRouted = {
      relocation: value.relocation,
      coordinator: value.coordinator,
      actor: value.actor,
      session: value.session,
      action: value.route.action,
      result: 'applied',
      currentAuthorityOwnerGeneration,
      lastAcceptedSessionSequence: sealed.lastAcceptedSessionSequence
    };
    return routed;
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
      || current.authorityOwnerGeneration !== value.actor.authorityOwnerGeneration
      || current.ownerLeaseGeneration !== value.actor.ownerLeaseGeneration
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
      if (value.actorId === actorId) this.activeServiceWireRelocations.delete(key);
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
  value: ServiceSessionRelocationSeal | ServiceSessionRelocationRoute | ServiceSessionRelocationRouted
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

function assertServiceWireRoutedReceipt(
  expected: ServiceSessionRelocationRouted,
  actual: ServiceSessionRelocationRouted
): void {
  const acceptedResult = actual.result === 'applied' || actual.result === 'alreadyApplied';
  const expectedBytes = encodeSessionRelocationRouted({ ...expected, result: 'applied' });
  const actualBytes = encodeSessionRelocationRouted({ ...actual, result: 'applied' });
  if (!acceptedResult || !expectedBytes.equals(actualBytes)) {
    throw new ServiceWireProtocolError(
      `Session relocation '${serviceWireBarrierKey(actual)}' command 45 changed its exact route result.`
    );
  }
}

function assertRemoteBoundSessionOwnershipClaim(
  claim: ZLinkActorSessionRelocationSnapshot,
  value: ReturnType<typeof decodeRemoteBoundSessionOwnershipPayload>,
  sessionIdentity: string | undefined,
  allowAdvancedOwner: boolean
): void {
  const previousOwnerMatches = claim.actorOwnershipGeneration
      === BigInt(value.previousActorOwnershipGeneration)
    && claim.ownerLeaseGeneration === BigInt(value.previousOwnerLeaseGeneration);
  const targetOwnerMatches = allowAdvancedOwner
    && claim.actorOwnershipGeneration === BigInt(value.actorOwnershipGeneration)
    && claim.ownerLeaseGeneration === BigInt(value.targetOwnerLeaseGeneration);
  if (
    claim.actorGeneration !== BigInt(value.actorGeneration)
    || claim.bindingGeneration !== BigInt(value.bindingGeneration)
    || claim.acceptedHighWater !== BigInt(value.acceptedHighWater)
    || claim.sessionIdentity !== sessionIdentity
    || (!previousOwnerMatches && !targetOwnerMatches)
  ) {
    throw new ZLinkRemoteBoundSessionFenceError(
      `Actor '${value.actorId}' bound-session ownership update was fenced by its binding identity.`
    );
  }
}

function assertRemoteBoundSessionAbortClaim(
  claim: ZLinkActorSessionRelocationSnapshot,
  value: ReturnType<typeof decodeRemoteBoundSessionSealPayload>,
  sessionIdentity: string | undefined
): void {
  if (
    claim.actorGeneration !== BigInt(value.actorGeneration)
    || claim.actorOwnershipGeneration !== BigInt(value.actorOwnershipGeneration)
    || claim.bindingGeneration !== BigInt(value.bindingGeneration)
    || claim.ownerLeaseGeneration !== BigInt(value.ownerLeaseGeneration)
    || claim.sessionIdentity !== sessionIdentity
  ) {
    throw new ZLinkRemoteBoundSessionFenceError(
      `Actor '${value.actorId}' session route seal abort was fenced by its exact identity.`
    );
  }
}

function remoteBoundSessionOwnershipTerminalFingerprint(
  value: ReturnType<typeof decodeRemoteBoundSessionOwnershipPayload>
): string {
  return JSON.stringify([
    'ownership',
    value.actorId,
    value.actorGeneration,
    value.previousActorOwnershipGeneration,
    value.actorOwnershipGeneration,
    value.bindingGeneration,
    value.previousOwnerLeaseGeneration,
    value.targetOwnerLeaseGeneration,
    value.acceptedHighWater,
    value.sealId,
    value.actorNodeRid,
    value.actorNodeRidHex ?? '',
    value.acceptedJournalReference,
    value.acceptedJournalChecksumCrc32c,
    value.actorPacketTarget ?? null
  ]);
}

function remoteBoundSessionAbortTerminalFingerprint(
  value: ReturnType<typeof decodeRemoteBoundSessionSealPayload>
): string {
  return JSON.stringify([
    'abort',
    value.actorId,
    value.actorGeneration,
    value.actorOwnershipGeneration,
    value.bindingGeneration,
    value.ownerLeaseGeneration,
    value.sealId
  ]);
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
    if (
      route.route.previousAuthorityOwnerGeneration
        !== sealed.actor.authorityOwnerGeneration
      || route.route.replayedHighWater !== sealed.lastAcceptedSessionSequence
    ) {
      throw new ServiceWireProtocolError(
        `Session relocation '${serviceWireBarrierKey(route)}' changed its accepted boundary.`
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
