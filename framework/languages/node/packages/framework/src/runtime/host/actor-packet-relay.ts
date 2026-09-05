import { ZLinkFrameworkInternalErrorKind, createInternalFrameworkException, internalFrameworkErrorKind  } from '../framework-errors-internal';
import { ZLinkBufferMessage as RuntimeMessage } from '../backend/runtime-message';
import type {
  ActorRef,
  RoutingId,
  ZLinkRouteMessageContext,
  ZLinkSessionActor
} from '../../contracts';
import {
  ZLinkFrameworkException,
  ZLinkSpotKind
} from '../../contracts';
import { ZLinkConfigurationException } from '../../contracts/Configuration/ConfigurationException';
import type { Message } from '../../contracts/Common/Message';
import {
  ZLINK_REMOTE_ACTOR_PACKET_RELAY_PACKET,
  ZLINK_REMOTE_ACTOR_SESSION_DISCONNECTED_PACKET,
  type ZLinkActorRoutedJoinTransport,
  type ZLinkRemoteActorPacketTarget,
  type ZLinkRemoteBoundSessionTarget
} from '../actors';
import {
  mergeRemoteBoundSessionTarget,
  preferredRemoteBoundSessionTarget
} from '../actors/actor-runtime-state';
import type { DefaultZLinkActorManager } from '../actors';
import {
  decodeRemoteActorSessionBinding,
  decodeRemoteActorPacketRelayPayload,
  encodeRemoteActorSessionBinding,
  encodeRemoteActorPacketRelayPayload,
  encodeRemoteActorPacketTarget,
  ZLINK_REMOTE_ACTOR_SESSION_BIND_PACKET
} from '../actors/actor-packet-relay-wire';
import { requestRoutedJsonReply } from '../actors/actor-routed-json-request';
import {
  attachActorMessageFollowContext,
  createInitialActorMessageFollowContext,
  createMessageFollowId,
  verifyActorMessageFollowPayload
} from '../actors/actor-message-follow-context';
import { streamMetadataMap } from '../actors/bound-session-wire';
import { decodeRoutingId, normalizeRoutingId, routingIdsEqual } from '../routing-id';
import { ZLinkSubmitStatus } from '../messaging/submission-result';
import type { DefaultZLinkSpotManager, ZLinkSpotNodeRuntimeManager } from '../spots';
import type { ZLinkSpotRouteTarget } from '../spots/spot-routing-internal';
import type { ZLinkDetachedTaskRunner } from '../spots/spot-actor-join-dispatch';
import type { ZLinkBoundSessionResponseTarget } from '../streams';
import type {
  ZLinkBoundSessionResponsePort,
  ZLinkStreamActorLookupPort
} from '../streams/stream-binding-runtime-ports';
import { actorSessionBindingRuntimeOwnerIfRegistered } from '../streams/actor-session-binding-runtime-owner';
import {
  decodeStreamHeader,
  encodeStreamHeader,
  messageToBytes,
  type ZLinkStreamFrameHeader,
  ZLinkStreamCodec,
  ZLinkStreamHeaderFlags,
  ZLinkStreamMessageKind
} from '../streams/protocol';
import type { MeshRouterResolver } from './mesh-router-resolver';
import { ZLinkRemoteActorPacketTargetStore } from './remote-actor-packet-target-store';
import type { ZLinkStoreLocationResolvers } from '../locations';

const REMOTE_SESSION_BIND_RETRY_DEADLINE_MS = 30_000;
const REMOTE_SESSION_BIND_RETRY_INITIAL_DELAY_MS = 25;
const REMOTE_SESSION_BIND_RETRY_MAX_DELAY_MS = 1_000;

export interface ZLinkActorPacketRelayOptions {
  readonly requestTimeoutMs?: number;
  readonly routeTransport: ZLinkActorRoutedJoinTransport;
  readonly streamBindingRuntime: () => ZLinkBoundSessionResponsePort & ZLinkStreamActorLookupPort;
  readonly meshRouters: MeshRouterResolver;
  readonly actorManager: () => DefaultZLinkActorManager | undefined;
  readonly spotManager: () => DefaultZLinkSpotManager | undefined;
  readonly spotNodeRuntime: () => ZLinkSpotNodeRuntimeManager | undefined;
  readonly detachedTaskRunner: ZLinkDetachedTaskRunner;
  readonly errorSink: () => { reportRuntimeTaskException(taskName: string, error: unknown): void };
  readonly actorLocationResolver?: () => ZLinkStoreLocationResolvers | undefined;
  readonly flowCreationEnabled?: () => boolean;
  readonly actorErrorSender?: (
    actorId: string,
    packetName: string,
    requestSeq: bigint,
    error: unknown,
    metadata: ReadonlyMap<string, string>,
    fallbackBoundSessionTarget?: ZLinkRemoteBoundSessionTarget,
    fallbackActorRef?: ActorRef
  ) => Promise<void>;
}

export class ZLinkActorPacketRelay {
  private readonly targets: ZLinkRemoteActorPacketTargetStore;

