import type {
  Message,
} from '../../contracts/Common/Message';
import type {
  RequestResult,
  SubmitResult,
  ZLinkBackendMessageLike as MessageLike
} from '../backend/runtime-values';
import type { RoutingId } from '../../contracts';
import type { ServiceActorRef } from './service-stateful-registry';
import type { ServiceDirectSpotRouteFence } from './service-stateful-wire-codec';

export interface MeshOperationId {
  readonly high: bigint;
  readonly low: bigint;
}

export interface ActorTransferId {
  readonly high: bigint;
  readonly low: bigint;
}

export const ReadyDomain = Object.freeze({
  None: 0,
  Application: 1,
  Infrastructure: 2,
  All: 3
} as const);

export const ReadyOwnerKind = Object.freeze({ Node: 1, Spot: 2, Actor: 3 } as const);

export const ReceiveKind = Object.freeze({
  NodeSend: 1,
  NodeRequest: 2,
  ChannelSend: 3,
  ChannelRequest: 4,
  SpotSend: 5,
  SpotRequest: 6,
  SpotMulticast: 7,
  SpotControl: 8,
  ActorSend: 9,
  ActorRequest: 10,
  Completion: 11,
  SendReady: 12,
  TransferControl: 13,
  InstanceSpotActivation: 14,
  ActorBinding: 15
} as const);

export const OperationKind = Object.freeze({
  NodeRequest: 1,
  ChannelRequest: 2,
  SpotRequest: 3,
  ActorRequest: 4,
  ActorLookup: 5,
  ActorDestroy: 6,
  ActorJoin: 7,
  ActorLeave: 8,
  StreamBind: 9,
  StreamUnbind: 10,
  StreamClose: 11,
  InstanceSpotRequest: 12,
  UserSpotCreate: 13,
  UserSpotClose: 14
} as const);

export function operationRequiresReply(operationKind: number): boolean {
  return operationKind === OperationKind.NodeRequest
    || operationKind === OperationKind.ChannelRequest
    || operationKind === OperationKind.SpotRequest
    || operationKind === OperationKind.ActorRequest
    || operationKind === OperationKind.InstanceSpotRequest;
}

export const ActorLifecycleKind = Object.freeze({
  Created: 1,
  Joined: 2,
  Left: 3,
  Disconnected: 4,
  Destroyed: 5
} as const);

export const ActorTransferRole = Object.freeze({ Source: 1, Target: 2 } as const);

export const ActorTransferPhase = Object.freeze({
  Preparing: 1,
  Fenced: 2,
  Committed: 3,
  Activated: 4,
  Aborted: 5
} as const);

export interface ActorLocation {
  readonly actor: ServiceActorRef;
  readonly spotId: RoutingId | null;
  readonly spotGeneration: bigint;
  readonly membershipEpoch: bigint;
}

export interface ActorControlPayload {
  readonly kind: 'actorControl';
  readonly lifecycleKind: number;
  readonly previousActor: ServiceActorRef | null;
  readonly currentActor: ServiceActorRef | null;
  readonly previousSpotId: RoutingId | null;
  readonly currentSpotId: RoutingId | null;
  readonly previousSpotGeneration: bigint;
  readonly currentSpotGeneration: bigint;
  readonly previousMembershipEpoch: bigint;
  readonly currentMembershipEpoch: bigint;
  readonly resultCode: number;
}

export interface ActorJoinCompletionPayload {
  readonly kind: 'actorJoinCompletion';
  readonly joinResult: number;
  readonly actor: ServiceActorRef | null;
  readonly location: ActorLocation;
}

export interface ActorLookupCompletionPayload {
  readonly kind: 'actorLookupCompletion';
  readonly location: ActorLocation;
}

export interface SendReadyPayload {
  readonly kind: 'sendReady';
  readonly destinationKind: number;
  readonly targetNodeRid: RoutingId | null;
  readonly targetSpotId: RoutingId | null;
  readonly targetActor: ServiceActorRef | null;
  readonly channelName: string | null;
}

