import type { ActorRef, RoutingId } from '../Common';

export type ZLinkActorTransferState = 'prepared' | 'committed' | 'activated' | 'aborted';

export type ZLinkActorTransferWriteStatus =
  | 'stored'
  | 'notFound'
  | 'ignoredStale'
  | 'rejectedConflict'
  | 'invalidState';

export interface ZLinkActorTransferRecord {
  readonly meshName: string;
  readonly actorId: string;
  readonly transferId: string;
  readonly source: ActorRef;
  readonly target: ActorRef;
  readonly expectedActorGeneration: bigint;
  readonly expectedMembershipEpoch: bigint;
  readonly participants: ReadonlySet<RoutingId>;
  readonly state: ZLinkActorTransferState;
  readonly recoveryOwnerId: string;
  readonly recoveryLeaseExpiresAt: Date;
  readonly updatedAt: Date;
}

export interface ZLinkActorTransferPrepareRequest {
  readonly meshName: string;
  readonly actorId: string;
  readonly transferId: string;
  readonly source: ActorRef;
  readonly target: ActorRef;
  readonly expectedActorGeneration: bigint;
  readonly expectedMembershipEpoch: bigint;
  readonly participants: ReadonlySet<RoutingId>;
  readonly recoveryOwnerId: string;
  readonly recoveryLeaseTtlMs: number;
}

export interface ZLinkActorTransferWriteResult {
  readonly status: ZLinkActorTransferWriteStatus;
  readonly record?: ZLinkActorTransferRecord;
}

export interface ZLinkActorTransferStore {
  prepareActorTransfer(
    request: ZLinkActorTransferPrepareRequest,
    signal?: AbortSignal
  ): Promise<ZLinkActorTransferWriteResult>;
  commitActorTransfer(
    meshName: string,
    actorId: string,
    transferId: string,
    recoveryOwnerId: string,
    signal?: AbortSignal
  ): Promise<ZLinkActorTransferWriteResult>;
  activateActorTransfer(
    meshName: string,
    actorId: string,
    transferId: string,
    recoveryOwnerId: string,
    signal?: AbortSignal
  ): Promise<ZLinkActorTransferWriteResult>;
  abortActorTransfer(
    meshName: string,
    actorId: string,
    transferId: string,
    recoveryOwnerId: string,
    signal?: AbortSignal
  ): Promise<ZLinkActorTransferWriteResult>;
  takeOverActorTransfer(
    meshName: string,
    actorId: string,
    transferId: string,
    successorOwnerId: string,
    recoveryLeaseTtlMs: number,
    signal?: AbortSignal
  ): Promise<ZLinkActorTransferWriteResult>;
  resolveActorTransfer(
    meshName: string,
    actorId: string,
    signal?: AbortSignal
  ): Promise<ZLinkActorTransferRecord | undefined>;
}