  constructor(private readonly options: ZLinkActorPacketRelayOptions) {
    const spotRouterChannelIdByMesh = (
      options.meshRouters as unknown as {
        spotRouterChannelIdByMesh?: () => (meshName: string) => string;
      }
    ).spotRouterChannelIdByMesh;
    this.targets = new ZLinkRemoteActorPacketTargetStore({
      actorManager: options.actorManager,
      streamBindingRuntime: options.streamBindingRuntime,
      meshRouters: options.meshRouters,
      primaryNodeRid: () => {
        const node = options.spotNodeRuntime()?.primaryMeshNode;
        return node === undefined ? undefined : String(node.status().routingId);
      },
      spotRouterChannelIdForMesh: spotRouterChannelIdByMesh?.call(options.meshRouters)
        ?? ((meshName) => meshName)
    });
  }

  async notifyBoundActorDisconnected(actor: ZLinkSessionActor, signal?: AbortSignal): Promise<void> {
    const state = this.options.actorManager()?.getState(actor.actorId);
    const currentRemoteBoundSessionTarget =
      state?.spotId === undefined
        ? undefined
        : preferredRemoteBoundSessionTarget(
            state.remoteBoundSessionTarget,
            state.boundSessionTransferTarget
          );
    const currentRemoteActorPacketTarget =
      state?.spotId === undefined ? undefined : state.remoteActorPacketTarget;
    const remoteTarget = currentRemoteBoundSessionTarget
      ?? currentRemoteActorPacketTarget
      ?? (state?.spotId === undefined ? undefined : this.targets.cachedTargetForActor(actor));
    if (remoteTarget !== undefined) {
      await this.notifyRemoteActorDisconnected(actor.actorId, remoteTarget, signal, actor.ref);
      return;
    }
    const actorRefTarget = this.targets.targetForActorRef(actor.ref);
    if (actorRefTarget !== undefined) {
      await this.notifyRemoteActorDisconnected(actor.actorId, actorRefTarget, signal, actor.ref);
      return;
    }
    if (state?.spotId !== undefined && state.actor !== undefined) {
      const handled = await this.requireSpotManager().notifyJoinedSpotActorDisconnected(
        state.spotId,
        state.actor,
        signal
      );
      if (handled) {
        return;
      }
    }
    await this.notifyActorDisconnectedById(actor.actorId, signal);
  }

  clearRemoteActorPacketTarget(actorId: string): void {
    this.targets.clear(actorId);
  }

  async updateRemoteActorPacketTarget(actorId: string, value: unknown): Promise<void> {
    await this.targets.updateFromWire(actorId, value);
  }

  actorPacketTargetForState(
    actorId: string,
    routerChannelIdHint?: string
  ): ZLinkRemoteActorPacketTarget | undefined {
    return this.targets.targetForState(actorId, routerChannelIdHint);
  }

  async notifyActorDisconnectedById(actorId: string, signal?: AbortSignal): Promise<void> {
    const state = this.options.actorManager()?.getState(actorId);
    const remoteTarget = state === undefined
      ? undefined
      : preferredRemoteBoundSessionTarget(
          state.remoteBoundSessionTarget,
          state.boundSessionTransferTarget
        ) ?? state.remoteActorPacketTarget;
    if (remoteTarget !== undefined) {
      await this.notifyRemoteActorDisconnected(actorId, remoteTarget, signal);
      return;
    }
    await this.notifyLocalActorDisconnectedById(actorId, signal);
  }

  async notifyLocalActorDisconnectedById(actorId: string, signal?: AbortSignal): Promise<void> {
    const state = this.options.actorManager()?.getState(actorId);
    if (state?.spotId !== undefined && state.actor !== undefined) {
      const handled = await this.requireSpotManager().notifyJoinedSpotActorDisconnected(
        state.spotId,
        state.actor,
        signal
      );
      if (handled) {
        return;
      }
    }
    const localActor = state?.actor;
    if (localActor === undefined) {
      throw new Error(`Actor '${actorId}' does not have a local actor instance.`);
    }
    const meshName = state?.meshName;
    if (meshName === undefined) {
      throw new Error(`Actor '${actorId}' does not have a RouteMesh identity.`);
    }
    await this.requireSpotNodeRuntime().notifyEntrySpotActorDisconnected(
      meshName,
      localActor,
      signal
    );
  }

