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
}
