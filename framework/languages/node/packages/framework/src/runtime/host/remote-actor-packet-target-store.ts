import {
  type ActorRef,
  type RoutingId,
  type ZLinkSessionActor,
  ZLinkSpotKind
} from '../../contracts';

import type { DefaultZLinkActorManager, ZLinkRemoteActorPacketTarget } from '../actors';
import { decodeRemoteActorPacketTarget } from '../actors/actor-packet-relay-wire';
import { routingIdsEqual } from '../routing-id';
import type { ZLinkStreamActorLookupPort } from '../streams/stream-binding-runtime-ports';
import type { MeshRouterResolver } from './mesh-router-resolver';

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
  private readonly sessionActorPacketTargets = new WeakMap<
    ZLinkSessionActor,
    ZLinkSessionActorPacketTargetCacheEntry
  >();
  private readonly sessionActorPacketTargetsByActor = new Map<
    string,
    ZLinkRemoteActorPacketTarget
  >();
  private readonly sessionActorPacketTargetsByActorId = new Map<
    string,
    ZLinkSessionActorPacketTargetCacheEntry
  >();
  private readonly sessionActorPacketTargetOwners = new Map<
    string,
    {
      readonly actors: Set<ZLinkSessionActor>;
      readonly keys: Set<string>;
    }
  >();

  constructor(private readonly options: ZLinkRemoteActorPacketTargetStoreOptions) {}

  async updateFromWire(actorId: string, value: unknown): Promise<void> {
    const actorPacketTarget = this.decodeFromWire(value);
    const state = this.options.actorManager()?.getState(actorId);
    if (actorPacketTarget !== undefined) {
      if (typeof state?.setRemoteActorPacketTarget === 'function') {
        state.setRemoteActorPacketTarget(actorPacketTarget);
      }
      const sessionActor = await this.options.streamBindingRuntime().find(actorId);
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

  clear(actorId: string, expectedTenureKey?: string, expectedActor?: ZLinkSessionActor): void {
    if (
      expectedTenureKey !== undefined &&
      (expectedActor === undefined ||
        expectedActor.actorId !== actorId ||
        this.tenureKeyForActor(expectedActor) !== expectedTenureKey)
    )
      return;
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
    return (
      (actorEntry?.routeKey === routeKey ? actorEntry.target : undefined) ??
      this.sessionActorPacketTargetsByActor.get(routeKey) ??
      (actorIdEntry?.routeKey === routeKey ? actorIdEntry.target : undefined) ??
      this.targetForActorRef(actor.ref)
    );
  }

  targetForState(actorId: string): ZLinkRemoteActorPacketTarget | undefined {
    const state = this.options.actorManager()?.getState(actorId);
    if (
      state?.remoteActorPacketTarget !== undefined &&
      (state.spotId === undefined ||
        routingIdsEqual(state.remoteActorPacketTarget.spotId, state.spotId))
    ) {
      return state.remoteActorPacketTarget;
    }
    return undefined;
  }

  targetForActorRef(actorRef: ActorRef): ZLinkRemoteActorPacketTarget | undefined {
    const targetNodeRid = actorRef.nodeRid as RoutingId;
    if (this.isLocalActorRef(actorRef)) {
      return undefined;
    }
    const meshName = actorRef.meshName;
    const routerChannelId =
      meshName.trim().length === 0
        ? (this.options.meshRouters.defaultSpotRouterChannelId() ??
          this.options.meshRouters.defaultRouterChannelId())
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

  isLocalActorRef(actorRef: ActorRef): boolean {
    const localNodeRid = this.options.primaryNodeRid();
    return localNodeRid !== undefined && routingIdsEqual(localNodeRid, actorRef.nodeRid);
  }

  tenureKeyForActor(actor: ZLinkSessionActor): string {
    return sessionActorPacketTargetTenureKey(actor);
  }

  tenureKeyForActorRef(actorId: string, actorRef: ActorRef): string {
    return sessionActorPacketTargetTenureKeyForRef(actorId, actorRef);
  }

  rememberActorTarget(
    actor: ZLinkSessionActor,
    target: ZLinkRemoteActorPacketTarget,
    expectedTenureKey?: string
  ): void {
    if (expectedTenureKey !== undefined && this.tenureKeyForActor(actor) !== expectedTenureKey)
      return;
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
  return sessionActorPacketTargetTenureKeyForRef(actor.actorId, actor.ref);
}

function sessionActorPacketTargetTenureKeyForRef(actorId: string, actorRef: ActorRef): string {
  const ref = actorRef as ActorRef & {
    readonly bindingGeneration?: bigint;
    readonly ownershipGeneration?: bigint;
    readonly ownerLeaseGeneration?: bigint;
  };
  return [
    String(ref.nodeRid),
    actorId,
    String(ref.objectGeneration),
    ref.bindingGeneration?.toString() ?? '',
    ref.ownershipGeneration?.toString() ?? '',
    ref.ownerLeaseGeneration?.toString() ?? ''
  ].join(':');
}