  async receiveRemoteActorPacketRelay(
    payload: unknown,
    _routeContext: ZLinkRouteMessageContext
  ): Promise<{
    readonly ok: boolean;
    readonly error?: unknown;
    readonly errorKind?: ZLinkFrameworkInternalErrorKind;
    readonly response?: unknown;
    readonly deferredResponse?: boolean;
    readonly actorPacketTarget?: unknown;
  }> {
    const relay = decodeRemoteActorPacketRelayPayload(payload);
    const remoteBoundSessionTarget: ZLinkRemoteBoundSessionTarget | undefined =
      relay.routerChannelId === undefined ||
      relay.boundSessionTargetNodeRid === undefined ||
      relay.boundSessionSpotId === undefined
        ? undefined
        : {
            routerChannelId: relay.routerChannelId,
            targetNodeRid: normalizeRoutingId(relay.boundSessionTargetNodeRid),
            spotId: normalizeRoutingId(relay.boundSessionSpotId)
          };
    const header = RuntimeMessage.fromOwned(Buffer.from(relay.header, 'base64'));
    const body = RuntimeMessage.fromOwned(Buffer.from(relay.payload, 'base64'));
    let closeFrameMessages = true;
    try {
      const frameHeader = decodeStreamHeader(
        messageToBytes(header),
        this.options.flowCreationEnabled?.() ?? true
      );
      const messageFollowContext = relay.messageFollowContext;
      if (messageFollowContext !== undefined) {
        if (messageFollowContext.deadlineUnixMs !== undefined
            && Date.now() >= messageFollowContext.deadlineUnixMs) {
          throw createInternalFrameworkException(
            ZLinkFrameworkInternalErrorKind.DeadlineExceeded,
            `Actor request '${relay.actorId}' exceeded its Message Follow deadline.`
          );
        }
        verifyActorMessageFollowPayload(messageFollowContext, [header, body]);
      }
      const relayNodeRid = relay.actorNodeRid ?? relay.bindingActorNodeRid;
      const relayNodeRidHex = relay.actorNodeRid === undefined
        ? relay.bindingActorNodeRidHex
        : relay.actorNodeRidHex;
      const relayGeneration = relay.actorGeneration ?? relay.bindingActorGeneration;
      const fallbackActorRef = relayNodeRid === undefined
        || relayGeneration === undefined
        ? undefined
        : messageFollowContext === undefined
          ? {
              actorId: relay.actorId,
              objectGeneration: BigInt(relayGeneration),
              meshName: _routeContext.meshName,
              nodeRid: decodeRoutingId(relayNodeRid, relayNodeRidHex),
              ...(relay.bindingGeneration === undefined
                ? {}
                : { bindingGeneration: BigInt(relay.bindingGeneration) })
            }
          : attachActorMessageFollowContext({
              actorId: relay.actorId,
              objectGeneration: BigInt(relayGeneration),
              meshName: _routeContext.meshName,
              nodeRid: decodeRoutingId(relayNodeRid, relayNodeRidHex),
              ...(relay.bindingGeneration === undefined
                ? {}
                : { bindingGeneration: BigInt(relay.bindingGeneration) })
            }, messageFollowContext);
      if (frameHeader.name === ZLINK_REMOTE_ACTOR_SESSION_BIND_PACKET) {
        if (frameHeader.kind !== ZLinkStreamMessageKind.Send) {
          throw new Error('Remote actor session binding requires a send frame.');
        }
        const { sessionNodeRid, sessionRid } = decodeRemoteActorSessionBinding(messageToBytes(body));
        if (!routingIdsEqual(sessionNodeRid, _routeContext.sourceNodeRid)) {
          throw new Error('Remote actor session binding source did not match the declared session node.');
        }
        const state = this.options.actorManager()?.getState(relay.actorId);
        const actorRef = state?.nativeActorRef;
        if (state === undefined || actorRef === undefined) {
          throw new Error(`Actor '${relay.actorId}' does not have a concrete actor ref.`);
        }
        if (this.requireSpotNodeRuntime().primaryMeshNode === undefined) {
          throw new Error('MeshNode actor runtime is not started.');
        }
        const target = relay.routerChannelId === undefined
          ? this.options.meshRouters.remoteBoundSessionTargetForSource(sessionNodeRid)
          : {
              routerChannelId: relay.routerChannelId,
              targetNodeRid: sessionNodeRid,
              spotId: sessionNodeRid
            };
        if (target === undefined) {
          throw new Error('Remote actor session binding did not declare a return router.');
        }
        const bindingGeneration = (fallbackActorRef as (ActorRef & {
          readonly bindingGeneration?: bigint;
        }) | undefined)?.bindingGeneration;
        const refreshedTarget = mergeRemoteBoundSessionTarget({
          ...target,
          sessionNodeRid,
          sessionRid,
          ...(bindingGeneration === undefined ? {} : { bindingGeneration })
        }, preferredRemoteBoundSessionTarget(
          state.remoteBoundSessionTarget,
          state.boundSessionTransferTarget
        ));
        state.setRemoteBoundSessionTarget(refreshedTarget);
        return { ok: true, response: { acknowledged: true } };
      }
      if (frameHeader.name === ZLINK_REMOTE_ACTOR_SESSION_DISCONNECTED_PACKET) {
        this.requireCurrentRemoteBinding(relay, _routeContext);
        this.options.actorManager()?.getState(relay.actorId)?.setRemoteBoundSessionTarget(undefined);
        await this.notifyLocalActorDisconnectedById(relay.actorId);
        return {
          ok: true,
          actorPacketTarget: encodeRemoteActorPacketTarget(
            this.actorPacketTargetForState(relay.actorId, relay.routerChannelId)
          )
        };
      }
      if (remoteBoundSessionTarget !== undefined) {
        this.requireCurrentRemoteBinding(relay, _routeContext);
      }
      const state = this.options.actorManager()?.getState(relay.actorId);
      if (frameHeader.kind === ZLinkStreamMessageKind.Send) {
        const dispatch = state?.spotId === undefined
          ? this.requireSpotNodeRuntime().dispatchEntryActorPacket(
              relay.actorId,
              [header, body],
              false,
              remoteBoundSessionTarget,
              fallbackActorRef
            )
          : this.requireSpotManager().dispatchRoutedActorPacket(
              state.spotId,
              relay.actorId,
              [header, body],
              false,
              remoteBoundSessionTarget,
              fallbackActorRef
            );
        // Spec 05 §1.3: a one-way Session Actor relay completes at relay
        // queue admission, not after the remote application turn. In
        // particular, a handler may register a deferred Join whose relocation
        // seal must drain the ingress frame that triggered this relay. Keeping
        // the route send open until that Join finalizes makes command 42 wait
        // on the very Actor turn that is waiting for relocation. The Actor
        // queue already owns the accepted frame here, so retain its messages
        // and observe the ordered handler terminal on a detached runtime task.
        closeFrameMessages = false;
        this.options.detachedTaskRunner.runDetached(
          'remote actor one-way packet relay',
          async () => {
            try {
              await dispatch;
            } catch (error) {
              this.options.errorSink().reportRuntimeTaskException(
                'remote actor packet relay',
                error
              );
            } finally {
              header.close();
              body.close();
            }
          }
        );
        return {
          ok: true,
          actorPacketTarget: encodeRemoteActorPacketTarget(
            this.actorPacketTargetForState(relay.actorId, relay.routerChannelId)
          )
        };
      }
      if (
        frameHeader.kind === ZLinkStreamMessageKind.Request
        && frameHeader.requestSeq !== undefined
        && relay.returnResponse !== true
      ) {
        const dispatch = state?.spotId === undefined
          ? this.requireSpotNodeRuntime().dispatchEntryActorPacket(
              relay.actorId,
              [header, body],
              false,
              remoteBoundSessionTarget,
              fallbackActorRef
            )
          : this.requireSpotManager().dispatchRoutedActorPacket(
              state.spotId,
              relay.actorId,
              [header, body],
              false,
              remoteBoundSessionTarget,
              fallbackActorRef
            );
        closeFrameMessages = false;
        const deferredPacketName = frameHeader.name;
        const deferredRequestSeq = frameHeader.requestSeq;
        const deferredMetadata = frameHeader.metadata;
        void dispatch.catch(async (error) => {
          //  Spec 32 §5 — an accepted Request must end in exactly one
          //  terminal completion. deferredResponse already told the source
          //  the reply will arrive on the bound-session route, so a routed
          //  dispatch REJECTED BEFORE DISPATCH (a closing/missing Spot
          //  throws ZLinkConfigurationException from activation resolution)
          //  must send its error there instead of only sinking it. Failures
          //  past that boundary already produce their own terminal through
          //  the inner actorErrorSender path — also sending here would emit
          //  a duplicate terminal for the same requestSeq.
          if (
            error instanceof ZLinkConfigurationException
            && this.options.actorErrorSender !== undefined
          ) {
            try {
              await this.options.actorErrorSender(
                relay.actorId,
                deferredPacketName,
                deferredRequestSeq,
                error,
                deferredMetadata,
                remoteBoundSessionTarget,
                fallbackActorRef
              );
              return;
            } catch (sendFailure) {
              this.options.errorSink().reportRuntimeTaskException(
                'remote actor packet relay error reply',
                sendFailure
              );
            }
          }
          this.options.errorSink().reportRuntimeTaskException('remote actor packet relay', error);
        }).finally(() => {
          header.close();
          body.close();
        });
        return {
          ok: true,
          deferredResponse: true,
          actorPacketTarget: encodeRemoteActorPacketTarget(
            this.actorPacketTargetForState(relay.actorId, relay.routerChannelId)
          )
        };
      }
      const response = state?.spotId === undefined
        ? await this.requireSpotNodeRuntime().dispatchEntryActorPacket(
            relay.actorId,
            [header, body],
            relay.returnResponse === true,
            remoteBoundSessionTarget,
            fallbackActorRef
          )
        : await this.requireSpotManager().dispatchRoutedActorPacket(
            state.spotId,
            relay.actorId,
            [header, body],
            relay.returnResponse === true,
            remoteBoundSessionTarget,
            fallbackActorRef
          );
      return {
        ok: true,
        response,
        actorPacketTarget: encodeRemoteActorPacketTarget(
          this.actorPacketTargetForState(relay.actorId, relay.routerChannelId)
        )
      };
    } catch (error) {
      return {
        ok: false,
        error: error instanceof Error ? error.message : String(error),
        errorKind: error instanceof ZLinkFrameworkException
          ? internalFrameworkErrorKind(error)
          : ZLinkFrameworkInternalErrorKind.RequestFailed
      };
    } finally {
      if (closeFrameMessages) {
        header.close();
        body.close();
      }
    }
  }

