import { ZLinkFrameworkInternalErrorKind, createInternalFrameworkException, internalFrameworkErrorKind  } from '../framework-errors-internal';
import type { ActorRef, RoutingId, SpotId } from '../../contracts/Common';
import {
  ZLinkLocationRole,
  ZLinkLocationTopologyState,
  ZLinkFrameworkRuntimeState,
  ZLinkObjectRole,
  type ZLinkLocationReadiness,
  type ZLinkLocationRuntimeQuery,
  type ZLinkPeerLocationResolver,
  type ZLinkActorLocation,
  type ZLinkActorLocationKey,
  type ZLinkPeerLocation,
  type ZLinkPeerLocationFilter,
  type ZLinkRouteLocation,
  type ZLinkRouteLocationKey,
  type ZLinkSpotLocation,
  type ZLinkSpotLocationKey,
} from './internal-location-contracts';
import type {
  ZLinkActorLocationStore,
  ZLinkAuthorityStore,
  ZLinkMeshNodeLocationStore,
  ZLinkPeerLocationStore,
  ZLinkRouteLocationStore,
  ZLinkSpotLocationStore
} from './internal-store-contracts';
import { decodeServiceReadySpotAuthority } from '../foundation/service-authority-payload-codec';
import { decodeActorAuthorityIdentity } from '../actors/actor-authority-publication';
import { encodeAuthorityKey } from './authority-key-codec';
import {
  ZLinkSpotKind
} from '../../contracts/Spots';
import type {
  SpotHandle,
  ZLinkActorSpotHandleResolver,
  ZLinkSpotHandleResolver
} from '../spots/spot-handle';
import { ZLinkFrameworkException } from '../../contracts/Errors';
import type {
  ZLinkSpotRouteResolver,
  ZLinkSpotRouteTarget
} from '../spots/spot-routing-internal';
import { createSpotHandle, type ResolvedSpotHandle } from '../spots/spot-handle';
import {
  isKnownZLinkLocationAutoConnectType,
  isKnownZLinkLocationRole
} from './canonical-codec';
import {
  ZLinkLiveRowFilter,
  ZLinkOwnerLeaseTracker
} from './lease-tracker';
import { routingIdsEqual } from '../routing-id';

export interface ZLinkStoreLocationResolverStores {
  readonly authorityStore: ZLinkAuthorityStore;
  readonly locationStore: ZLinkMeshNodeLocationStore;
  readonly peerStore: ZLinkPeerLocationStore;
  readonly spotStore: ZLinkSpotLocationStore;
  readonly actorStore: ZLinkActorLocationStore;
  readonly routeStore: ZLinkRouteLocationStore;
}

export interface ZLinkLocationResolverEventSink {
  spotResolveMiss(key: ZLinkSpotLocationKey): void;
  actorResolveMiss(key: ZLinkActorLocationKey): void;
  routeResolveMiss(key: ZLinkRouteLocationKey): void;
}

export interface ZLinkStoreLocationResolversOptions {
  readonly stores: ZLinkStoreLocationResolverStores;
  readonly leaseTracker: ZLinkOwnerLeaseTracker;
  readonly events?: ZLinkLocationResolverEventSink;
  readonly spotMeshNames?: readonly string[];
  readonly routeCacheMaxAgeMs?: number;
  readonly monotonicNowMs?: () => number;
}

interface CachedReadyRoute<TRow> {
  readonly row: TRow;
  readonly expiresAtMs: number;
  readonly storeVersion?: string;
}

export interface ZLinkResolvedActorRoute {
  readonly meshName: string;
  readonly actorRef: ActorRef;
  readonly actorType: string;
  readonly ownerNodeGeneration: bigint;
  readonly ownerId: string;
  readonly ownerLeaseGeneration: bigint;
  readonly authorityOwnerGeneration: bigint;
  readonly authorityStoreVersion: string;
  readonly spotId?: string;
  readonly spotGeneration?: bigint;
  /** Exact enclosing User Spot authority used by remote Actor packet relay. */
  readonly enclosingSpotRoute?: ZLinkSpotRouteTarget;
}

