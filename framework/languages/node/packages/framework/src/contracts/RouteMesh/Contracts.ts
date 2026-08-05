import type { ActorRef, RoutingId } from '../Common';

export interface ZLinkActorMembership {
  readonly actor: ActorRef;
  readonly actorType: string;
  readonly membershipEpoch: bigint;
}

export interface ZLinkLocationOptionValues {
  readonly ownerLeaseRenewIntervalMs: number;
  readonly ownerLeaseTtlMs: number;
  readonly pollingIntervalMs: number;
  readonly storeFailureGraceMs: number;
  readonly ownerLeaseFencingMarginMs: number;
  readonly ownerLeaseRenewTimeoutMs: number;
  readonly routeCacheMaxAgeMs: number;
  readonly messageFollowDurationMs: number;
  readonly maxActiveOutboundRelocations: number;
  readonly maxActiveInboundRelocations: number;
  readonly maxConcurrentRelocationCaptures: number;
  readonly maxConcurrentRelocationRestores: number;
  readonly maxRelocationPayloadInFlightBytes: number;
}

export type ZLinkMessageSurface =
  | 'node'
  | 'channel'
  | 'spot'
  | 'instance_spot'
  | 'logical_multicast'
  | 'actor'
  | 'stream'
  | 'classic_fanout'
  | 'actor_transfer';

export type ZLinkMessageKind =
  | 'send'
  | 'request'
  | 'response'
  | 'error'
  | 'publish'
  | 'control';

export type ZLinkRequestFailureReason = 'timeout' | 'cancelled' | 'shutdown';

export class ZLinkRequestFailureError extends Error {
  readonly reason: ZLinkRequestFailureReason;

  constructor(reason: ZLinkRequestFailureReason, message: string, cause?: unknown) {
    super(message, { cause });
    this.name = 'ZLinkRequestFailureError';
    this.reason = reason;
  }
}

export interface ZLinkRuntimeErrorEvent {
  readonly eventId: 'zlink.runtime_error';
  readonly timestamp: Date;
  readonly kind: 'observer_failed';
  readonly source: 'message_flow_observer';
  readonly reason: string;
}

export interface ZLinkRuntimeErrorSink {
  onRuntimeError(error: ZLinkRuntimeErrorEvent): Promise<void> | void;
}

export interface ZLinkRouteMeshRuntimeOptions {
  mesh(meshName: string): ZLinkMeshPlacementRuntimeOptions;
  channel(channelName: string): ZLinkMeshChannelRuntimeOptions;
}

export interface ZLinkMeshPlacementRuntimeOptions {
  placementWeight: number;
}

export interface ZLinkMeshChannelRuntimeOptions {
  weight: number;
}

export enum ZLinkPeerState {
  Connecting = 0,
  Ready = 1,
  Draining = 2,
  NotConnected = 3,
  NotRequired = 4
}

export enum ZLinkTopologyReason {
  RuntimeNotReady = 0,
  NoReadyPeer = 1,
  NoReadyTarget = 2,
  LocationUnavailable = 3,
  CapacityExceeded = 4,
  Draining = 5,
  InternalFailure = 6
}

export interface ZLinkPeerStatus {
  readonly nodeRid: RoutingId;
  readonly state: ZLinkPeerState;
  readonly unavailableReason?: ZLinkTopologyReason;
}

export enum ZLinkTopologyState {
  Starting = 0,
  Ready = 1,
  Degraded = 2,
  Stopping = 3,
  Stopped = 4,
  Failed = 5
}

export interface ZLinkChannelStatus {
  readonly channelName: string;
  readonly isReady: boolean;
  readonly readyTargetCount: number;
}

export interface ZLinkPlacementStatus {
  readonly isAvailable: boolean;
  readonly activeActorCount: number;
  readonly activeSpotCount: number;
  readonly unavailableReason?: ZLinkTopologyReason;
}

export interface ZLinkRouteMeshStatus {
  readonly meshName: string;
  readonly state: ZLinkTopologyState;
  readonly isReady: boolean;
  readonly readyPeerCount: number;
  readonly channels: readonly ZLinkChannelStatus[];
  readonly peers: readonly ZLinkPeerStatus[];
  readonly placement: ZLinkPlacementStatus;
  readonly sequence: bigint;
  readonly observedAt: Date;
}

export interface ZLinkObservationLoss {
  readonly coalescedCount: bigint;
  readonly discardedTerminalCount: bigint;
}

export interface ZLinkObservedStatus<TStatus> {
  readonly status: TStatus;
  readonly loss: ZLinkObservationLoss;
}

export interface ZLinkRouteMeshRuntime {
  snapshot(meshName: string): ZLinkRouteMeshStatus;
  observe(
    meshName: string,
    capacity?: number,
    signal?: AbortSignal
  ): AsyncIterable<ZLinkObservedStatus<ZLinkRouteMeshStatus>>;
  isReady(meshName: string): boolean;
}
