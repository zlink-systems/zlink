import type {
  ActorRef,
  RoutingId,
  SpotId,
  ZLinkSessionActor
} from '../../contracts';
import { ZLinkSpotKind } from '../../contracts';
import type {
  DefaultZLinkActorManager,
  ZLinkRemoteActorPacketTarget
} from '../actors';
import type { ZLinkStreamActorLookupPort } from '../streams/stream-binding-runtime-ports';
import {
  decodeRemoteActorPacketTarget,
  sessionActorPacketTargetKey
} from '../actors/actor-packet-relay-wire';
import { normalizeRoutingId as normalizeRuntimeRoutingId } from '../routing-id';
import type { MeshRouterResolver } from './mesh-router-resolver';
import { routingIdsEqual } from '../routing-id';

export interface ZLinkRemoteActorPacketTargetStoreOptions {
  readonly actorManager: () => DefaultZLinkActorManager | undefined;
  readonly streamBindingRuntime: () => ZLinkStreamActorLookupPort;
  readonly meshRouters: MeshRouterResolver;
  readonly primaryNodeRid: () => RoutingId | undefined;
  readonly spotRouterChannelIdForMesh: (meshName: string) => string;
}

export class ZLinkRemoteActorPacketTargetStore {
  private readonly sessionActorPacketTargets = new WeakMap<ZLinkSessionActor, ZLinkRemoteActorPacketTarget>();
  private readonly sessionActorPacketTargetsByActor = new Map<string, ZLinkRemoteActorPacketTarget>();
  private readonly sessionActorPacketTargetsByActorId = new Map<string, ZLinkRemoteActorPacketTarget>();

  constructor(private readonly options: ZLinkRemoteActorPacketTargetStoreOptions) {}

  updateFromWire(actorId: string, value: unknown): void {
    const actorPacketTarget = this.decodeFromWire(value);
    const state = this.options.actorManager()?.getState(actorId);
    if (actorPacketTarget !== undefined) {
      if (typeof state?.setRemoteActorPacketTarget === 'function') {
        state.setRemoteActorPacketTarget(actorPacketTarget);
      }
      const sessionActor = this.options.streamBindingRuntime().find(actorId);
      if (sessionActor !== undefined) {
        this.sessionActorPacketTargets.set(sessionActor, actorPacketTarget);
        this.sessionActorPacketTargetsByActor.set(
          sessionActorPacketTargetKey(sessionActor),
          actorPacketTarget
        );
      }
      this.sessionActorPacketTargetsByActorId.set(actorId, actorPacketTarget);
      return;
    }
    if (typeof state?.setRemoteActorPacketTarget === 'function') {
      state.setRemoteActorPacketTarget(undefined);
    }
    this.sessionActorPacketTargetsByActorId.delete(actorId);
  }

  decodeFromWire(value: unknown): ZLinkRemoteActorPacketTarget | undefined {
    return decodeRemoteActorPacketTarget(value);
  }

  clear(actorId: string): void {
    const state = this.options.actorManager()?.getState(actorId);
    if (typeof state?.setRemoteActorPacketTarget === 'function') {
      state.setRemoteActorPacketTarget(undefined);
    }
    this.sessionActorPacketTargetsByActorId.delete(actorId);
    const sessionActor = this.options.streamBindingRuntime().find(actorId);
    if (sessionActor !== undefined) {
      this.sessionActorPacketTargets.delete(sessionActor);
      this.sessionActorPacketTargetsByActor.delete(sessionActorPacketTargetKey(sessionActor));
    }
    const actorRef = state?.nativeActorRef;
    if (actorRef !== undefined) {
      this.sessionActorPacketTargetsByActor.delete(
        `${String(actorRef.nodeRid)}:${actorId}:${String(actorRef.generation)}`
      );
    }
  }