  async relayActorPacket(
    actor: ZLinkSessionActor,
    frameHeader: ZLinkStreamFrameHeader,
    payload: Message,
    signal?: AbortSignal
  ): Promise<boolean> {
    //  Spec 32:87 — a route/owner that is not currently usable is
    //  Unavailable and retryable within the request deadline. Right after a
    //  relocation the cached packet target can carry a transiently
    //  incomplete Ready authority fence (route publication still
    //  converging); failing the request on the fence check would surface a
    //  NotFound for a Spot that exists and is Ready on its owner. Clear the
    //  stale hint and re-resolve with a short bound instead.
    const fenceRetryDeadlineMs = performance.now() + 1_000;
    for (;;) {
      try {
        if (await this.relayRemoteActorPacket(actor, frameHeader, payload, signal)) {
          return true;
        }
        return await this.relayLocalActorPacket(actor, frameHeader, payload, signal);
      } catch (error) {
        if (
          !(error instanceof Error
            && error.message.includes('no complete Ready authority fence'))
          || performance.now() >= fenceRetryDeadlineMs
        ) {
          throw error;
        }
        this.targets.clear(actor.actorId);
        this.options.actorManager()?.getState(actor.actorId)
          ?.setRemoteActorPacketTarget(undefined);
        await new Promise<void>((resolve) => setTimeout(resolve, 25));
      }
    }
  }

