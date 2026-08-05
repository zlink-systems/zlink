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
import { normalizeRoutingId, routingIdsEqual } from '../routing-id';
import { ZLinkSubmitStatus } from '../messaging/submission-result';
import type { DefaultZLinkSpotManager, ZLinkSpotNodeRuntimeManager } from '../spots';
import type { ZLinkDetachedTaskRunner } from '../spots/spot-actor-join-dispatch';
import type { ZLinkBoundSessionResponseTarget } from '../streams';
import type {
  ZLinkBoundSessionResponsePort,
  ZLinkStreamActorLookupPort
} from '../streams/stream-binding-runtime-ports';
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
import type { ZLinkResolvedActorRoute, ZLinkStoreLocationResolvers } from '../locations';

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
      state?.spotId === undefined ? undefined : state.remoteBoundSessionTarget;
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

  updateRemoteActorPacketTarget(actorId: string, value: unknown): void {
    this.targets.updateFromWire(actorId, value);
  }

  actorPacketTargetForState(
    actorId: string,
    routerChannelIdHint?: string
  ): ZLinkRemoteActorPacketTarget | undefined {
    return this.targets.targetForState(actorId, routerChannelIdHint);
  }

  async notifyActorDisconnectedById(actorId: string, signal?: AbortSignal): Promise<void> {
    const state = this.options.actorManager()?.getState(actorId);
    const remoteTarget = state?.remoteBoundSessionTarget ?? state?.remoteActorPacketTarget;
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
    const header = RuntimeMessage.from(Buffer.from(relay.header, 'base64'));
    const body = RuntimeMessage.from(Buffer.from(relay.payload, 'base64'));
    let closeFrameMessages = true;
    try {
      const frameHeader = decodeStreamHeader(messageToBytes(header));
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
      const relayGeneration = relay.actorGeneration ?? relay.bindingActorGeneration;
      const fallbackActorRef = relayNodeRid === undefined
        || relayGeneration === undefined
        ? undefined
        : messageFollowContext === undefined
          ? {
              actorId: relay.actorId,
              objectGeneration: BigInt(relayGeneration),
              meshName: _routeContext.meshName,
              nodeRid: normalizeRoutingId(relayNodeRid),
              ...(relay.bindingGeneration === undefined
                ? {}
                : { bindingGeneration: BigInt(relay.bindingGeneration) })
            }
          : attachActorMessageFollowContext({
              actorId: relay.actorId,
              objectGeneration: BigInt(relayGeneration),
              meshName: _routeContext.meshName,
              nodeRid: normalizeRoutingId(relayNodeRid),
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
        void dispatch.catch((error) =>
          this.options.errorSink().reportRuntimeTaskException('remote actor packet relay', error)
        ).finally(() => {
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
    if (await this.relayRemoteActorPacket(actor, frameHeader, payload, signal)) {
      return true;
    }
    return await this.relayLocalActorPacket(actor, frameHeader, payload, signal);
  }

  async confirmRemoteSessionBinding(
    actor: ActorRef,
    sessionNodeRid: RoutingId,
    sessionRid: RoutingId,
    signal?: AbortSignal
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
    const reply = await this.requestRemoteTarget<{
      readonly ok?: boolean;
      readonly error?: unknown;
      readonly acknowledged?: boolean;
      readonly response?: { readonly acknowledged?: boolean };
    }>(
      { ...target, spotKind: target.spotKind ?? ZLinkSpotKind.Entry },
      request,
      `Remote actor session binding is not available for '${actor.actorId}'.`,
      (parts) => JSON.parse(parts[0]?.getString('utf8') ?? '{}'),
      signal
    );
    if (reply.ok === false || (reply.response?.acknowledged !== true && reply.acknowledged !== true)) {
      throw new Error(
        `Actor '${actor.actorId}' did not acknowledge its remote session binding: ${JSON.stringify(reply)}`
      );
    }
  }

  async relayRemoteActorPacket(
    actor: ZLinkSessionActor,
    frameHeader: ZLinkStreamFrameHeader,
    payload: Message,
    signal?: AbortSignal
  ): Promise<boolean> {
    const responseTarget = this.options.streamBindingRuntime().captureBoundSessionResponseTarget(actor);
    const localNode = this.options.spotNodeRuntime()?.primaryMeshNode;
    const localNodeRid = localNode === undefined ? undefined : String(localNode.status().routingId);
    const resolvedRoute = await this.options.actorLocationResolver?.()
      ?.resolveDirectActorRoute(actor.actorId, signal);
    if (
      resolvedRoute !== undefined
      && localNodeRid !== undefined
      && routingIdsEqual(resolvedRoute.actorRef.nodeRid, localNodeRid)
      && this.options.actorManager()?.getState(actor.actorId)?.actor !== undefined
    ) {
      return false;
    }
    const remoteTarget = resolvedRoute === undefined
      ? this.options.actorManager()?.getState(actor.actorId)?.remoteActorPacketTarget
        ?? this.targets.cachedTargetForActor(actor)
      : actorPacketTargetFromResolvedRoute(resolvedRoute);
    if (remoteTarget === undefined) {
      return false;
    }
    // Session Actor relay is a one-way submit. The target sends a response or
    // error through the bound-session route after its handler reaches a
    // terminal state; the source stream must not wait for that handler.
    const returnResponse = false;
    const header = RuntimeMessage.from(Buffer.from(encodeStreamHeader(frameHeader)));
    let request: Record<string, unknown>;
    try {
      const messageFollowContext = resolvedRoute === undefined
        ? undefined
        : createInitialActorMessageFollowContext(
            resolvedRoute,
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
        bindingActorRef: actor.ref,
        header: messageToBytes(header),
        payload: messageToBytes(payload),
        returnResponse,
        actorRef: resolvedRoute?.actorRef,
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
    if (
      actorPacketTarget !== undefined &&
      (localNodeRid === undefined || !routingIdsEqual(actorPacketTarget.targetNodeRid, localNodeRid))
    ) {
      this.targets.rememberActorTarget(actor, actorPacketTarget);
    } else if (reply.ok !== false) {
      this.targets.clear(actor.actorId);
    }
    if (reply.deferredResponse === true && reply.ok !== false) {
      return true;
    }
    if (reply.ok === false) {
      const sent = this.sendCapturedOrCurrentBoundSessionError(
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
    const sent = this.sendCapturedOrCurrentBoundSessionResponse(
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
    const current = this.options.actorManager()?.getState(relay.actorId)?.remoteBoundSessionTarget;
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
    return await requestRoutedJsonReply(
      this.options.routeTransport,
      { ...target, spotKind: target.spotKind ?? ZLinkSpotKind.Entry },
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
    const responseTarget = this.options.streamBindingRuntime().captureBoundSessionResponseTarget(actor);
    const header = RuntimeMessage.from(Buffer.from(encodeStreamHeader(frameHeader)));
    const body = RuntimeMessage.from(Buffer.from(messageToBytes(payload)));
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
          const sent = this.sendCapturedOrCurrentBoundSessionResponse(
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
          const sent = this.sendCapturedOrCurrentBoundSessionError(
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

  private sendCapturedOrCurrentBoundSessionResponse(
    target: ZLinkBoundSessionResponseTarget | undefined,
    actorId: string,
    packetName: string,
    requestSeq: bigint,
    response: unknown,
    metadata: ReadonlyMap<string, string>
  ): boolean {
    return target?.sendResponse(packetName, requestSeq, response, metadata)
      ?? this.options.streamBindingRuntime().sendLocalBoundSessionResponse(
        actorId,
        packetName,
        requestSeq,
        response,
        metadata,
        false
      );
  }

  private sendCapturedOrCurrentBoundSessionError(
    target: ZLinkBoundSessionResponseTarget | undefined,
    actorId: string,
    packetName: string,
    requestSeq: bigint,
    error: unknown,
    metadata: ReadonlyMap<string, string>
  ): boolean {
    return target?.sendError(packetName, requestSeq, error, metadata)
      ?? this.options.streamBindingRuntime().sendLocalBoundSessionError(
        actorId,
        packetName,
        requestSeq,
        error,
        metadata
      );
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

function actorPacketTargetFromResolvedRoute(
  route: ZLinkResolvedActorRoute
): ZLinkRemoteActorPacketTarget {
  if (route.spotId !== undefined) {
    if (route.enclosingSpotRoute === undefined) {
      throw createInternalFrameworkException(
        ZLinkFrameworkInternalErrorKind.ActorLocationStale,
        `Actor '${route.actorRef.actorId}' enclosing Spot authority is not Ready.`,
        true
      );
    }
    return route.enclosingSpotRoute;
  }
  return {
    routerChannelId: route.meshName,
    targetNodeRid: route.actorRef.nodeRid,
    spotId: route.actorRef.nodeRid,
    spotKind: ZLinkSpotKind.Entry
  };
}

function isValidMessageFollowId(value: string | undefined): value is string {
  return value !== undefined
    && /^[0-9a-f]{32}$/.test(value)
    && !/^0+$/.test(value);
}
