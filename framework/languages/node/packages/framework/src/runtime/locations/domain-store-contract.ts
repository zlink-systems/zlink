import type {
  ZLinkLocationOwnerToken,
  ZLinkLocationPage,
  ZLinkLocationWriteIntent,
  ZLinkLocationWriteResult,
  ZLinkLocationWriteStatus,
  ZLinkMeshNodeDescriptor,
  ZLinkMeshNodeDescriptorKey,
  ZLinkClientServerServerDescriptor,
  ZLinkClientServerServerDescriptorKey,
  ZLinkFanoutPublisherDescriptor,
  ZLinkFanoutPublisherDescriptorKey,
  ZLinkOwnerLeaseClaimResult,
  ZLinkOwnerLeaseReadResult,
  ZLinkOwnerLeaseReleaseResult,
  ZLinkOwnerLeaseRenewResult,
  ZLinkPageRequest
} from '../../contracts/Locations/Models';
import type {
  ZLinkAggregateAbortResult,
  ZLinkAggregateCommitResult,
  ZLinkAggregateFence,
  ZLinkAggregatePrepareRequest,
  ZLinkAggregatePrepareResult,
  ZLinkAuthorityCompareExchangeResult,
  ZLinkAuthorityKey,
  ZLinkAuthorityMutation,
  ZLinkAuthorityReadResult,
  ZLinkAuthorityScanCursor,
  ZLinkAuthorityScanResult,
  ZLinkAuthorityStoreVersion,
  ZLinkCreationOperationIdentity,
  ZLinkCreationTerminalReadResult,
  ZLinkObjectAbortRequest,
  ZLinkObjectAbortResult,
  ZLinkObjectCommitRequest,
  ZLinkObjectCommitResult,
  ZLinkObjectCreationCompleteRequest,
  ZLinkObjectCreationCompleteResult,
  ZLinkObjectReserveRequest,
  ZLinkObjectReserveResult,
  ZLinkRelocationCapacityAbortResult,
  ZLinkRelocationCapacityFence,
  ZLinkRelocationCapacityReservationRequest,
  ZLinkRelocationCapacityReserveResult
} from '../../contracts/Locations/Authority';

/**
 * Framework-private domain repository. Provider implementations never
 * implement this interface; the framework maps it to the opaque Store SPI.
 */
export interface ZLinkDomainLocationStore {
  updateMeshNode(
    descriptor: ZLinkMeshNodeDescriptor,
    intent: ZLinkLocationWriteIntent,
    signal?: AbortSignal
  ): Promise<ZLinkLocationWriteResult>;
  removeMeshNode(
    key: ZLinkMeshNodeDescriptorKey,
    owner: ZLinkLocationOwnerToken,
    signal?: AbortSignal
  ): Promise<ZLinkLocationWriteStatus>;
  listMeshNodes(
    meshName: string,
    page?: ZLinkPageRequest,
    signal?: AbortSignal
  ): Promise<ZLinkLocationPage<ZLinkMeshNodeDescriptor>>;

  updateClientServer(
    descriptor: ZLinkClientServerServerDescriptor,
    intent: ZLinkLocationWriteIntent,
    signal?: AbortSignal
  ): Promise<ZLinkLocationWriteResult>;
  removeClientServer(
    key: ZLinkClientServerServerDescriptorKey,
    owner: ZLinkLocationOwnerToken,
    signal?: AbortSignal
  ): Promise<ZLinkLocationWriteStatus>;
  listClientServers(
    channelName: string,
    page?: ZLinkPageRequest,
    signal?: AbortSignal
  ): Promise<ZLinkLocationPage<ZLinkClientServerServerDescriptor>>;
  updateFanoutPublisher(
    descriptor: ZLinkFanoutPublisherDescriptor,
    intent: ZLinkLocationWriteIntent,
    signal?: AbortSignal
  ): Promise<ZLinkLocationWriteResult>;
  removeFanoutPublisher(
    key: ZLinkFanoutPublisherDescriptorKey,
    owner: ZLinkLocationOwnerToken,
    signal?: AbortSignal
  ): Promise<ZLinkLocationWriteStatus>;
  listFanoutPublishers(
    channelName: string,
    page?: ZLinkPageRequest,
    signal?: AbortSignal
  ): Promise<ZLinkLocationPage<ZLinkFanoutPublisherDescriptor>>;
  claimOwnerLease(
    ownerId: string,
    leaseTtlMs: number,
    signal?: AbortSignal
  ): Promise<ZLinkOwnerLeaseClaimResult>;
  readOwnerLease(
    ownerId: string,
    signal?: AbortSignal
  ): Promise<ZLinkOwnerLeaseReadResult>;
  renewOwnerLease(
    token: ZLinkLocationOwnerToken,
    leaseTtlMs: number,
    signal?: AbortSignal
  ): Promise<ZLinkOwnerLeaseRenewResult>;
  releaseOwnerLease(
    token: ZLinkLocationOwnerToken,
    signal?: AbortSignal
  ): Promise<ZLinkOwnerLeaseReleaseResult>;

  readAuthority(key: ZLinkAuthorityKey, signal?: AbortSignal): Promise<ZLinkAuthorityReadResult>;
  compareExchangeAuthority(
    key: ZLinkAuthorityKey,
    expectedStoreVersion: ZLinkAuthorityStoreVersion,
    mutation: ZLinkAuthorityMutation,
    signal?: AbortSignal
  ): Promise<ZLinkAuthorityCompareExchangeResult>;
  listAuthorities(
    prefix: string,
    cursor: ZLinkAuthorityScanCursor | undefined,
    limit: number,
    signal?: AbortSignal
  ): Promise<ZLinkAuthorityScanResult>;

  readCreationTerminal(
    operation: ZLinkCreationOperationIdentity,
    signal?: AbortSignal
  ): Promise<ZLinkCreationTerminalReadResult>;
  reserve(request: ZLinkObjectReserveRequest, signal?: AbortSignal): Promise<ZLinkObjectReserveResult>;
  commit(request: ZLinkObjectCommitRequest, signal?: AbortSignal): Promise<ZLinkObjectCommitResult>;
  completeCreation(
    request: ZLinkObjectCreationCompleteRequest,
    signal?: AbortSignal
  ): Promise<ZLinkObjectCreationCompleteResult>;
  abort(request: ZLinkObjectAbortRequest, signal?: AbortSignal): Promise<ZLinkObjectAbortResult>;

  reserveRelocationCapacity(
    request: ZLinkRelocationCapacityReservationRequest,
    signal?: AbortSignal
  ): Promise<ZLinkRelocationCapacityReserveResult>;
  abortRelocationCapacity(
    fence: ZLinkRelocationCapacityFence,
    signal?: AbortSignal
  ): Promise<ZLinkRelocationCapacityAbortResult>;
  prepareAggregate(
    request: ZLinkAggregatePrepareRequest,
    signal?: AbortSignal
  ): Promise<ZLinkAggregatePrepareResult>;
  commitAggregate(fence: ZLinkAggregateFence, signal?: AbortSignal): Promise<ZLinkAggregateCommitResult>;
  abortAggregate(fence: ZLinkAggregateFence, signal?: AbortSignal): Promise<ZLinkAggregateAbortResult>;

  removeAllByOwner(owner: ZLinkLocationOwnerToken, signal?: AbortSignal): Promise<bigint>;
  getMeshNodeChangeStamp?(meshName: string, signal?: AbortSignal): Promise<bigint | undefined>;
}
