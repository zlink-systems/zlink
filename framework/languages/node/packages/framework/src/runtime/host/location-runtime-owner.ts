import type { RoutingId } from '../../contracts';
import type { ZLinkRuntimeEventPublisher } from '../diagnostics';
import type { ZLinkActorTransferStore } from '../../contracts/Locations/ActorTransfer';
import type { ZLinkFrameworkRegistration } from '../configuration';
import type { ZLinkChannelRuntimeManager } from '../channels';
import {
  ZLinkInMemoryProviderLocationStore,
  ZLinkLocationLifecycle,
  ZLinkLocationRuntime,
  ZLinkLocationStoreRepository,
  ZLinkLocationSpotRouteResolver,
  ZLinkOwnerLeaseTracker,
  ZLinkStoreLocationResolvers,
  type ZLinkLocationEventSink,
  type ZLinkLocationRuntimeStores,
  type ZLinkDomainLocationStore
} from '../locations';
import type { ZLinkSpotNodeRuntimeManager } from '../spots';
import type { ZLinkSpotRouteResolver } from '../spots/spot-routing-internal';

export interface ZLinkLocationRuntimeOwnerOptions {
  readonly registration: ZLinkFrameworkRegistration;
  readonly runtimeEventPublisher: ZLinkRuntimeEventPublisher;
  readonly fallbackNodeRid: RoutingId;
  readonly metrics: import('../diagnostics').ZLinkRuntimeMetrics;
  readonly rewriteAuthorityPayloadForOwner?: import('../locations').ZLinkLocationRuntimeOptions['rewriteAuthorityPayloadForOwner'];
}

export interface ZLinkLocationRuntimeStopSnapshot {
  readonly runtime?: ZLinkLocationRuntime;
  readonly lifecycle?: ZLinkLocationLifecycle;
}

export class ZLinkLocationRuntimeOwner {
  private stores?: ZLinkLocationRuntimeStores;
  private runtime?: ZLinkLocationRuntime;
  private lifecycle?: ZLinkLocationLifecycle;
  private events?: ZLinkLocationEventSink;
  private leaseTrackerState?: {
    readonly stores: ZLinkLocationRuntimeStores;
    readonly tracker: ZLinkOwnerLeaseTracker;
  };
  private spotRouteInvalidator?: (spotId: RoutingId) => void;

  constructor(private readonly options: ZLinkLocationRuntimeOwnerOptions) {}

  get currentStores(): ZLinkLocationRuntimeStores | undefined {
    return this.stores;
  }

  get currentRuntime(): ZLinkLocationRuntime | undefined {
    return this.runtime;
  }

  get currentLifecycle(): ZLinkLocationLifecycle | undefined {
    return this.lifecycle;
  }

  get currentEvents(): ZLinkLocationEventSink | undefined {
    return this.events;
  }

  get currentLeaseTracker(): ZLinkOwnerLeaseTracker | undefined {
    return this.leaseTrackerState?.tracker;
  }

  setSpotRouteInvalidator(invalidator?: (spotId: RoutingId) => void): void {
    this.spotRouteInvalidator = invalidator;
  }

  actorTransferStore(): ZLinkActorTransferStore | undefined {
    const store = this.stores?.locationStore ?? this.createAndRememberStores()?.locationStore;
    return isActorTransferStore(store) ? store : undefined;
  }

  locationStore(): ZLinkDomainLocationStore | undefined {
    return this.stores?.locationStore ?? this.createAndRememberStores()?.locationStore;
  }

