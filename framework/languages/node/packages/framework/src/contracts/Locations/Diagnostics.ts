import type { RoutingId } from '../Common';

export interface ZLinkLocationRuntimeStatus {
  readonly storeHealthy: boolean;
  readonly watchEnabled: boolean;
  readonly pollingIntervalMs: number;
  readonly lastRefreshAt?: Date;
  readonly lastError?: string;
  readonly ownerLeaseHealthy: boolean;
  readonly ownerLeaseRenewedAt?: Date;
}

export enum ZLinkLocationTopologyState {
  Discovered = 1,
  Connecting = 2,
  Ready = 3,
  Lost = 4,
  Error = 5,
  Stopped = 6
}

export interface ZLinkLocationTopologyFilter {
  readonly meshName?: string;
  readonly nodeRid?: RoutingId;
  readonly state?: ZLinkLocationTopologyState;
}

export interface ZLinkLocationTopologyEntry {
  readonly meshName: string;
  readonly nodeRid: RoutingId;
  readonly endpoint: string;
  readonly draining: boolean;
  readonly state: ZLinkLocationTopologyState;
  readonly updatedAt: Date;
}

export interface ZLinkLocationServiceSummaryFilter {
  readonly meshName?: string;
}

export interface ZLinkLocationServiceSummary {
  readonly meshName: string;
  readonly totalCount: number;
  readonly readyCount: number;
  readonly errorCount: number;
  readonly stoppedCount: number;
  readonly lastUpdatedAt: Date;
}
