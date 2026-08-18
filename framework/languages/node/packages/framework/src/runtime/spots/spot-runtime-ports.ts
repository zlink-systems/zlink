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
  ZLinkDeferredJoinAcceptedRoot
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
  prepareDeferredJoinAccepted(
    actorId: string,
    operationId: ZLinkActorJoinOperationId,
    actorRef: ActorRef,
    rawReply: Uint8Array,
    signal?: AbortSignal
  ): Promise<ZLinkDeferredJoinAcceptedRoot>;
  discardDeferredJoinAccepted(
    root: ZLinkDeferredJoinAcceptedRoot,
    signal?: AbortSignal
  ): Promise<void>;
  markDeferredJoinAcceptedCommitted(
    root: ZLinkDeferredJoinAcceptedRoot,
    actorRef: ActorRef,
    signal?: AbortSignal
  ): Promise<ZLinkDeferredJoinAcceptedRoot>;
  commitAndDeliverDeferredJoinAccepted(
    root: ZLinkDeferredJoinAcceptedRoot,
    actor: ZLinkActor,
    actorRef: ActorRef,
    submitMailbox: <T>(operation: () => Promise<T>) => Promise<T>,
    signal?: AbortSignal
  ): Promise<ZLinkDeferredJoinAcceptedRoot>;
  getOrCreateRoutedActor(
    actorId: string,
    actorType: string,
    actorRef?: ActorRef,
    remoteBoundSessionTarget?: ZLinkRemoteBoundSessionTarget,
    actorCreateRequest?: Message,
    signal?: AbortSignal
  ): Promise<ZLinkRemoteActorJoinActor>;
  materializeRoutedActor: ZLinkRoutedActorTransferProvider;
  commitRoutedActorAuthority(
    actor: ZLinkActor,
    sourceActorRef: ActorRef,
    transferId: string,
    spotId: RoutingId,
    spotGeneration: bigint,
    membershipEpoch: bigint,
    deadlineAtMs: number,
    signal?: AbortSignal
  ): Promise<import('../../contracts/Locations').ZLinkAuthoritySnapshot>;
  rememberRoutedActorTransferTarget(
    actorId: string,
    target: ZLinkRemoteBoundSessionTarget | undefined
  ): void;
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
  openRoutedActorSession(actor: ZLinkActor): Promise<void>;
  bindRoutedActorRef(actor: ZLinkActor, actorRef: ActorRef): void;
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