export interface ZLinkActorRouteInvalidationFence {
  readonly actorId: string;
  readonly objectGeneration: bigint;
  readonly targetNodeRid: string;
  readonly targetNodeGeneration: bigint;
  readonly authorityOwnerGeneration: bigint;
  readonly ownerLeaseGeneration: bigint;
}

export class ZLinkStoreLocationResolvers implements
  ZLinkPeerLocationResolver,
  ZLinkSpotHandleResolver,
  ZLinkActorSpotHandleResolver {
  private readonly liveRows: ZLinkLiveRowFilter;
  private readonly monotonicNowMs: () => number;
  private readonly actorRoutes = new Map<string, CachedReadyRoute<ZLinkActorLocation>>();
  private readonly directActorRoutes = new Map<string, CachedReadyRoute<ZLinkResolvedActorRoute>>();
  private readonly spotRoutes = new Map<string, CachedReadyRoute<ZLinkSpotLocation>>();
  private readonly authoritySpotResolver: ZLinkAuthoritySpotRouteResolver;
  private nextActorPlacement = 0n;

  constructor(private readonly options: ZLinkStoreLocationResolversOptions) {
    this.liveRows = new ZLinkLiveRowFilter(options.leaseTracker);
    this.monotonicNowMs = options.monotonicNowMs ?? (() => performance.now());
    this.authoritySpotResolver = new ZLinkAuthoritySpotRouteResolver(
      options.stores.authorityStore,
      (meshName) => meshName,
      undefined,
      options.leaseTracker,
      options.routeCacheMaxAgeMs,
      this.monotonicNowMs
    );
  }

  async listLivePeers(filter: ZLinkPeerLocationFilter, signal?: AbortSignal): Promise<readonly ZLinkPeerLocation[]> {
    const rows = await this.options.stores.peerStore.listPeers(filter, signal);
    return await this.liveRows.filter(
      rows,
      (row) => row.ownerId,
      signal,
      (row) => isKnownZLinkLocationAutoConnectType(row.autoConnectType) && isKnownZLinkLocationRole(row.role)
    );
  }

  async resolveEntrySpotNode(
    spotId: SpotId,
    meshNames: readonly string[],
    signal?: AbortSignal
  ): Promise<{
    readonly meshName: string;
    readonly nodeRid: RoutingId;
    readonly spotId: SpotId;
    readonly targetNodeGeneration: bigint;
    readonly targetOwnerId: string;
    readonly ownerLeaseGeneration: bigint;
  } | undefined> {
    for (const meshName of meshNames) {
      const descriptors = await this.liveRows.filter(
        (await this.options.stores.locationStore.listMeshNodes(meshName, undefined, signal)).items,
        (descriptor) => descriptor.ownerId,
        signal
      );
      const descriptor = descriptors.find((candidate) =>
        candidate.entrySpotId === spotId
        && candidate.objectRole === ZLinkObjectRole.Server
        // An Entry Spot accepts new joins only while its owning node is Serving.
        // Preparing/retiring/error rows may still be visible during replacement,
        // but routing them would turn a valid transfer into NotConnected.
        && candidate.state === ZLinkFrameworkRuntimeState.Serving
      );
      if (descriptor !== undefined) {
        return {
          meshName,
          nodeRid: descriptor.rid,
          spotId,
          targetNodeGeneration: descriptor.lifecycleGeneration,
          targetOwnerId: descriptor.ownerId,
          ownerLeaseGeneration: descriptor.leaseGeneration
        };
      }
    }
    return undefined;
  }

  async selectActorPlacement(
    meshName: string,
    actorType: string,
    excludedNodeRid: RoutingId,
    signal?: AbortSignal,
    excludedCandidateRids: ReadonlySet<string> = new Set()
  ): Promise<RoutingId | undefined> {
    const descriptors = (await this.options.stores.locationStore.listMeshNodes(meshName, undefined, signal)).items;
    const liveDescriptors = await this.liveRows.filter(
      descriptors,
      (descriptor) => descriptor.ownerId,
      signal
    );
    const candidates = liveDescriptors.filter((descriptor) => {
      const capacity = descriptor.populationCapacity.actors;
      return descriptor.state === ZLinkFrameworkRuntimeState.Serving
        && descriptor.objectRole === ZLinkObjectRole.Server
        && descriptor.placementWeight > 0
        && !routingIdsEqual(descriptor.rid, excludedNodeRid)
        && !excludedCandidateRids.has(String(descriptor.rid))
        && capacity.active + capacity.reserved < capacity.limit
        && descriptor.objectCapabilities.some((candidate) =>
          candidate.objectKind === 'actor' && candidate.stableType === actorType
        );
    });
    if (candidates.length === 0) return undefined;
    const totalWeight = candidates.reduce(
      (total, candidate) => total + BigInt(candidate.placementWeight),
      0n
    );
    let ticket = this.nextActorPlacement % totalWeight;
    this.nextActorPlacement++;
    for (const candidate of candidates) {
      const weight = BigInt(candidate.placementWeight);
      if (ticket < weight) return candidate.rid;
      ticket -= weight;
    }
    return candidates[candidates.length - 1]?.rid;
  }

  async resolveRoute(key: ZLinkRouteLocationKey, signal?: AbortSignal): Promise<ZLinkRouteLocation | undefined> {
    const row = await this.liveRows.resolve(
      await this.options.stores.routeStore.resolveRoute(key, signal),
      (candidate) => candidate.ownerId,
      signal
    );
    if (row === undefined) {
      this.options.events?.routeResolveMiss(key);
      return undefined;
    }
    return row;
  }

  async resolveSpotRef(
    meshName: string,
    spotId: RoutingId,
    signal?: AbortSignal
  ): Promise<ResolvedSpotHandle | undefined> {
    const row = await this.resolveSpotRow({ meshName, spotId }, signal);
    if (row !== undefined) {
      if (row.spotGeneration <= 0n) {
        throw createInternalFrameworkException(
          ZLinkFrameworkInternalErrorKind.SpotRouteNotFound,
          `SPOT '${spotId}' location row has no valid Core lifecycle generation.`
        );
      }
      return {
        meshName: row.meshName,
        nodeRid: String(row.ownerNodeRid),
        spotId: String(row.spotId),
        spotKind: row.spotKind,
        spotGeneration: row.spotGeneration,
        targetNodeState: await this.resolveMeshNodeState(
          row.meshName,
          row.ownerNodeRid,
          row.ownerNodeGeneration,
          signal
        )
      };
    }
    const entrySpot = await resolveEntrySpotPeerInMeshes(this, spotId, [meshName], signal);
    if (entrySpot !== undefined) {
      return {
        meshName: entrySpot.meshName,
        nodeRid: String(entrySpot.nodeRid),
        spotId: entrySpot.spotId,
        spotKind: ZLinkSpotKind.Entry,
        targetNodeState: ZLinkFrameworkRuntimeState.Serving
      };
    }
    return undefined;
  }

  async resolveSpotHandle(
    meshName: string,
    spotId: RoutingId,
    signal?: AbortSignal
  ): Promise<SpotHandle | undefined> {
    const initial = await this.resolveSpotRef(meshName, spotId, signal);
    if (initial === undefined) return undefined;
    return createSpotHandle(
      String(spotId),
      initial,
      (refreshSignal) => this.resolveSpotRef(meshName, spotId, refreshSignal)
    );
  }

  async resolveActorSpotRef(
    meshName: string,
    actorId: string,
    signal?: AbortSignal
  ): Promise<ResolvedSpotHandle | undefined> {
    const row = await this.resolveActorRow({ meshName, actorId }, signal);
    if (row === undefined) {
      return undefined;
    }
    const spotId = row.spotKind === ZLinkSpotKind.Entry
      ? row.ownerNodeRid
      : row.spotId;
    return {
      meshName: row.meshName,
      nodeRid: String(row.ownerNodeRid),
      spotId: String(spotId),
      spotKind: row.spotKind,
      spotGeneration: row.spotGeneration
    };
  }

  async resolveActorRef(actorId: string, signal?: AbortSignal): Promise<ActorRef | undefined> {
    const direct = await this.resolveDirectActorRoute(actorId, signal);
    if (direct !== undefined) {
      return {
        ...direct.actorRef,
        ownershipGeneration: direct.authorityOwnerGeneration,
        ownerLeaseGeneration: direct.ownerLeaseGeneration,
        acceptedHighWater: 0n
      } as ActorRef;
    }
    // An authority row is the canonical Actor location. If it exists but its
    // owner lease is expired or its payload is invalid, the legacy location
    // projection must not resurrect a stale route while recovery is pending.
    const authority = await this.options.stores.authorityStore.readAuthority(
      encodeAuthorityKey('actor', actorId),
      signal
    );
    if (authority.kind === 'snapshot') {
      return undefined;
    }
    return (await this.resolveActorRow({ meshName: '', actorId }, signal))?.actorRef;
  }

  async resolveDirectActorRoute(
    actorId: string,
    signal?: AbortSignal
  ): Promise<ZLinkResolvedActorRoute | undefined> {
    const cached = this.getCached(this.directActorRoutes, actorId);
    if (cached !== undefined) return cached;
    const current = await this.options.stores.authorityStore.readAuthority(
      encodeAuthorityKey('actor', actorId),
      signal
    );
    if (current.kind !== 'snapshot' || current.allocation.state !== 'active') return undefined;
    const decoded = decodeActorAuthorityIdentity(current.payload);
    if (
      decoded === undefined
      || decoded.actor.actorId !== actorId
      || decoded.actor.objectGeneration !== current.objectGeneration
    ) return undefined;
    const currentOwner = {
      ownerId: current.ownerId,
      leaseGeneration: current.ownerLeaseGeneration
    };
    const remainingLeaseMs = await this.options.leaseTracker.remainingOwnerTokenLeaseMs(
      currentOwner,
      signal
    );
    if (remainingLeaseMs <= 0) return undefined;
    let enclosingSpotRoute: ZLinkSpotRouteTarget | undefined;
    if (decoded.spotId !== undefined) {
      try {
        enclosingSpotRoute = await this.authoritySpotResolver.resolve(decoded.spotId, signal);
      } catch (error) {
        if (
          error instanceof ZLinkFrameworkException
          && internalFrameworkErrorKind(error) === ZLinkFrameworkInternalErrorKind.SpotRouteNotFound
        ) {
          return undefined;
        }
        throw error;
      }
      if (
        decoded.spotGeneration === undefined
        || enclosingSpotRoute.targetSpotGeneration !== decoded.spotGeneration
        || !routingIdsEqual(enclosingSpotRoute.targetNodeRid, decoded.actor.nodeRid)
      ) {
        return undefined;
      }
    }
    const route: ZLinkResolvedActorRoute = {
      meshName: current.allocation.descriptor.meshName,
      actorRef: {
        ...decoded.actor,
        meshName: current.allocation.descriptor.meshName,
        nodeRid: current.allocation.descriptor.rid
      },
      actorType: decoded.actorType,
      ownerNodeGeneration: current.allocation.descriptorLifecycleGeneration,
      ownerId: currentOwner.ownerId,
      ownerLeaseGeneration: currentOwner.leaseGeneration,
      authorityOwnerGeneration: current.authorityOwnerGeneration,
      authorityStoreVersion: current.storeVersion.value,
      spotId: decoded.spotId,
      spotGeneration: decoded.spotGeneration,
      enclosingSpotRoute
    };
    const maxAgeMs = this.options.routeCacheMaxAgeMs ?? 15000;
    if (maxAgeMs > 0) {
      this.directActorRoutes.set(actorId, {
        row: route,
        expiresAtMs: this.monotonicNowMs() + Math.min(maxAgeMs, remainingLeaseMs),
        storeVersion: current.storeVersion.value
      });
    }
    return route;
  }

  observeActorAuthorityVersion(actorId: string, storeVersion: string): void {
    const cached = this.directActorRoutes.get(actorId);
    if (cached !== undefined && cached.storeVersion !== storeVersion) {
      this.directActorRoutes.delete(actorId);
    }
  }

  async resolveActorSpotHandle(
    meshName: string,
    actorId: string,
    signal?: AbortSignal
  ): Promise<SpotHandle | undefined> {
    const initial = await this.resolveActorSpotRef(meshName, actorId, signal);
    if (initial === undefined) return undefined;
    return createSpotHandle(
      initial.spotId,
      initial,
      (refreshSignal) => this.resolveActorSpotRef(meshName, actorId, refreshSignal)
    );
  }

  async resolveSpotRow(
    key: ZLinkSpotLocationKey,
    signal?: AbortSignal
  ): Promise<ZLinkSpotLocation | undefined> {
    const cacheKey = `${key.meshName}\u0000${String(key.spotId)}`;
    const cached = this.getCached(this.spotRoutes, cacheKey);
    if (cached !== undefined) return cached;
    const row = await this.liveRows.resolve(
      await this.options.stores.spotStore.resolveSpot(key, signal),
      (candidate) => candidate.ownerId,
      signal
    );
    if (row === undefined) {
      this.options.events?.spotResolveMiss(key);
      return undefined;
    }
    if (!await this.cacheReady(
      this.spotRoutes,
      cacheKey,
      row,
      row.ownerId,
      row.leaseGeneration,
      signal
    )) {
      this.options.events?.spotResolveMiss(key);
      return undefined;
    }
    return row;
  }

  async resolveSpotRowInMeshes(
    spotId: RoutingId,
    meshNames: readonly string[],
    signal?: AbortSignal
  ): Promise<ZLinkSpotLocation | undefined> {
    for (const meshName of meshNames) {
      const row = await this.resolveSpotRow({ meshName, spotId }, signal);
      if (row !== undefined) {
        return row;
      }
    }
    return undefined;
  }

  async resolveMeshNodeState(
    meshName: string,
    nodeRid: RoutingId,
    expectedLifecycleGeneration?: bigint,
    signal?: AbortSignal
  ): Promise<ZLinkFrameworkRuntimeState | undefined> {
    const descriptor = (await this.options.stores.locationStore.listMeshNodes(
      meshName,
      undefined,
      signal
    )).items.find((candidate) =>
      routingIdsEqual(candidate.rid, nodeRid)
      && (
        expectedLifecycleGeneration === undefined
        || candidate.lifecycleGeneration === expectedLifecycleGeneration
      )
    );
    return descriptor?.state;
  }

  async resolveActorRow(
    key: ZLinkActorLocationKey,
    signal?: AbortSignal
  ): Promise<ZLinkActorLocation | undefined> {
    const cacheKey = `${key.meshName}\u0000${key.actorId}`;
    const cached = this.getCached(this.actorRoutes, cacheKey);
    if (cached !== undefined) return cached;
    const meshNames = key.meshName.length === 0
      ? this.options.spotMeshNames ?? []
      : [key.meshName];
    let stored: ZLinkActorLocation | undefined;
    for (const meshName of meshNames) {
      stored = await this.options.stores.actorStore.resolveActor({
        meshName,
        actorId: key.actorId
      }, signal);
      if (stored !== undefined) break;
    }
    const row = await this.liveRows.resolve(
      stored,
      (candidate) => candidate.ownerId,
      signal,
      (candidate) =>
        candidate.actorRef.objectGeneration > 0n
        && candidate.ownerNodeGeneration > 0n
        && candidate.membershipEpoch > 0n
    );
    if (row === undefined) {
      this.options.events?.actorResolveMiss(key);
      return undefined;
    }
    if (row.spotKind === ZLinkSpotKind.User) {
      const currentSpot = await this.resolveSpotRow({
        meshName: row.meshName,
        spotId: row.spotId
      }, signal);
      if (
        currentSpot === undefined
        || row.spotGeneration <= 0n
        || row.membershipEpoch <= 0n
        || row.spotGeneration !== currentSpot.spotGeneration
        || row.ownerNodeGeneration !== currentSpot.ownerNodeGeneration
        || !routingIdsEqual(row.ownerNodeRid, currentSpot.ownerNodeRid)
      ) {
        this.options.events?.actorResolveMiss(key);
        return undefined;
      }
    }
    if (!await this.cacheReady(
      this.actorRoutes,
      cacheKey,
      row,
      row.ownerId,
      row.leaseGeneration,
      signal
    )) {
      this.options.events?.actorResolveMiss(key);
      return undefined;
    }
    return row;
  }

  invalidateActorRoute(actorId: string, meshName = ''): void {
    this.directActorRoutes.delete(actorId);
    if (meshName.length > 0) {
      this.actorRoutes.delete(`${meshName}\u0000${actorId}`);
      return;
    }
    for (const key of this.actorRoutes.keys()) {
      if (key.endsWith(`\u0000${actorId}`)) this.actorRoutes.delete(key);
    }
  }

  invalidateActorRouteIfMatches(fence: ZLinkActorRouteInvalidationFence): boolean {
    let invalidated = false;
    const direct = this.directActorRoutes.get(fence.actorId);
    if (direct !== undefined && directActorRouteMatchesFence(direct.row, fence)) {
      this.directActorRoutes.delete(fence.actorId);
      invalidated = true;
    }
    return invalidated;
  }

  invalidateSpotRoute(spotId: SpotId, meshName?: string): void {
    if (meshName !== undefined) {
      this.spotRoutes.delete(`${meshName}\u0000${spotId}`);
    } else {
      for (const key of this.spotRoutes.keys()) {
        if (key.endsWith(`\u0000${spotId}`)) this.spotRoutes.delete(key);
      }
    }
    for (const [key, cached] of this.actorRoutes) {
      if (
        cached.row.spotId === spotId
        && (meshName === undefined || cached.row.meshName === meshName)
      ) this.actorRoutes.delete(key);
    }
  }

  private getCached<TRow>(
    cache: Map<string, CachedReadyRoute<TRow>>,
    key: string
  ): TRow | undefined {
    const entry = cache.get(key);
    if (entry === undefined) return undefined;
    if (this.monotonicNowMs() >= entry.expiresAtMs) {
      cache.delete(key);
      return undefined;
    }
    return entry.row;
  }

  private async cacheReady<TRow>(
    cache: Map<string, CachedReadyRoute<TRow>>,
    key: string,
    row: TRow,
    ownerId: string,
    ownerLeaseGeneration: bigint,
    signal?: AbortSignal
  ): Promise<boolean> {
    const maxAgeMs = this.options.routeCacheMaxAgeMs ?? 15000;
    const remainingLeaseMs = await this.options.leaseTracker.remainingOwnerTokenLeaseMs(
      { ownerId, leaseGeneration: ownerLeaseGeneration },
      signal
    );
    if (remainingLeaseMs <= 0) return false;
    if (maxAgeMs <= 0) return true;
    const lifetimeMs = Math.min(maxAgeMs, remainingLeaseMs);
    if (lifetimeMs <= 0) return false;
    cache.set(key, { row, expiresAtMs: this.monotonicNowMs() + lifetimeMs });
    return true;
  }
}

