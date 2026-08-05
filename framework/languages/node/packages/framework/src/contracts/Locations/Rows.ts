import type { ActorRef, RoutingId, SpotId } from '../Common';
import type { ZLinkSpotKind } from '../Spots';
import type { ZLinkLocationAutoConnectType, ZLinkLocationRole, ZLinkRouteKind } from './Values';

export enum ZLinkObjectRole {
  None = 'none',
  Client = 'client',
  Server = 'server'
}

export enum ZLinkFrameworkRuntimeState {
  Preparing = 0,
  Serving = 1,
  Relocating = 2,
  Relocated = 3,
  Draining = 4,
  Stopped = 5,
  Error = 6
}
export type ZLinkObjectMaintenancePolicyKind = 'disabled' | 'recreate' | 'snapshot';

export interface ZLinkObjectCapability {
  readonly objectKind: 'actor' | 'user_spot' | 'instance_spot';
  readonly stableType: string;
  readonly policy: ZLinkObjectMaintenancePolicyKind;
  readonly hasSnapshotAdapter: boolean;
  readonly limit: number;
}

export interface ZLinkPopulationCapacity {
  readonly active: number;
  readonly reserved: number;
  readonly limit: number;
}

export interface ZLinkSpotTypeCapacity extends ZLinkPopulationCapacity {
  readonly objectKind: 'user_spot' | 'instance_spot';
  readonly stableType: string;
}

export interface ZLinkPeerLocation {
  readonly autoConnectType: ZLinkLocationAutoConnectType;
  readonly meshName: string;
  readonly nodeRid?: RoutingId;
  readonly role: ZLinkLocationRole;
  readonly endpoint: string;
  readonly weight: number;
  readonly draining: boolean;
  readonly value: bigint;
  readonly metadata?: Readonly<Record<string, string>>;
  readonly capabilities?: readonly string[];
  readonly ownerId: string;
  readonly generation: bigint;
  readonly updatedAt: Date;
}

/** RouteMesh discovery descriptor fixed by the 10.0 location-store contract. */
export interface ZLinkMeshNodeDescriptor {
  readonly meshName: string;
  readonly rid: RoutingId;
  readonly lifecycleGeneration: bigint;
  readonly descriptorRevision: bigint;
  readonly endpoint: string;
  readonly objectRole: ZLinkObjectRole;
  readonly entrySpotId?: string;
  readonly placementWeight: number;
  readonly populationCapacity: {
    readonly actors: ZLinkPopulationCapacity;
    readonly spots: ZLinkPopulationCapacity;
    readonly spotTypes: readonly ZLinkSpotTypeCapacity[];
  };
  readonly activationConcurrency: {
    readonly active: number;
    readonly limit: number;
  };
  readonly channelWeights: Readonly<Record<string, number>>;
  readonly applicationVersion: bigint;
  readonly spotTypes: readonly string[];
  readonly objectCapabilities: readonly ZLinkObjectCapability[];
  readonly maintenanceWave?: string;
  readonly state: ZLinkFrameworkRuntimeState;
  readonly securityIdentity: string;
  readonly ownerId: string;
  readonly leaseGeneration: bigint;
  readonly updatedAt: Date;
}

export interface ZLinkClientServerServerDescriptor {
  readonly channelName: string;
  readonly serverRid: RoutingId;
  readonly lifecycleGeneration: bigint;
  readonly descriptorRevision: bigint;
  readonly endpoint: string;
  readonly weight: number;
  readonly state: ZLinkFrameworkRuntimeState;
  readonly securityIdentity: string;
  readonly ownerId: string;
  readonly leaseGeneration: bigint;
  readonly updatedAt: Date;
}

export interface ZLinkFanoutPublisherDescriptor {
  readonly channelName: string;
  readonly publisherRid: RoutingId;
  readonly lifecycleGeneration: bigint;
  readonly descriptorRevision: bigint;
  readonly endpoint: string;
  readonly state: ZLinkFrameworkRuntimeState;
  readonly securityIdentity: string;
  readonly ownerId: string;
  readonly leaseGeneration: bigint;
  readonly updatedAt: Date;
}

export interface ZLinkSpotLocation {
  readonly meshName: string;
  readonly spotId: SpotId;
  /** Core lifecycle generation of the addressed Spot. */
  readonly spotGeneration: bigint;
  readonly spotType: string;
  /** MeshNode that currently owns this Spot. */
  readonly ownerNodeRid: RoutingId;
  /** Core lifecycle generation of the owning MeshNode. */
  readonly ownerNodeGeneration: bigint;
  readonly spotKind: ZLinkSpotKind;
  readonly ownerId: string;
  /** Generation of the exact owner lease that published this row. */
  readonly leaseGeneration: bigint;
  readonly updatedAt: Date;
}

export interface ZLinkActorLocation {
  /** RouteMesh that owns this Actor location. */
  readonly meshName: string;
  readonly actorId: string;
  readonly actorType: string;
  readonly actorRef: ActorRef;
  /** MeshNode that currently owns this Actor. */
  readonly ownerNodeRid: RoutingId;
  readonly ownerNodeGeneration: bigint;
  /** Kind of Spot that currently owns this Actor. */
  readonly spotKind: ZLinkSpotKind;
  readonly spotId: SpotId;
  readonly spotGeneration: bigint;
  readonly membershipEpoch: bigint;
  readonly ownerId: string;
  /** Generation of the exact owner lease that published this row. */
  readonly leaseGeneration: bigint;
  readonly updatedAt: Date;
}

export interface ZLinkRouteLocation {
  readonly routeKind: ZLinkRouteKind;
  readonly routeKey: string;
  readonly ownerNodeRid: RoutingId;
  readonly ownerId: string;
  readonly generation: bigint;
  /**
   * Framework internal route payload. This is not an application key-value
   * storage surface.
   */
  readonly value: Uint8Array;
  readonly updatedAt: Date;
}