  async confirmRemoteSessionBinding(
    actor: ActorRef,
    sessionNodeRid: RoutingId,
    sessionRid: RoutingId,
    signal?: AbortSignal,
    options?: { readonly waitForAcknowledgement?: boolean }
  ): Promise<void> {
    const target = this.targets.targetForActorRef(actor);
    if (target === undefined) return;
    const header = encodeStreamHeader({
      kind: ZLinkStreamMessageKind.Send,
      codec: ZLinkStreamCodec.Json,
      flags: ZLinkStreamHeaderFlags.None,
      name: ZLINK_REMOTE_ACTOR_SESSION_BIND_PACKET,
      metadata: new Map()
    });
    const request = encodeRemoteActorPacketRelayPayload({
      actorId: actor.actorId,
      routerChannelId: target.routerChannelId,
      boundSessionTargetNodeRid: String(sessionNodeRid),
      boundSessionSpotId: String(sessionNodeRid),
      bindingActorRef: actor,
      header,
      payload: encodeRemoteActorSessionBinding({ sessionNodeRid, sessionRid })
    });
    if (options?.waitForAcknowledgement === false) {
      await this.retryRemoteSessionBindingSend(
        withDefaultSpotKind(target),
        request,
        performance.now() + REMOTE_SESSION_BIND_RETRY_DEADLINE_MS,
        REMOTE_SESSION_BIND_RETRY_INITIAL_DELAY_MS
      );
      return;
    }
    const reply = await this.requestRemoteTarget<{
      readonly ok?: boolean;
      readonly error?: unknown;
      readonly errorKind?: unknown;
      readonly acknowledged?: boolean;
      readonly response?: { readonly acknowledged?: boolean };
    }>(
      withDefaultSpotKind(target),
      request,
      `Remote actor session binding is not available for '${actor.actorId}'.`,
      (parts) => JSON.parse(parts[0]?.getString('utf8') ?? '{}'),
      signal
    );
    if (reply.ok === false || (reply.response?.acknowledged !== true && reply.acknowledged !== true)) {
      //  An {ok:false} (or unacknowledged) reply here is an immediate remote
      //  rejection — the transport wait itself throws on timeout — so spec 32
      //  reserves DeadlineExceeded for the deadline path and this reply
      //  surfaces the remote failure's own classification instead: decode the
      //  reply's errorKind when it carries a valid framework kind, else
      //  RequestFailed, matching how other acknowledged relay replies map
      //  {ok:false} (actorRelayError / remoteRelayErrorKind).
      const remoteKind = Object.values(ZLinkFrameworkInternalErrorKind)
        .includes(reply.errorKind as ZLinkFrameworkInternalErrorKind)
        ? reply.errorKind as ZLinkFrameworkInternalErrorKind
        : ZLinkFrameworkInternalErrorKind.RequestFailed;
      throw createInternalFrameworkException(
        remoteKind,
        `Actor '${actor.actorId}' did not acknowledge its remote session binding: ${JSON.stringify(reply)}`,
        reply.error
      );
    }
  }

  private async retryRemoteSessionBindingSend(
    target: ZLinkSpotRouteTarget,
    request: unknown,
    deadline: number,
    delayMs: number
  ): Promise<void> {
    try {
      await this.options.routeTransport.sendToSpot(
        target,
        request,
        { packetName: ZLINK_REMOTE_ACTOR_PACKET_RELAY_PACKET }
      );
    } catch (error) {
      if (performance.now() >= deadline) {
        //  Spec 32-framework-error-model: DeadlineExceeded(7). Retry
        //  exhaustion on the one-way bind send remains diagnostics-only but
        //  the caller waits for its bounded submission terminal so later
        //  application relay cannot overtake it.
        this.options.errorSink().reportRuntimeTaskException(
          'remote session binding send',
          createInternalFrameworkException(
            ZLinkFrameworkInternalErrorKind.DeadlineExceeded,
            'Remote actor session binding send retries exceeded their deadline.',
            error
          )
        );
        return;
      }
      await new Promise<void>((resolve) => setTimeout(resolve, delayMs));
      await this.retryRemoteSessionBindingSend(
        target,
        request,
        deadline,
        Math.min(delayMs * 2, REMOTE_SESSION_BIND_RETRY_MAX_DELAY_MS)
      );
    }
  }