export interface ActorTransferControlPayload {
  readonly kind: 'transferControl';
  readonly phase: number;
  readonly role: number;
  readonly transferId: ActorTransferId;
  readonly actor: ServiceActorRef | null;
  readonly membershipEpoch: bigint;
  readonly finalSequence: bigint;
  readonly resultCode: number;
  readonly failureErrno: number;
}

export interface ActorBindingControlPayload {
  readonly kind: 'actorBinding';
  readonly actor: ServiceActorRef;
  readonly bindingGeneration: bigint;
  readonly sessionNodeRid: RoutingId;
  readonly sessionRid: RoutingId;
}

export type ReceiveKindData =
  | ActorControlPayload
  | ActorJoinCompletionPayload
  | ActorLookupCompletionPayload
  | SendReadyPayload
  | ActorTransferControlPayload
  | ActorBindingControlPayload;

export interface ReadyRecord {
  readonly ownerKind: number;
  readonly domain: number;
  readonly spotId: RoutingId | null;
  readonly actor: ServiceActorRef | null;
}

export interface ReceiveRequirements {
  readonly messageCount: number;
  readonly partCount: number;
  readonly byteCount: number;
}

export interface ZLinkMessageFollowOrigin {
  readonly sourceNodeRid: RoutingId;
  readonly originalOperation: MeshOperationId;
  readonly originalReplyRouteId: bigint;
}

export interface ReceiveRecord {
  readonly kind: number;
  readonly domain: number;
  readonly sourceNodeRid: RoutingId | null;
  readonly sourceSpotId: RoutingId | null;
  readonly sourceBindingGeneration: bigint;
  readonly sourceActor: ServiceActorRef | null;
  readonly operationId: MeshOperationId;
  readonly operationKind: number;
  readonly channelName: string | null;
  readonly topic: string | null;
  readonly applicationMetadata: Buffer | null;
  readonly packetName?: string;
  readonly contentType?: string;
  readonly kindData: ReceiveKindData | null;
  readonly terminalResult: number;
  readonly failureErrno: number;
  readonly parts: Message[];
  /** Returns whether the local operation can still accept a lifecycle reply. */
  readonly isPending?: () => boolean;
  /** Absolute deadline used by the local lifecycle dispatch fence. */
  readonly deadlineUnixMs?: bigint;
  readonly messageFollowOrigin?: ZLinkMessageFollowOrigin;
  readonly onTerminalCompletion?: () => void | Promise<void>;
  reply(parts: MessageLike | readonly MessageLike[], flags?: number): SubmitResult;
  replyActorJoin(
    joinResult: number,
    parts: MessageLike | readonly MessageLike[],
    flags?: number
  ): SubmitResult;
}

export interface ClaimReceiveResult {
  readonly ok: boolean;
  readonly required?: ReceiveRequirements;
  readonly records: ReceiveRecord[];
}

export interface Claim {
  recvBatch(batch: ReceiveBatch, flags?: number): ClaimReceiveResult;
  release(): void;
}

export interface ReadyBatch {
  readonly records: ReadyRecord[];
  takeClaim(index: number): Claim;
  reset(): void;
  close(): void;
}

export interface ReceiveBatch {
  reset(): void;
  close(): void;
}

export interface MeshNodeStatus {
  readonly state: number;
  readonly routingId: RoutingId;
  readonly meshName: string;
  readonly localEndpoint: string;
  readonly lifecycleGeneration: bigint;
  readonly descriptorRevision: bigint;
  readonly channelCount: number;
  readonly configuredPeerCount: number;
  readonly admittedPeerCount: number;
  readonly drainingPeerCount: number;
  readonly pendingApplicationMessages: bigint;
  readonly pendingInfrastructureMessages: bigint;
  readonly pendingBytes: bigint;
  readonly lastError: number;
  readonly lastChangedMs: bigint;
}