function directActorRouteMatchesFence(
  route: ZLinkResolvedActorRoute,
  fence: ZLinkActorRouteInvalidationFence
): boolean {
  return route.actorRef.actorId === fence.actorId
    && route.actorRef.objectGeneration === fence.objectGeneration
    && routingIdsEqual(route.actorRef.nodeRid, fence.targetNodeRid)
    && route.ownerNodeGeneration === fence.targetNodeGeneration
    && route.authorityOwnerGeneration === fence.authorityOwnerGeneration
    && route.ownerLeaseGeneration === fence.ownerLeaseGeneration;
}

export class DefaultZLinkLocationReadiness implements ZLinkLocationReadiness {
  constructor(private readonly query: ZLinkLocationRuntimeQuery) {}

  async isPeerReady(
    meshName: string,
    role: ZLinkLocationRole,
    nodeRid?: RoutingId,
    signal?: AbortSignal
  ): Promise<boolean> {
    try {
      void role;
      const page = await this.query.listTopology({
        meshName,
        nodeRid,
        state: ZLinkLocationTopologyState.Ready
      }, undefined, signal);
      return page.items.length > 0;
    } catch {
      return false;
    }
  }
}

export class ZLinkLocationSpotRouteResolver implements ZLinkSpotRouteResolver {
  constructor(
    private readonly rows: ZLinkStoreLocationResolvers,
    private readonly meshNames: readonly string[],
    private readonly routerChannelIdForMesh: (meshName: string) => string = (meshName) => meshName,
    private readonly resolveLocalSpot?: (spotId: RoutingId) => ZLinkSpotRouteTarget | undefined
  ) {}