  async relayRemoteActorPacket(
    actor: ZLinkSessionActor,
    frameHeader: ZLinkStreamFrameHeader,
    payload: Message,
    signal?: AbortSignal
  ): Promise<boolean> {
    const streamRuntime = this.options.streamBindingRuntime();
    const responseTarget = await streamRuntime.captureBoundSessionResponseTarget(actor);
    const localNode = this.options.spotNodeRuntime()?.primaryMeshNode;
    const localNodeRid = localNode === undefined ? undefined : String(localNode.status().routingId);
    const storedRoute = await streamRuntime.sessionRouteFence(actor.actorId);
    const aggregateOwner = actorSessionBindingRuntimeOwnerIfRegistered(streamRuntime);
    const aggregateRoute = aggregateOwner === undefined
      ? undefined
      : await aggregateOwner.committedRoute(actor.actorId);
    const storedActorRef = aggregateRoute?.actor ?? storedRoute?.actor ?? actor.ref;
    const capturedTenureKey = this.targets.tenureKeyForActorRef(actor.actorId, storedActorRef);
    if (
      localNodeRid !== undefined
      && routingIdsEqual(storedActorRef.nodeRid, localNodeRid)
      && this.options.actorManager()?.getState(actor.actorId)?.actor !== undefined
    ) {
      return false;
    }
    const remoteTarget = this.options.actorManager()?.getState(actor.actorId)?.remoteActorPacketTarget
      ?? this.targets.cachedTargetForActor(actor);
    if (remoteTarget === undefined) {
      return false;
    }
    // Session Actor relay is a one-way submit. The target sends a response or
    // error through the bound-session route after its handler reaches a
    // terminal state; the source stream must not wait for that handler.
    const returnResponse = false;
    const encodedHeader = encodeStreamHeader(frameHeader);
    //  encodeStreamHeader returns a fresh, unaliased array; view it without re-copying.
    const header = RuntimeMessage.fromOwned(
      Buffer.from(encodedHeader.buffer, encodedHeader.byteOffset, encodedHeader.byteLength)
    );
    let request: Record<string, unknown>;
    try {
      const authority = aggregateRoute?.authorityFence;
      const messageFollowContext = authority?.ownerId === undefined
          || authority.ownerNodeGeneration === undefined
        ? undefined
        : createInitialActorMessageFollowContext(
            {
              meshName: storedActorRef.meshName,
              actorRef: storedActorRef,
              actorType: authority.actorType ?? '',
              ownerNodeGeneration: authority.ownerNodeGeneration,
              ownerId: authority.ownerId,
              ownerLeaseGeneration: authority.ownerLeaseGeneration,
              authorityOwnerGeneration: authority.authorityOwnerGeneration,
              authorityStoreVersion: authority.authorityStoreVersion ?? ''
            },
            [header, payload],
            returnResponse,
            undefined,
            isValidMessageFollowId(frameHeader.correlationId)
              ? frameHeader.correlationId
              : createMessageFollowId()
          );
      request = encodeRemoteActorPacketRelayPayload({
        actorId: actor.actorId,
        routerChannelId: remoteTarget.routerChannelId,
        boundSessionTargetNodeRid: localNodeRid === undefined ? undefined : String(localNodeRid),
        boundSessionSpotId: localNodeRid === undefined ? undefined : String(localNodeRid),
        bindingActorRef: storedActorRef,
        header: messageToBytes(header),
        payload: messageToBytes(payload),
        returnResponse,
        actorRef: storedActorRef,
        messageFollowContext
      });
    } finally {
      header.close();
    }
    const remoteAddress = {
      ...remoteTarget,
      routerChannelId: remoteTarget.routerChannelId,
      targetNodeRid: remoteTarget.targetNodeRid,
      spotId: remoteTarget.spotId,
      spotKind: remoteTarget.spotKind ?? ZLinkSpotKind.User
    };
    if (
      (frameHeader.kind === ZLinkStreamMessageKind.Send || frameHeader.requestSeq === undefined)
      && remoteAddress.spotKind === ZLinkSpotKind.Entry
      && this.options.routeTransport.submit !== undefined
    ) {
      const result = await this.options.routeTransport.submit(
        remoteAddress.routerChannelId,
        String(remoteAddress.targetNodeRid),
        ZLINK_REMOTE_ACTOR_PACKET_RELAY_PACKET,
        request,
        signal
      );
      if (result.status !== ZLinkSubmitStatus.Submitted) {
        throw new Error(`Actor '${actor.actorId}' remote node relay was not admitted.`);
      }
      return true;
    }
    if (frameHeader.kind === ZLinkStreamMessageKind.Send || frameHeader.requestSeq === undefined) {
      await this.options.routeTransport.sendToSpot(remoteAddress, request, {
        packetName: ZLINK_REMOTE_ACTOR_PACKET_RELAY_PACKET,
        signal
      });
      return true;
    }
    let reply: {
      readonly ok?: boolean;
      readonly error?: unknown;
      readonly response?: unknown;
      readonly deferredResponse?: boolean;
      readonly actorPacketTarget?: unknown;
    };
    reply = await this.requestRemoteTarget(
      remoteAddress,
      request,
      `Remote actor packet relay raw request is not available for '${actor.actorId}'.`,
      (parts) => {
        if (parts.length === 0) {
          throw new Error(`Remote actor packet relay reply was empty for '${actor.actorId}'.`);
        }
        return JSON.parse(parts[0].getString('utf8')) as typeof reply;
      },
      signal
    );
    const actorPacketTarget = this.targets.decodeFromWire(reply.actorPacketTarget);
    const currentStoredRoute = await streamRuntime.sessionRouteFence(actor.actorId);
    const currentAggregateRoute = aggregateOwner === undefined
      ? undefined
      : await aggregateOwner.committedRoute(actor.actorId);
    const currentActorRef = currentAggregateRoute?.actor ?? currentStoredRoute?.actor ?? actor.ref;
    const currentTenure = this.targets.tenureKeyForActorRef(actor.actorId, currentActorRef)
      === capturedTenureKey;
    if (
      currentTenure
      && this.targets.tenureKeyForActor(actor) === capturedTenureKey
      &&
      actorPacketTarget !== undefined &&
      (localNodeRid === undefined || !routingIdsEqual(actorPacketTarget.targetNodeRid, localNodeRid))
    ) {
      this.targets.rememberActorTarget(actor, actorPacketTarget, capturedTenureKey);
    } else if (currentTenure && reply.ok !== false) {
      this.targets.clear(actor.actorId, capturedTenureKey, actor);
    }
    if (reply.deferredResponse === true && reply.ok !== false) {
      return true;
    }
    if (reply.ok === false) {
      const sent = await this.sendCapturedOrCurrentBoundSessionError(
        responseTarget,
        actor.actorId,
        frameHeader.name,
        frameHeader.requestSeq,
        reply.error ?? 'Remote actor request failed.',
        streamMetadataMap(frameHeader.metadata)
      );
      if (!sent) {
        throw new Error(`Actor '${actor.actorId}' local bound session error response route is not ready.`);
      }
      return true;
    }
    const sent = await this.sendCapturedOrCurrentBoundSessionResponse(
      responseTarget,
      actor.actorId,
      frameHeader.name,
      frameHeader.requestSeq,
      reply.response,
      streamMetadataMap(frameHeader.metadata)
    );
    if (!sent) {
      throw new Error(`Actor '${actor.actorId}' local bound session response route is not ready.`);
    }
    return true;
  }

