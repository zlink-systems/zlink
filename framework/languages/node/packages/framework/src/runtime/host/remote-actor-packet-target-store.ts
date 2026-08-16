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
import { decodeRemoteActorPacketTarget } from '../actors/actor-packet-relay-wire';
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

interface ZLinkSessionActorPacketTargetCacheEntry {
  readonly routeKey: string;
  readonly target: ZLinkRemoteActorPacketTarget;
}

export class ZLinkRemoteActorPacketTargetStore {
  private readonly sessionActorPacketTargets =
    new WeakMap<ZLinkSessionActor, ZLinkSessionActorPacketTargetCacheEntry>();
  private readonly sessionActorPacketTargetsByActor = new Map<string, ZLinkRemoteActorPacketTarget>();
  private readonly sessionActorPacketTargetsByActorId =
    new Map<string, ZLinkSessionActorPacketTargetCacheEntry>();
  private readonly sessionActorPacketTargetOwners = new Map<string, {
    readonly actors: Set<ZLinkSessionActor>;
    readonly keys: Set<string>;
  }>();

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
        this.rememberSessionActorTarget(sessionActor, actorPacketTarget);
      }
      return;
    }
    this.clear(actorId);
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
    const owner = this.sessionActorPacketTargetOwners.get(actorId);
    if (owner !== undefined) {
      for (const actor of owner.actors) {
        this.sessionActorPacketTargets.delete(actor);
      }
      for (const key of owner.keys) {
        this.sessionActorPacketTargetsByActor.delete(key);
      }
      this.sessionActorPacketTargetOwners.delete(actorId);
    }
  }

  cachedTargetForActor(actor: ZLinkSessionActor): ZLinkRemoteActorPacketTarget | undefined {
    const routeKey = sessionActorPacketTargetTenureKey(actor);
    const actorEntry = this.sessionActorPacketTargets.get(actor);
    const actorIdEntry = this.sessionActorPacketTargetsByActorId.get(actor.actorId);
    return (actorEntry?.routeKey === routeKey ? actorEntry.target : undefined)
      ?? this.sessionActorPacketTargetsByActor.get(routeKey)
      ?? (actorIdEntry?.routeKey === routeKey ? actorIdEntry.target : undefined)
      ?? this.targetForActorRef(actor.ref);
  }

  targetForState(actorId: string, routerChannelIdHint?: string): ZLinkRemoteActorPacketTarget | undefined {
    const state = this.options.actorManager()?.getState(actorId);
    if (
      state?.remoteActorPacketTarget !== undefined &&
      (state.spotId === undefined
        || routingIdsEqual(state.remoteActorPacketTarget.spotId, state.spotId))
    ) {
      return state.remoteActorPacketTarget;
    }
    const spotId = state?.spotId;
    if (spotId !== undefined && state?.remoteActorPacketTarget !== undefined) {
      //  The cached packet target refers to a different Spot than the
      //  actor's current membership (the actor just moved Entry -> User).
      //  Combining the stale target's node with the new Spot id fabricates
      //  a route with no complete Ready authority fence, and the next
      //  direct request then fails the fence check instead of resolving
      //  the current owner route (spec 12 §direct payload uses the
      //  Location Store's current owner route; spec 32:87 — an unavailable
      //  route is Unavailable, not a fabricated hit). Publish no hint so
      //  the source clears its cache and re-resolves completely.
      return undefined;
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
    this.rememberSessionActorTarget(actor, target);
  }

  private rememberSessionActorTarget(
    actor: ZLinkSessionActor,
    target: ZLinkRemoteActorPacketTarget
  ): void {
    let owner = this.sessionActorPacketTargetOwners.get(actor.actorId);
    if (owner === undefined) {
      owner = { actors: new Set(), keys: new Set() };
      this.sessionActorPacketTargetOwners.set(actor.actorId, owner);
    }
    const key = sessionActorPacketTargetTenureKey(actor);
    const entry = { routeKey: key, target };
    owner.actors.add(actor);
    owner.keys.add(key);
    this.sessionActorPacketTargets.set(actor, entry);
    this.sessionActorPacketTargetsByActor.set(key, target);
    this.sessionActorPacketTargetsByActorId.set(actor.actorId, entry);
  }
}

function sessionActorPacketTargetTenureKey(actor: ZLinkSessionActor): string {
  const ref = actor.ref as ActorRef & {
    readonly bindingGeneration?: bigint;
    readonly ownershipGeneration?: bigint;
    readonly ownerLeaseGeneration?: bigint;
  };
  return [
    String(ref.nodeRid),
    actor.actorId,
    String(ref.objectGeneration),
    ref.bindingGeneration?.toString() ?? '',
    ref.ownershipGeneration?.toString() ?? '',
    ref.ownerLeaseGeneration?.toString() ?? ''
  ].join(':');
}

function validateSpotId(value: string): SpotId {
  const byteLength = Buffer.byteLength(value, 'utf8');
  if (byteLength < 1 || byteLength > 255) {
    throw new TypeError('SpotId must contain between 1 and 255 UTF-8 bytes.');
  }
  return value;
}