  async resolve(spotId: RoutingId, signal?: AbortSignal): Promise<ZLinkSpotRouteTarget> {
    const row = await this.rows.resolveSpotRowInMeshes(spotId, this.meshNames, signal);
    if (row !== undefined) {
      if (row.spotGeneration <= 0n) {
        throw createInternalFrameworkException(
          ZLinkFrameworkInternalErrorKind.SpotRouteNotFound,
          `SPOT '${spotId}' location row has no valid Core lifecycle generation.`
        );
      }
      return {
        routerChannelId: this.routerChannelIdForMesh(row.meshName),
        targetNodeRid: row.ownerNodeRid,
        spotId: row.spotId,
        spotKind: row.spotKind,
        targetSpotGeneration: row.spotGeneration,
        ownerLeaseGeneration: row.leaseGeneration,
        targetNodeState: await this.rows.resolveMeshNodeState(
          row.meshName,
          row.ownerNodeRid,
          row.ownerNodeGeneration,
          signal
        )
      };
    }
    const local = this.resolveLocalSpot?.(spotId);
    if (local !== undefined) {
      return local;
    }
    const entrySpot = await resolveEntrySpotPeerInMeshes(this.rows, spotId, this.meshNames, signal);
    if (entrySpot !== undefined) {
      return {
        routerChannelId: this.routerChannelIdForMesh(entrySpot.meshName),
        targetNodeRid: entrySpot.nodeRid,
        spotId: entrySpot.spotId,
        spotKind: ZLinkSpotKind.Entry,
        targetNodeGeneration: entrySpot.targetNodeGeneration,
        targetOwnerId: entrySpot.targetOwnerId,
        ownerLeaseGeneration: entrySpot.ownerLeaseGeneration,
        targetNodeState: ZLinkFrameworkRuntimeState.Serving
      };
    }
    throw createInternalFrameworkException(
      ZLinkFrameworkInternalErrorKind.SpotRouteNotFound,
      `SPOT '${spotId}' has no live location row in any registered spot mesh.`
    );
  }