  async notifyRemoteActorDisconnected(
    actorId: string,
    remoteTarget: ZLinkRemoteBoundSessionTarget | ZLinkRemoteActorPacketTarget,
    signal?: AbortSignal,
    bindingActorRef?: ActorRef
  ): Promise<void> {
    const spotKind: ZLinkSpotKind | undefined =
      'spotKind' in remoteTarget ? remoteTarget.spotKind as ZLinkSpotKind | undefined : undefined;
    const header: ZLinkStreamFrameHeader = {
      kind: ZLinkStreamMessageKind.Send,
      codec: ZLinkStreamCodec.Raw,
      flags: ZLinkStreamHeaderFlags.None,
      name: ZLINK_REMOTE_ACTOR_SESSION_DISCONNECTED_PACKET,
      metadata: new Map()
    };
    const payload = encodeRemoteActorPacketRelayPayload({
      actorId,
      routerChannelId: remoteTarget.routerChannelId,
      bindingActorRef,
      header: encodeStreamHeader(header),
      payload: Buffer.alloc(0)
    });
    if ((spotKind ?? ZLinkSpotKind.Entry) === ZLinkSpotKind.Entry) {
      await this.options.routeTransport.request(
        remoteTarget.routerChannelId,
        String(remoteTarget.targetNodeRid),
        ZLINK_REMOTE_ACTOR_PACKET_RELAY_PACKET,
        payload,
        this.options.requestTimeoutMs,
        signal
      );
      return;
    }
    await this.options.routeTransport.requestToSpot(
      {
        routerChannelId: remoteTarget.routerChannelId,
        targetNodeRid: remoteTarget.targetNodeRid,
        spotId: remoteTarget.spotId,
        spotKind: spotKind ?? ZLinkSpotKind.Entry
      },
      payload,
      {
        packetName: ZLINK_REMOTE_ACTOR_PACKET_RELAY_PACKET,
        timeoutMs: this.options.requestTimeoutMs,
        signal
      }
    );
  }

  private requireCurrentRemoteBinding(
    relay: ReturnType<typeof decodeRemoteActorPacketRelayPayload>,
    routeContext: ZLinkRouteMessageContext
  ): void {
    const state = this.options.actorManager()?.getState(relay.actorId);
    const current = state === undefined
      ? undefined
      : preferredRemoteBoundSessionTarget(
          state.remoteBoundSessionTarget,
          state.boundSessionTransferTarget
        );
    if (
      current === undefined
      || current.sessionNodeRid === undefined
      || !routingIdsEqual(current.sessionNodeRid, routeContext.sourceNodeRid)
      || current.bindingGeneration === undefined
      || relay.bindingGeneration === undefined
      || current.bindingGeneration !== BigInt(relay.bindingGeneration)
    ) {
      throw createInternalFrameworkException(
        ZLinkFrameworkInternalErrorKind.ActorSessionNotBound,
        `Actor '${relay.actorId}' bound session identity is stale.`,
        true
      );
    }
  }

  private async requestRemoteTarget<TReply>(
    target: ZLinkRemoteActorPacketTarget,
    request: Record<string, unknown>,
    unavailableMessage: string,
    decodeReply: (parts: readonly Message[]) => TReply,
    signal?: AbortSignal
  ): Promise<TReply> {
    if (target.spotKind === ZLinkSpotKind.Entry) {
      try {
        return await this.options.routeTransport.request<TReply>(
          target.routerChannelId,
          String(target.targetNodeRid),
          ZLINK_REMOTE_ACTOR_PACKET_RELAY_PACKET,
          request,
          this.options.requestTimeoutMs,
          signal
        );
      } catch (error) {
        if (!(error instanceof ZLinkConfigurationException)) {
          throw error;
        }
      }
    }
    return requestRoutedJsonReply(
      this.options.routeTransport,
      withDefaultSpotKind(target),
      request,
      { timeoutMs: this.options.requestTimeoutMs, signal },
      unavailableMessage,
      decodeReply
    );
  }

