import type {
  ActorRef,
  RoutingId,
  ZLinkActor,
  ZLinkActorJoinOperationId,
  ZLinkSpot
} from '../../contracts';
import type { Message } from '../../contracts/Common/Message';
import type { ZLinkMessageFollowOrigin } from '../foundation/service-runtime-contracts';
import type {
  ZLinkRemoteActorPacketTarget,
  ZLinkRemoteBoundSessionTarget,
  ZLinkDeferredJoinAcceptedRoot,
  ZLinkDeferredJoinRecoveryInput
} from '../actors';
import type { ZLinkActorHandoffDispatch } from '../actors/actor-handoff';
import type { ZLinkActorResponseOptions } from './spot-actor-packet-dispatch';
import type {
  ZLinkRemoteActorJoinActor,
  ZLinkRoutedActorTransferProvider
} from './spot-remote-codec';
import type { ZLinkBackendSpotNode } from '../backend/contracts';

export interface ZLinkEntryActorRuntime {
  resolveActor(actorId: string): ZLinkActor | undefined;
  commitActorTransaction(actor: ZLinkActor, onJoined: () => Promise<void>): Promise<void>;
  destroyActor(
    node: ZLinkBackendSpotNode,
    entryNodeRid: RoutingId,
    actor: ZLinkActor,
    signal?: AbortSignal
  ): Promise<void>;
  routePacket(
    actorId: string,
    parts: readonly Message[],
    returnResponse?: boolean,
    remoteBoundSessionTarget?: ZLinkRemoteBoundSessionTarget,
    fallbackActorRef?: ActorRef
  ): Promise<{ readonly handled: boolean; readonly response?: unknown }>;
}

export interface ZLinkSpotActorTransferRuntime {
  readPreparedTransferState(
    reference: string,
    checksumCrc32c: number,
    signal?: AbortSignal
  ): Promise<Buffer>;
  prepareDeferredJoinAccepted(
    actorId: string,
    operationId: ZLinkActorJoinOperationId,
    actorRef: ActorRef,
    rawReply: Uint8Array,
    signal?: AbortSignal,
    recovery?: ZLinkDeferredJoinRecoveryInput
  ): Promise<ZLinkDeferredJoinAcceptedRoot>;
  recoverDeferredJoinAccepted(
    actorId: string,
    signal?: AbortSignal
  ): Promise<ZLinkDeferredJoinAcceptedRoot | undefined>;
  discardDeferredJoinAccepted(
    root: ZLinkDeferredJoinAcceptedRoot,
    signal?: AbortSignal
  ): Promise<void>;
  markDeferredJoinRecoveryMessageReplayed(
    root: ZLinkDeferredJoinAcceptedRoot,
    nextReplayCursor: number,
    signal?: AbortSignal
  ): Promise<ZLinkDeferredJoinAcceptedRoot>;
  markDeferredJoinAcceptedCommitted(
    root: ZLinkDeferredJoinAcceptedRoot,
    actorRef: ActorRef,
    signal?: AbortSignal
  ): Promise<ZLinkDeferredJoinAcceptedRoot>;
  readDeferredJoinRecoveryPayload(
    root: ZLinkDeferredJoinAcceptedRoot,
    signal?: AbortSignal
  ): Promise<Buffer>;
  takeOverDeferredJoinRecoveryAuthority(
    root: ZLinkDeferredJoinAcceptedRoot,
    targetActorRef: ActorRef,
    target: {
      readonly meshName: string;
      readonly nodeRid: RoutingId;
      readonly nodeGeneration: bigint;
      readonly owner: {
        readonly ownerId: string;
        readonly leaseGeneration: bigint;
      };
      readonly spotId: string;
      readonly spotGeneration: bigint;
      readonly membershipEpoch: bigint;
      readonly spotAuthority: import('../../contracts/Locations').ZLinkAuthoritySnapshot;
      readonly spotAuthorityPayload: Uint8Array;
    },
    signal?: AbortSignal
  ): Promise<{
    readonly root: ZLinkDeferredJoinAcceptedRoot;
    readonly actorAuthority: import('../../contracts/Locations').ZLinkAuthoritySnapshot;
    readonly spotAuthority: import('../../contracts/Locations').ZLinkAuthoritySnapshot;
  } | undefined>;
  commitAndDeliverDeferredJoinAccepted(
    root: ZLinkDeferredJoinAcceptedRoot,
    actor: ZLinkActor,
    actorRef: ActorRef,
    submitMailbox: <T>(operation: () => Promise<T>) => Promise<T>,
    signal?: AbortSignal,
    retainRecoveryRoot?: boolean
  ): Promise<ZLinkDeferredJoinAcceptedRoot>;
  releaseDeferredJoinRecovery(
    root: ZLinkDeferredJoinAcceptedRoot,
    signal?: AbortSignal
  ): Promise<void>;
  getOrCreateRoutedActor(
    actorId: string,
    actorType: string,
    actorRef?: ActorRef,
    remoteBoundSessionTarget?: ZLinkRemoteBoundSessionTarget,
    actorCreateRequest?: Message,
    signal?: AbortSignal
  ): Promise<ZLinkRemoteActorJoinActor>;
  materializeRoutedActor: ZLinkRoutedActorTransferProvider;
  rememberRoutedActorTransferTarget(
    actorId: string,
    target: ZLinkRemoteBoundSessionTarget | undefined
  ): void;
  prepareRecoveryRoutedActor(
    actorId: string,
    actorType: string,
    actorRef: ActorRef,
    authorityOwnerGeneration: bigint,
    spotId: RoutingId,
    spotGeneration: bigint,
    membershipEpoch: bigint,
    adapterKey: string | undefined,
    transferState: Message,
    actorEntryNodeRid: RoutingId | undefined,
    remoteBoundSessionTarget?: ZLinkRemoteBoundSessionTarget,
    signal?: AbortSignal
  ): Promise<ZLinkActor>;
  claimNativeActorLocation(
    actor: ZLinkActor,
    spotId: RoutingId,
    spotMeshName: string
  ): Promise<ZLinkNativeActorJoinSnapshot>;
  claimRoutedActorLocation(
    actor: ZLinkActor,
    spotId: RoutingId,
    spotMeshName: string,
    joinedLocation?: {
      readonly spotGeneration: bigint;
      readonly membershipEpoch: bigint;
    }
  ): Promise<void>;
  publishRoutedActorOwnership(actor: ZLinkActor): Promise<void>;
  publishRecoveryRoutedActor(actor: ZLinkActor): void;
  openRoutedActorSession(actor: ZLinkActor): Promise<void>;
  bindRoutedActorRef(actor: ZLinkActor, actorRef: ActorRef): void;
  currentRoutedActorRef(actor: ZLinkActor): ActorRef | undefined;
  adoptRoutedActorAuthority(
    actor: ZLinkActor,
    authority: import('../../contracts/Locations').ZLinkAuthoritySnapshot,
    spotId: RoutingId,
    spot: ZLinkSpot,
    membershipEpoch: bigint
  ): void;
  commitRoutedActor(actor: ZLinkActor, spotId: RoutingId, spot: ZLinkSpot): void;
  clearRoutedActor(actor: ZLinkActor): void;
  rollbackNativeActorJoin(
    actor: ZLinkActor,
    snapshot: ZLinkNativeActorJoinSnapshot
  ): Promise<void>;
  rollbackRoutedActor(actor: ZLinkActor, signal?: AbortSignal): Promise<void>;
  notifyCoreSourceLeave(actor: ZLinkActor, callback: () => Promise<void>): Promise<void>;
  actorEntryNodeRid(actor: ZLinkActor): RoutingId | undefined;
}