  invalidate(spotId: RoutingId): void {
    this.rows.invalidateSpotRoute(String(spotId));
  }
}

export class ZLinkAuthoritySpotRouteResolver implements ZLinkSpotRouteResolver {
  private readonly cache = new Map<string, CachedReadyRoute<ZLinkSpotRouteTarget>>();
  private readonly routeEpochs = new Map<string, number>();
  private readonly activeResolutions = new Map<string, number>();

  constructor(
    private readonly store: ZLinkAuthorityStore,
    private readonly routerChannelIdForMesh: (meshName: string) => string,
    private readonly fallback?: ZLinkSpotRouteResolver,
    private readonly leaseTracker?: ZLinkOwnerLeaseTracker,
    private readonly routeCacheMaxAgeMs = 15000,
    private readonly monotonicNowMs: () => number = () => performance.now(),
    private readonly targetNodeStateResolver?: (
      meshName: string,
      nodeRid: RoutingId,
      expectedLifecycleGeneration: bigint,
      signal?: AbortSignal
    ) => Promise<ZLinkFrameworkRuntimeState | undefined>
  ) {}

  async resolve(spotId: RoutingId, signal?: AbortSignal): Promise<ZLinkSpotRouteTarget> {
    const key = String(spotId);
    const epoch = this.beginResolution(key);
    try {
      const cached = this.cache.get(key);
      if (cached !== undefined) {
        if (this.monotonicNowMs() < cached.expiresAtMs) {
          return cached.row;
        }
        this.cache.delete(key);
      }

      const current = await this.store.readAuthority(
        encodeAuthorityKey('user_spot', key),
        signal
      );
      if (current.kind === 'snapshot' && current.allocation.state === 'active') {
        const decoded = decodeServiceReadySpotAuthority(current.payload);
        if (
          decoded !== undefined
          && decoded.spotId === String(spotId)
          && decoded.ownerId === current.ownerId
          && decoded.ownerLeaseGeneration === current.ownerLeaseGeneration
        ) {
          const target: ZLinkSpotRouteTarget = {
            routerChannelId: this.routerChannelIdForMesh(decoded.ownerMeshName),
            targetNodeRid: decoded.ownerNodeRid,
            spotId,
            spotKind: decoded.kind === 'instance_spot'
              ? ZLinkSpotKind.Instance
              : ZLinkSpotKind.User,
            stableType: decoded.stableType,
            targetSpotGeneration: current.objectGeneration,
            targetNodeGeneration: current.allocation.descriptorLifecycleGeneration,
            authorityOwnerGeneration: current.authorityOwnerGeneration,
            targetOwnerId: current.ownerId,
            ownerLeaseGeneration: current.ownerLeaseGeneration,
            authorityStoreVersion: current.storeVersion.value,
            targetNodeState: await this.targetNodeStateResolver?.(
              decoded.ownerMeshName,
              decoded.ownerNodeRid,
              current.allocation.descriptorLifecycleGeneration,
              signal
            )
          };
          if (this.routeCacheMaxAgeMs > 0 && this.currentEpoch(key) === epoch) {
            const remainingLeaseMs = this.leaseTracker === undefined
              ? 0
              : await this.leaseTracker.remainingOwnerTokenLeaseMs({
                  ownerId: current.ownerId,
                  leaseGeneration: current.ownerLeaseGeneration
                }, signal);
            const lifetimeMs = Math.min(this.routeCacheMaxAgeMs, remainingLeaseMs);
            if (lifetimeMs > 0 && this.currentEpoch(key) === epoch) {
              this.cache.set(key, {
                row: target,
                expiresAtMs: this.monotonicNowMs() + lifetimeMs,
                storeVersion: current.storeVersion.value
              });
            }
          }
          return target;
        }
      }

      if (current.kind === 'snapshot') {
        throw createInternalFrameworkException(
          ZLinkFrameworkInternalErrorKind.SpotRouteNotFound,
          `SPOT '${spotId}' authority has not crossed the Ready barrier.`
        );
      }
      if (this.fallback !== undefined) {
        return await this.fallback.resolve(spotId, signal);
      }
      throw createInternalFrameworkException(
        ZLinkFrameworkInternalErrorKind.SpotRouteNotFound,
        `SPOT '${spotId}' has no Ready authority.`
      );
    } finally {
      this.finishResolution(key);
    }
  }