  ensureRuntime(primaryMeshName: string | undefined): ZLinkLocationRuntime | undefined {
    if (this.runtime !== undefined) {
      return this.runtime;
    }
    const stores = this.stores ?? this.createRuntimeStores();
    if (stores === undefined) {
      return undefined;
    }
    this.stores = stores;
    const runtime = new ZLinkLocationRuntime({
      stores,
      options: this.options.registration.locations.options,
      events: this.ensureEvents(),
      leaseTracker: this.leaseTracker(stores),
      metrics: this.options.metrics,
      meshNames: [...this.options.registration.spotNodes.keys()],
      leaseScopes: [
        ...[...this.options.registration.spotNodes.keys()]
          .map(name => ({ kind: 'mesh' as const, name })),
        ...[...this.options.registration.channels.keys()]
          .map(name => ({ kind: 'channel' as const, name }))
      ],
      rewriteAuthorityPayloadForOwner: this.options.rewriteAuthorityPayloadForOwner
    });
    this.runtime = runtime;
    this.lifecycle = new ZLinkLocationLifecycle(
      runtime,
      stores.actorStore,
      primaryMeshName ?? '',
      stores.authorityStore,
      stores.spotStore,
      spotId => this.spotRouteInvalidator?.(spotId)
    );
    return runtime;
  }

  createRefResolver(spotMeshNames: readonly string[]): ZLinkStoreLocationResolvers | undefined {
    this.ensureRuntime(spotMeshNames.length === 1 ? spotMeshNames[0] : undefined);
    const stores = this.stores;
    if (stores === undefined) {
      return undefined;
    }
    return new ZLinkStoreLocationResolvers({
      stores: { ...stores, authorityStore: stores.locationStore },
      leaseTracker: this.leaseTracker(stores),
      events: this.events,
      spotMeshNames,
      routeCacheMaxAgeMs: this.options.registration.locations.options.routeCacheMaxAgeMs
    });
  }

  createSpotRouteResolver(
    spotMeshNames: readonly string[],
    spotRouterChannelIdByMesh: (meshName: string) => string,
    resolveLocalSpot?: (spotId: RoutingId) => import('../spots/spot-routing-internal').ZLinkSpotRouteTarget | undefined
  ): ZLinkSpotRouteResolver | undefined {
    const stores = this.stores;
    if (stores === undefined || spotMeshNames.length === 0) {
      return undefined;
    }
    return new ZLinkLocationSpotRouteResolver(
      new ZLinkStoreLocationResolvers({
        stores: { ...stores, authorityStore: stores.locationStore },
        leaseTracker: this.leaseTracker(stores),
        events: this.events,
        routeCacheMaxAgeMs: this.options.registration.locations.options.routeCacheMaxAgeMs
      }),
      spotMeshNames,
      spotRouterChannelIdByMesh,
      resolveLocalSpot
    );
  }

  createActorLocationResolver(spotMeshNames: readonly string[]): ZLinkStoreLocationResolvers | undefined {
    const stores = this.stores;
    if (stores === undefined) {
      return undefined;
    }
    return new ZLinkStoreLocationResolvers({
      stores: { ...stores, authorityStore: stores.locationStore },
      leaseTracker: this.leaseTracker(stores),
      events: this.events,
      spotMeshNames,
      routeCacheMaxAgeMs: this.options.registration.locations.options.routeCacheMaxAgeMs
    });
  }

  ownerNodeRid(spotNodeRuntime?: ZLinkSpotNodeRuntimeManager): RoutingId {
    const primaryMeshNode = spotNodeRuntime?.primaryMeshNode;
    const spotNodeRid = primaryMeshNode === undefined
      ? undefined
      : String(primaryMeshNode.status().routingId);
    if (spotNodeRid !== undefined) {
      return spotNodeRid;
    }
    for (const channel of this.options.registration.channels.values()) {
      const routingId = channel.routingId ?? channel.server?.routingId;
      if (routingId !== undefined) {
        return routingId;
      }
    }
    for (const routeChannel of this.options.registration.routeChannelOptions.values()) {
      if (routeChannel.routingId !== undefined) {
        return routeChannel.routingId;
      }
    }
    for (const spotNode of this.options.registration.spotNodes.values()) {
      const routingId = spotNode.routingId ?? spotNode.router?.routingId ?? spotNode.pubSub?.routingId;
      if (routingId !== undefined) {
        return routingId;
      }
    }
    return this.options.fallbackNodeRid;
  }