export interface ZLinkNativeActorJoinSnapshot {
  readonly spotId?: RoutingId;
  readonly spot?: ZLinkSpot;
  readonly locationSpotId?: RoutingId;
  readonly spotMeshName?: string;
  readonly actorRef?: ActorRef;
  readonly spotGeneration?: bigint;
  readonly membershipEpoch?: bigint;
  readonly ownerNodeGeneration?: bigint;
}

export interface ZLinkSpotBoundSessionRuntime {
  receiveRoutedBoundSession(
    actorId: string,
    message: unknown,
    packetName: string | undefined,
    metadata: ReadonlyMap<string, string>,
    actorRef?: ActorRef,
    actorPacketTarget?: unknown,
    signal?: AbortSignal
  ): Promise<void>;
  receiveRoutedBoundSessionResponse(
    actorId: string,
    packetName: string,
    requestSeq: bigint,
    message: unknown,
    replyOptions: ZLinkActorResponseOptions,
    actorPacketTarget?: unknown,
    signal?: AbortSignal
  ): Promise<void>;
  receiveRoutedBoundSessionError(
    actorId: string,
    packetName: string,
    requestSeq: bigint,
    error: unknown,
    metadata: ReadonlyMap<string, string>,
    actorPacketTarget?: unknown,
    signal?: AbortSignal
  ): Promise<void>;
  receiveRemoteBoundSessionOwnership(payload: unknown): Promise<{
    readonly actorId: string;
    readonly actorGeneration: string;
    readonly actorOwnershipGeneration: string;
    readonly bindingGeneration: string;
    readonly targetOwnerLeaseGeneration: string;
    readonly acceptedHighWater: string;
    readonly sealId: string;
  }>;
  receiveRemoteBoundSessionSeal(payload: unknown): Promise<{
    readonly actorId: string;
    readonly sealId: string;
    readonly acceptedHighWater: string;
  }>;
  rememberRemoteBoundSessionTarget(actorId: string, target: ZLinkRemoteBoundSessionTarget | undefined): void;
  resolveRemoteBoundSessionTarget(
    sourceNodeRid: RoutingId,
    sourceSessionRid: RoutingId
  ): ZLinkRemoteBoundSessionTarget | undefined;
  actorPacketTargetForState(actorId: string): ZLinkRemoteActorPacketTarget | undefined;
  sendActorResponse(
    actor: ZLinkActor,
    packetName: string,
    requestSeq: bigint,
    response: unknown,
    replyOptions: ZLinkActorResponseOptions,
    fallbackBoundSessionTarget?: ZLinkRemoteBoundSessionTarget,
    fallbackActorRef?: ActorRef,
    signal?: AbortSignal
  ): Promise<void>;
  sendActorError(
    actorId: string,
    packetName: string,
    requestSeq: bigint,
    error: unknown,
    metadata: ReadonlyMap<string, string>,
    fallbackBoundSessionTarget?: ZLinkRemoteBoundSessionTarget,
    actorRef?: ActorRef,
    signal?: AbortSignal
  ): Promise<void>;
}

export interface ZLinkSpotActorHandoffRuntime {
  capture(
    actorId: string,
    parts: readonly Message[],
    returnResponse?: boolean,
    remoteBoundSessionTarget?: ZLinkRemoteBoundSessionTarget,
    fallbackActorRef?: ActorRef,
    deadlineUnixMs?: number,
    messageFollowOrigin?: ZLinkMessageFollowOrigin,
    provisionalReplay?: ZLinkActorHandoffDispatch
  ): Promise<unknown> | undefined;
}