  private async relayLocalActorPacket(
    actor: ZLinkSessionActor,
    frameHeader: ZLinkStreamFrameHeader,
    payload: Message,
    signal?: AbortSignal
  ): Promise<boolean> {
    void signal;
    const state = this.options.actorManager()?.getState(actor.actorId);
    const spotId = state?.spotId as RoutingId | undefined;
    const hasActiveSpot = spotId !== undefined && this.options.spotManager()?.hasActiveSpot(spotId) === true;
    if (!hasActiveSpot) {
      return false;
    }
    const actorRef = state?.nativeActorRef as ActorRef | undefined;
    const localNode = this.options.spotNodeRuntime()?.primaryMeshNode;
    const localNodeRid = localNode === undefined ? undefined : String(localNode.status().routingId);
    if (
      actorRef?.nodeRid !== undefined &&
      localNodeRid !== undefined &&
      !routingIdsEqual(actorRef.nodeRid as RoutingId, localNodeRid)
    ) {
      return false;
    }
    const responseTarget = await this.options.streamBindingRuntime().captureBoundSessionResponseTarget(actor);
    const encodedHeader = encodeStreamHeader(frameHeader);
    //  encodeStreamHeader returns a fresh, unaliased array; view it without re-copying.
    const header = RuntimeMessage.fromOwned(
      Buffer.from(encodedHeader.buffer, encodedHeader.byteOffset, encodedHeader.byteLength)
    );
    const body = RuntimeMessage.fromOwned(Buffer.from(messageToBytes(payload)));
    const returnResponse = frameHeader.kind === ZLinkStreamMessageKind.Request
      && frameHeader.requestSeq !== undefined;
    try {
      this.options.detachedTaskRunner.runDetached('local actor packet relay', async () => {
        try {
          const response = await this.requireSpotManager().dispatchRoutedActorPacket(
            spotId,
            actor.actorId,
            [header, body],
            returnResponse
          );
          if (!returnResponse) {
            return;
          }
          const sent = await this.sendCapturedOrCurrentBoundSessionResponse(
            responseTarget,
            actor.actorId,
            frameHeader.name,
            frameHeader.requestSeq,
            response,
            streamMetadataMap(frameHeader.metadata)
          );
          if (!sent) {
            throw new Error(`Actor '${actor.actorId}' local bound session response route is not ready.`);
          }
        } catch (error) {
          if (!returnResponse) {
            throw error;
          }
          const sent = await this.sendCapturedOrCurrentBoundSessionError(
            responseTarget,
            actor.actorId,
            frameHeader.name,
            frameHeader.requestSeq,
            error,
            streamMetadataMap(frameHeader.metadata)
          );
          if (!sent) {
            throw error;
          }
        } finally {
          header.close();
          body.close();
        }
      });
      return true;
    } catch (error) {
      header.close();
      body.close();
      throw error;
    }
  }

  private async sendCapturedOrCurrentBoundSessionResponse(
    target: ZLinkBoundSessionResponseTarget | undefined,
    actorId: string,
    packetName: string,
    requestSeq: bigint,
    response: unknown,
    metadata: ReadonlyMap<string, string>
  ): Promise<boolean> {
    return target === undefined
      ? await this.options.streamBindingRuntime().sendLocalBoundSessionResponse(
        actorId,
        packetName,
        requestSeq,
        response,
        metadata,
        false
      )
      : await target.sendResponse(packetName, requestSeq, response, metadata);
  }

  private async sendCapturedOrCurrentBoundSessionError(
    target: ZLinkBoundSessionResponseTarget | undefined,
    actorId: string,
    packetName: string,
    requestSeq: bigint,
    error: unknown,
    metadata: ReadonlyMap<string, string>
  ): Promise<boolean> {
    return target === undefined
      ? await this.options.streamBindingRuntime().sendLocalBoundSessionError(
        actorId,
        packetName,
        requestSeq,
        error,
        metadata
      )
      : await target.sendError(packetName, requestSeq, error, metadata);
  }

  private requireSpotNodeRuntime(): ZLinkSpotNodeRuntimeManager {
    const runtime = this.options.spotNodeRuntime();
    if (runtime === undefined) {
      throw new Error('SPOT node runtime is not started.');
    }
    return runtime;
  }

  private requireSpotManager(): DefaultZLinkSpotManager {
    const manager = this.options.spotManager();
    if (manager === undefined) {
      throw new Error('SPOT manager runtime is not started.');
    }
    return manager;
  }
}

function isValidMessageFollowId(value: string | undefined): value is string {
  return value !== undefined
    && /^[0-9a-f]{32}$/.test(value)
    && !/^0+$/.test(value);
}

/** Reuses the target when it already carries a spot kind instead of spreading a copy per call. */
function withDefaultSpotKind(
  target: ZLinkRemoteActorPacketTarget
): ZLinkRemoteActorPacketTarget & { readonly spotKind: ZLinkSpotKind } {
  return target.spotKind === undefined
    ? { ...target, spotKind: ZLinkSpotKind.Entry }
    : target as ZLinkRemoteActorPacketTarget & { readonly spotKind: ZLinkSpotKind };
}