  invalidate(spotId: RoutingId): void {
    const key = String(spotId);
    this.cache.delete(key);
    if (this.activeResolutions.has(key)) {
      this.routeEpochs.set(key, this.currentEpoch(key) + 1);
    } else {
      this.routeEpochs.delete(key);
    }
    this.fallback?.invalidate?.(spotId);
  }

  private beginResolution(key: string): number {
    const epoch = this.currentEpoch(key);
    this.routeEpochs.set(key, epoch);
    this.activeResolutions.set(key, (this.activeResolutions.get(key) ?? 0) + 1);
    return epoch;
  }

  private finishResolution(key: string): void {
    const active = this.activeResolutions.get(key);
    if (active === undefined || active <= 1) {
      this.activeResolutions.delete(key);
      if (!this.cache.has(key)) this.routeEpochs.delete(key);
      return;
    }
    this.activeResolutions.set(key, active - 1);
  }

  private currentEpoch(key: string): number {
    return this.routeEpochs.get(key) ?? 0;
  }
}

async function resolveEntrySpotPeerInMeshes(
  rows: Pick<ZLinkStoreLocationResolvers, 'resolveEntrySpotNode'>,
  spotId: SpotId,
  meshNames: readonly string[],
  signal?: AbortSignal
): Promise<Awaited<ReturnType<ZLinkStoreLocationResolvers['resolveEntrySpotNode']>>> {
  return await rows.resolveEntrySpotNode(spotId, meshNames, signal);
}