  cachedTargetForActor(actor: ZLinkSessionActor): ZLinkRemoteActorPacketTarget | undefined {
    return this.sessionActorPacketTargets.get(actor)
      ?? this.sessionActorPacketTargetsByActor.get(sessionActorPacketTargetKey(actor))
      ?? this.sessionActorPacketTargetsByActorId.get(actor.actorId)
      ?? this.targetForActorRef(actor.ref);
  }

  targetForState(actorId: string, routerChannelIdHint?: string): ZLinkRemoteActorPacketTarget | undefined {
    const state = this.options.actorManager()?.getState(actorId);
    if (
      state?.remoteActorPacketTarget !== undefined &&
      (state.spotId === undefined || state.remoteActorPacketTarget.spotId === state.spotId)
    ) {
      return state.remoteActorPacketTarget;
    }
    const spotId = state?.spotId;
    if (spotId !== undefined && state?.remoteActorPacketTarget !== undefined) {
      return {
        routerChannelId: state.remoteActorPacketTarget.routerChannelId,
        targetNodeRid: state.remoteActorPacketTarget.targetNodeRid,
        spotId: validateSpotId(spotId),
        spotKind: ZLinkSpotKind.User,
        ...(state.spotGeneration === undefined
          ? {}
          : { targetSpotGeneration: state.spotGeneration })
      };
    }
    const actorRef = state?.nativeActorRef as ActorRef | undefined;
    const targetNodeRid = actorRef?.nodeRid as RoutingId | undefined
      ?? this.options.primaryNodeRid();
    const routerChannelId = routerChannelIdHint
      ?? this.options.meshRouters.defaultSpotRouterChannelId()
      ?? this.options.meshRouters.defaultRouterChannelId();
    const localNodeRid = this.options.primaryNodeRid();
    if (
      spotId === undefined &&
      targetNodeRid !== undefined &&
      localNodeRid !== undefined &&
      routingIdsEqual(targetNodeRid, localNodeRid)
    ) {
      return undefined;
    }
    if (spotId === undefined || targetNodeRid === undefined || routerChannelId === undefined) {
      return undefined;
    }
    return {
      routerChannelId,
      targetNodeRid: normalizeRuntimeRoutingId(targetNodeRid),
      spotId: validateSpotId(spotId),
      spotKind: ZLinkSpotKind.User,
      ...(state?.spotGeneration === undefined
        ? {}
        : { targetSpotGeneration: state.spotGeneration })
    };
  }

  targetForActorRef(actorRef: ActorRef): ZLinkRemoteActorPacketTarget | undefined {
    const targetNodeRid = actorRef.nodeRid as RoutingId;
    const localNodeRid = this.options.primaryNodeRid();
    if (localNodeRid !== undefined && routingIdsEqual(localNodeRid, targetNodeRid)) {
      return undefined;
    }
    const meshName = actorRef.meshName;
    const routerChannelId = meshName.trim().length === 0
      ? this.options.meshRouters.defaultSpotRouterChannelId()
        ?? this.options.meshRouters.defaultRouterChannelId()
      : this.options.spotRouterChannelIdForMesh(meshName);
    if (routerChannelId === undefined) {
      return undefined;
    }
    return {
      routerChannelId,
      targetNodeRid,
      spotId: targetNodeRid,
      spotKind: ZLinkSpotKind.Entry
    };
  }

  rememberActorTarget(actor: ZLinkSessionActor, target: ZLinkRemoteActorPacketTarget): void {
    const state = this.options.actorManager()?.getState(actor.actorId);
    if (typeof state?.setRemoteActorPacketTarget === 'function') {
      state.setRemoteActorPacketTarget(target);
    }
    this.sessionActorPacketTargets.set(actor, target);
    this.sessionActorPacketTargetsByActor.set(sessionActorPacketTargetKey(actor), target);
    this.sessionActorPacketTargetsByActorId.set(actor.actorId, target);
  }
}

function validateSpotId(value: string): SpotId {
  const byteLength = Buffer.byteLength(value, 'utf8');
  if (byteLength < 1 || byteLength > 255) {
    throw new TypeError('SpotId must contain between 1 and 255 UTF-8 bytes.');
  }
  return value;
}
