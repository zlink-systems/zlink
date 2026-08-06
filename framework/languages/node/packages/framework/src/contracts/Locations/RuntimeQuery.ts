import type {
  ZLinkLocationPage,
  ZLinkLocationRuntimeStatus,
  ZLinkLocationServiceSummary,
  ZLinkLocationServiceSummaryFilter,
  ZLinkLocationTopologyEntry,
  ZLinkLocationTopologyFilter,
  ZLinkMeshNodeDescriptor,
  ZLinkPageRequest,
} from './Models';
import type { ZLinkPlacementObjectKind } from './Authority';

export type ZLinkLocationObjectState = 'creating' | 'ready';

export interface ZLinkLocationObjectEntry {
  readonly globalId: string;
  readonly objectGeneration: bigint;
  readonly meshName: string;
  readonly nodeRid: import('../Common').RoutingId;
  readonly state: ZLinkLocationObjectState;
  readonly stableType: string;
}

export interface ZLinkLocationObjectFilter {
  readonly objectKind: ZLinkPlacementObjectKind;
  readonly stableType?: string;
  readonly meshName?: string;
}

export interface ZLinkLocationRuntimeQuery {
  getStatus(signal?: AbortSignal): Promise<ZLinkLocationRuntimeStatus>;
  listMeshNodeDescriptors(
    meshName: string,
    page?: ZLinkPageRequest,
    signal?: AbortSignal
  ): Promise<ZLinkLocationPage<ZLinkMeshNodeDescriptor>>;
  listTopology(
    filter: ZLinkLocationTopologyFilter,
    page?: ZLinkPageRequest,
    signal?: AbortSignal
  ): Promise<ZLinkLocationPage<ZLinkLocationTopologyEntry>>;
  listServiceSummaries(
    filter: ZLinkLocationServiceSummaryFilter,
    page?: ZLinkPageRequest,
    signal?: AbortSignal
  ): Promise<ZLinkLocationPage<ZLinkLocationServiceSummary>>;
  listObjectLocations(filter: ZLinkLocationObjectFilter, page?: ZLinkPageRequest, signal?: AbortSignal): Promise<ZLinkLocationPage<ZLinkLocationObjectEntry>>;
}