  async startForRuntime(
    primaryMeshName: string | undefined,
    spotNodeRuntime: ZLinkSpotNodeRuntimeManager,
    channelRuntime: ZLinkChannelRuntimeManager
  ): Promise<ZLinkLocationRuntime | undefined> {
    const runtime = this.ensureRuntime(primaryMeshName);
    if (runtime === undefined) {
      return undefined;
    }
    await runtime.start(this.ownerNodeRid(spotNodeRuntime));
    const stores = this.currentStores;
    if (stores === undefined) {
      return runtime;
    }
    spotNodeRuntime.configureLocationAutoConnect(
      runtime,
      stores,
      this.options.registration.locations.options,
      this.currentEvents
    );
    await spotNodeRuntime.startLocationAutoConnect();
    channelRuntime.configureLocationAutoConnect(
      runtime,
      stores,
      this.options.registration.locations.options,
      this.currentEvents
    );
    await channelRuntime.startLocationAutoConnect();
    return runtime;
  }

  clearForStop(): ZLinkLocationRuntimeStopSnapshot {
    const snapshot = {
      runtime: this.runtime,
      lifecycle: this.lifecycle
    };
    this.stores = undefined;
    this.runtime = undefined;
    this.lifecycle = undefined;
    this.events = undefined;
    this.leaseTrackerState = undefined;
    return snapshot;
  }

  private createRuntimeStores(): ZLinkLocationRuntimeStores | undefined {
    const locations = this.options.registration.locations;
    if (locations.useInMemoryStores) {
      const provider = new ZLinkInMemoryProviderLocationStore();
      const store = new ZLinkLocationStoreRepository(provider);
      return {
        locationStore: store,
        authorityStore: store,
        clientServerStore: store,
        fanoutStore: store,
        peerStore: store,
        spotStore: store,
        actorStore: store,
        routeStore: store,
        ownerLeaseStore: store
      };
    }
    const provider = locations.storeInstance;
    if (provider !== undefined) {
      const store = new ZLinkLocationStoreRepository(provider);
      return {
        locationStore: store,
        authorityStore: store,
        clientServerStore: store,
        fanoutStore: store,
        peerStore: store,
        spotStore: store,
        actorStore: store,
        routeStore: store,
        ownerLeaseStore: store
      };
    }
    return undefined;
  }

  private createAndRememberStores(): ZLinkLocationRuntimeStores | undefined {
    if (this.stores !== undefined) return this.stores;
    this.stores = this.createRuntimeStores();
    return this.stores;
  }

  private leaseTracker(stores: ZLinkLocationRuntimeStores): ZLinkOwnerLeaseTracker {
    if (this.leaseTrackerState?.stores === stores) {
      return this.leaseTrackerState.tracker;
    }
    const tracker = new ZLinkOwnerLeaseTracker({
      store: stores.ownerLeaseStore,
      options: this.options.registration.locations.options
    });
    this.leaseTrackerState = { stores, tracker };
    return tracker;
  }

  private ensureEvents(): ZLinkLocationEventSink | undefined {
    return this.events;
  }
}

function isActorTransferStore(value: unknown): value is ZLinkActorTransferStore {
  const store = value as Partial<Record<
    | 'prepareActorTransfer'
    | 'commitActorTransfer'
    | 'activateActorTransfer'
    | 'abortActorTransfer'
    | 'takeOverActorTransfer'
    | 'resolveActorTransfer',
    unknown
  >>;
  return typeof store.prepareActorTransfer === 'function'
    && typeof store.commitActorTransfer === 'function'
    && typeof store.activateActorTransfer === 'function'
    && typeof store.abortActorTransfer === 'function'
    && typeof store.takeOverActorTransfer === 'function'
    && typeof store.resolveActorTransfer === 'function';
}