export interface MeshPeerEntry {
  readonly connectionIntentId: bigint;
  readonly source: number;
  readonly state: number;
  readonly routingId: RoutingId | null;
  readonly lifecycleGeneration: bigint;
  readonly descriptorRevision: bigint;
  readonly endpoint: string;
  readonly channelCount: number;
  readonly lastError: number;
  readonly lastChangedMs: bigint;
}

export interface StreamSessionStatus {
  readonly state: number;
  readonly lifecycleGeneration: bigint;
  readonly sessionCount: bigint;
  readonly bindingCount: bigint;
  readonly pendingMessageCount: bigint;
  readonly pendingByteCount: bigint;
  readonly lastError: number;
}

export interface StreamSessionService {
  start(): void;
  shutdown(timeoutMs: number): RequestResult;
  close(): void;
  status(): StreamSessionStatus;
  lookupActor(targetNodeRid: RoutingId, actorId: string, timeoutMs?: number): MeshOperationId;
  bindActor(sessionRid: RoutingId, actor: ServiceActorRef, timeoutMs?: number): MeshOperationId;
  unbindActor(
    sessionRid: RoutingId,
    actor: ServiceActorRef,
    expectedBindingGeneration: bigint,
    timeoutMs?: number
  ): MeshOperationId;
  bindings(sessionRid: RoutingId): ReadonlyArray<{
    readonly sessionRid: RoutingId;
    readonly actor: ServiceActorRef;
    readonly bindingGeneration: bigint;
    readonly membershipEpoch: bigint;
  }>;
  sendToActor(
    sessionRid: RoutingId,
    actor: ServiceActorRef,
    parts: MessageLike | readonly MessageLike[],
    options?: { flags?: number }
  ): SubmitResult;
}

export interface MeshPublisher {
  publish(
    channelName: string,
    topic: string,
    parts: MessageLike | readonly MessageLike[],
    options?: { flags?: number }
  ): void;
  publishAsync(
    channelName: string,
    topic: string,
    parts: MessageLike | readonly MessageLike[],
    options?: { flags?: number },
    signal?: AbortSignal
  ): Promise<void>;
  close(): void;
}

export interface ServiceSpot {
  readonly routingId: RoutingId | null;
  status(): { readonly routingId: RoutingId; readonly lifecycleGeneration: bigint };
  sendToChannel(
    channelName: string,
    parts: MessageLike | readonly MessageLike[],
    options?: { flags?: number }
  ): SubmitResult;
  requestToChannel(
    channelName: string,
    parts: MessageLike | readonly MessageLike[],
    options?: { flags?: number; timeoutMs?: number; applicationMetadata?: Buffer }
  ): MeshOperationId;
  sendToSpot(
    targetNodeRid: unknown,
    targetSpotId: unknown,
    targetSpotGeneration: bigint,
    parts: MessageLike | readonly MessageLike[],
    options?: {
      flags?: number;
      routeFence?: ServiceDirectSpotRouteFence;
      entrySpot?: boolean;
    }
  ): SubmitResult;
  requestToSpot(
    targetNodeRid: unknown,
    targetSpotId: unknown,
    targetSpotGeneration: bigint,
    parts: MessageLike | readonly MessageLike[],
    options?: {
      flags?: number;
      timeoutMs?: number;
      applicationMetadata?: Buffer;
      routeFence?: ServiceDirectSpotRouteFence;
      entrySpot?: boolean;
    }
  ): MeshOperationId;
  publish(
    channelName: string,
    topic: string,
    parts: MessageLike | readonly MessageLike[],
    options?: { flags?: number }
  ): void;
  setSubscription(channelName: string, topicFilter: string, kind?: number): void;
  unsetSubscription(channelName: string, topicFilter: string, kind?: number): void;
  close(): void;
}
