import type {
  RequestResult,
  SubmitResult,
  ZLinkBackendMessageLike,
  ZLinkBackendMonitorEventType,
  ZLinkBackendReceived,
  ZLinkBackendRequestCallback,
  ZLinkBackendTopicMessage
} from '../runtime-values';
import {
  type ZLinkBackendRecvFlags,
  type ZLinkBackendSendFlags
} from '../runtime-values';
export * from '../runtime-values';
type MessageLike = ZLinkBackendMessageLike;
type Received = ZLinkBackendReceived;
type RequestCallback = ZLinkBackendRequestCallback;
type TopicMessage = ZLinkBackendTopicMessage;
type MonitorEventType = ZLinkBackendMonitorEventType;
import type {
  MeshNodeStatus,
  MeshOperationId,
  MeshPeerEntry,
  MeshPublisher,
  ReadyBatch,
  ReadyRecord,
  ReceiveBatch,
  ServiceSpot,
  StreamSessionService
} from '../../foundation/service-runtime-contracts';
import type {
  RoutingId
} from '../../../contracts';
import type { ZLinkSubmitResult } from '../../messaging/submission-result';
import type { Message } from '../../../contracts/Common/Message';
import type {
  ServiceSpotMessageFollowSeal,
  ServiceUserSpotOperationHandler,
  ServiceUserSpotOperationResult
} from '../../foundation/service-stateful-runtime';
import type {
  ServiceActorCreateRecord,
  ServiceDirectSpotRouteFence,
  ServiceInstanceRouteFence,
  ServiceUserSpotCloseRecord,
  ServiceUserSpotCreateRecord
} from '../../foundation/service-stateful-wire-codec';

export type ZLinkBackendSpotNodeMode = number;
export const ZLINK_BACKEND_SPOT_NODE_MODE_PUBSUB = 1 as ZLinkBackendSpotNodeMode;
export const ZLINK_BACKEND_SPOT_NODE_MODE_ROUTED = 2 as ZLinkBackendSpotNodeMode;
export const ZLINK_BACKEND_SPOT_NODE_MODE_ALL = 3 as ZLinkBackendSpotNodeMode;
export const ZLINK_BACKEND_SPOT_ROUTE_BRIDGE_ROUTE_ONLY = 0x00000001;
export const ZLINK_BACKEND_SPOT_ROUTE_BRIDGE_ROUTE_WITH_CHANNEL_INBOUND = 0x00000003;

export type ZLinkBackendMeshNodeStatus = MeshNodeStatus;
export type ZLinkBackendMeshPeerEntry = MeshPeerEntry;
export type ZLinkBackendReadyBatch = ReadyBatch;
export type ZLinkBackendReceiveBatch = ReceiveBatch;

export type ZLinkBackendObjectPlacement =
  | {
      readonly kind: 'selected';
      readonly target: {
        readonly targetNodeRid: string;
        readonly targetNodeGeneration: bigint;
        readonly descriptorVersion: string;
      };
    }
  | { readonly kind: 'unsupported' }
  | { readonly kind: 'capacity' }
  | { readonly kind: 'unavailable' };

export interface ZLinkBackendMeshNode {
  setRoutingId(routingId: unknown): void;
  setBind(endpoint: string): void;
  start(): void;
  setInboundMessageDroppedHandler?(handler: (
    surface: 'node' | 'channel',
    messageKind: 'send',
    reason: 'backpressure'
  ) => void): void;
  setMessageFollowHandler?(handler: (
    record: import('../../foundation/service-stateful-wire-codec').ServiceMessageFollowRecord
  ) => void): void;
  sendMessageFollowNotification?(
    targetNodeRid: string,
    record: Omit<
      import('../../foundation/service-stateful-wire-codec').ServiceMessageFollowRecord,
      'kind'
    >
  ): void;
  shutdown(timeoutMs: number): RequestResult;
  close(): void;
  addChannelName(name: string): void;
  setChannelWeight(name: string, weight: number): void;
  setPlacementWeight(weight: number): void;
  configureObjectPlacement(options: {
    readonly role: 'none' | 'client' | 'server';
    readonly placementWeight: number;
    readonly activeCapacityLimit: number;
    readonly pendingCapacityLimit: number;
    readonly objectCapabilities: readonly string[];
  }): void;
  selectObjectPlacement(stableType: string): ZLinkBackendObjectPlacement;
  instanceSpotPlacementTypes?(): readonly string[];
  sendToMissingInstanceSpot(
    target: {
      readonly targetNodeRid: string;
      readonly targetNodeGeneration: bigint;
      readonly targetSpotId: string;
      readonly stableType: string;
      readonly descriptorVersion: string;
    },
    parts: ZLinkBackendMessageLike | readonly ZLinkBackendMessageLike[],
    deadlineUnixMs: bigint,
    sourceSpotId?: string,
    metadata?: ReadonlyMap<string, string>
  ): SubmitResult;
  /** Prepares one immutable Missing Instance send for bounded admission retries. */
  prepareMissingInstanceSpotSend?(
    target: {
      readonly targetNodeRid: string;
      readonly targetNodeGeneration: bigint;
      readonly targetSpotId: string;
      readonly stableType: string;
      readonly descriptorVersion: string;
    },
    parts: ZLinkBackendMessageLike | readonly ZLinkBackendMessageLike[],
    deadlineUnixMs: bigint,
    sourceSpotId?: string,
    metadata?: ReadonlyMap<string, string>
  ): () => SubmitResult;
  requestToMissingInstanceSpot(
    target: {
      readonly targetNodeRid: string;
      readonly targetNodeGeneration: bigint;
      readonly targetSpotId: string;
      readonly stableType: string;
      readonly descriptorVersion: string;
    },
    parts: ZLinkBackendMessageLike | readonly ZLinkBackendMessageLike[],
    deadlineUnixMs: bigint,
    sourceSpotId?: string,
    metadata?: ReadonlyMap<string, string>
  ): MeshOperationId;
  sendToInstanceSpot(
    route: ServiceInstanceRouteFence,
    parts: MessageLike | readonly MessageLike[],
    sourceSpotId?: string,
    metadata?: ReadonlyMap<string, string>
  ): SubmitResult;
  requestInstanceSpot(
    route: ServiceInstanceRouteFence,
    parts: MessageLike | readonly MessageLike[],
    timeoutMs?: number,
    sourceSpotId?: string,
    metadata?: ReadonlyMap<string, string>
  ): MeshOperationId;
  registerUserSpotOperationHandler(handler: ServiceUserSpotOperationHandler): void;
  requestUserSpotCreate(
    targetNodeRid: string,
    request: Omit<ServiceUserSpotCreateRecord, 'kind' | 'correlation' | 'operation'>,
    timeoutMs: number
  ): Promise<ServiceUserSpotOperationResult>;
  requestUserSpotClose(
    targetNodeRid: string,
    request: Omit<ServiceUserSpotCloseRecord, 'kind' | 'correlation' | 'operation'>,
    timeoutMs: number
  ): Promise<ServiceUserSpotOperationResult>;
  requestActorCreate(
    targetNodeRid: string,
    request: Omit<ServiceActorCreateRecord, 'kind' | 'correlation' | 'operation'>,
    timeoutMs: number
  ): Promise<ServiceUserSpotOperationResult>;
  connectPeer(options: {
    readonly endpoint: string;
    readonly expectedRid?: unknown;
    readonly expectedSecurityIdentity?: string;
    readonly expectedLifecycleGeneration?: bigint;
  }): bigint;
  removePeerConnection(intentId: bigint): void;
  disconnectPeer(peerRid: unknown, lifecycleGeneration: bigint): void;
  replaceDiscoveredNotRequiredPeers?(peers: readonly {
    readonly nodeRoutingId: string;
    readonly lifecycleGeneration: bigint;
    readonly descriptorRevision: bigint;
    readonly endpoint: string;
  }[]): void;
  isObjectClientNodeDirectTarget?(targetRid: unknown): boolean;
  isPeerRouteReady?(targetRid: unknown, lifecycleGeneration?: bigint): boolean;
  sendToNode(
    targetRid: unknown,
    parts: MessageLike | readonly MessageLike[],
    options?: { flags?: number }
  ): SubmitResult;
  requestToNode(
    targetRid: unknown,
    parts: MessageLike | readonly MessageLike[],
    options?: { flags?: number; timeoutMs?: number; applicationMetadata?: Buffer }
  ): MeshOperationId;
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
  status(): MeshNodeStatus;
  peers(): MeshPeerEntry[];
  peerChannels(
    peerRid: unknown,
    lifecycleGeneration: bigint
  ): { readonly names: string[]; readonly weights: number[] };
  createPublisher(): MeshPublisher;
  createSpot(): ServiceSpot;
  entrySpot(): ServiceSpot;
  getOrCreateSpot(routingId: unknown): { readonly spot: ServiceSpot; readonly created: boolean };
  instanceSpotApplicationTarget?(spotId: string): {
    readonly stableType: string;
    readonly objectGeneration: bigint;
  } | undefined;
  waitForInstanceApplicationQuiescence?(spotId: string, signal?: AbortSignal): Promise<void>;
  restoreUserSpotAuthority?(
    spotId: string,
    stableType: string,
    generation: bigint,
    authorityOwnerGeneration: bigint
  ): ServiceSpot;
  restoreSpotAuthority?(
    spotId: string,
    objectKind: 'user_spot' | 'instance_spot',
    stableType: string,
    generation: bigint,
    authorityOwnerGeneration: bigint
  ): ServiceSpot;
  createActor(
    actorId: string,
    parts?: MessageLike | readonly MessageLike[]
  ): ZLinkBackendActorRef;
  restoreActorAuthority?(
    actorId: string,
    stableType: string,
    generation: bigint,
    authorityOwnerGeneration: bigint,
    spotId: string,
    spotGeneration: bigint,
    membershipEpoch: bigint
  ): ZLinkBackendActorRef;
  discardRelocatedActor?(actor: ZLinkBackendActorRef): void;
  restoreActorSessionBinding?(
    actor: ZLinkBackendActorRef,
    sessionNodeRid: unknown,
    sessionRid: unknown,
    bindingGeneration: bigint
  ): void;
  actorLookup(actorId: string): {
    readonly actor: ZLinkBackendActorRef;
    readonly spotId: unknown;
    readonly spotGeneration: bigint;
    readonly membershipEpoch: bigint;
  };
  lookupRemoteActor(targetNodeRid: unknown, actorId: string, timeoutMs?: number): MeshOperationId;
  destroyActor(actor: ZLinkBackendActorRef, timeoutMs?: number): MeshOperationId;
  rememberSpotRoute?(
    route: ServiceDirectSpotRouteFence
  ): void;
  forgetSpotRoute?(
    spot: ServiceDirectSpotRouteFence['spot'],
    authorityOwnerGeneration: bigint,
    storeVersion?: string
  ): void;
  sealSpotMessageFollowIngress?(
    source: ServiceDirectSpotRouteFence
  ): ServiceSpotMessageFollowSeal | undefined;
  abortSpotMessageFollowIngress?(seal: ServiceSpotMessageFollowSeal): boolean;
  commitSpotMessageFollowIngress?(
    seal: ServiceSpotMessageFollowSeal,
    target: ServiceDirectSpotRouteFence,
    durationMs: number
  ): Promise<boolean>;
  joinActorSpot(
    actor: ZLinkBackendActorRef,
    targetNodeRid: unknown,
    targetSpotId: unknown,
    targetSpotGeneration: bigint,
    parts?: MessageLike | readonly MessageLike[],
    timeoutMs?: number
  ): MeshOperationId;
  joinActorEntrySpot(
    actor: ZLinkBackendActorRef,
    targetNodeRid: unknown,
    parts?: MessageLike | readonly MessageLike[],
    timeoutMs?: number
  ): MeshOperationId;
  sendToActor(
    actor: ZLinkBackendActorRef,
    parts: MessageLike | readonly MessageLike[],
    options?: { flags?: number }
  ): SubmitResult;
  requestToActor(
    actor: ZLinkBackendActorRef,
    parts: MessageLike | readonly MessageLike[],
    options?: { flags?: number; timeoutMs?: number; applicationMetadata?: Buffer }
  ): MeshOperationId;
  actorSendToActor(
    source: ZLinkBackendActorRef,
    target: ZLinkBackendActorRef,
    parts: MessageLike | readonly MessageLike[],
    options?: { flags?: number }
  ): SubmitResult;
  actorRequestToActor(
    source: ZLinkBackendActorRef,
    target: ZLinkBackendActorRef,
    parts: MessageLike | readonly MessageLike[],
    options?: { flags?: number; timeoutMs?: number; applicationMetadata?: Buffer }
  ): MeshOperationId;
  sendActorBoundSession(
    actor: ZLinkBackendActorRef,
    expectedBindingGeneration: bigint,
    parts: MessageLike | readonly MessageLike[],
    flags?: number
  ): SubmitResult;
  closeActorBoundSession(
    actor: ZLinkBackendActorRef,
    expectedBindingGeneration: bigint,
    timeoutMs?: number
  ): MeshOperationId;
  leaveActor(
    actor: ZLinkBackendActorRef,
    expectedMembershipEpoch: bigint,
    timeoutMs?: number
  ): MeshOperationId;
  setReadyHandler(handler: (readyDomains: number) => number): void;
  createReadyBatch(capacity: number): ReadyBatch;
  createReceiveBatch(messageCapacity: number, partCapacity: number, byteCapacity: number): ReceiveBatch;
  drainReady(
    domains: number,
    batch: ReadyBatch,
    flags?: number
  ): { readonly ok: boolean; readonly hasResidue: boolean; readonly records: readonly ReadyRecord[] };
  createStreamSessionService(stream: unknown): StreamSessionService;
}

export enum ZLinkBackendSpotDispatchEvent {
  SubscribeReadable = 1,
  RoutedReadable = 2,
  TimerReadable = 3,
  ChannelReplyReadable = 4,
  ActorReadable = 5,
  ActorJoinReadable = 6,
  ActorLifecycleReadable = 7
}

export interface ZLinkBackendActorRef {
  readonly nodeRid: RoutingId;
  readonly actorId: string;
  readonly generation: bigint;
}

export interface ZLinkBackendActorSessionNode {
  sendActorBoundSession(
    actor: ZLinkBackendActorRef,
    expectedBindingGeneration: bigint,
    parts: readonly Message[],
    flags: number
  ): ZLinkSubmitResult;
  closeActorBoundSession(
    actor: ZLinkBackendActorRef,
    expectedBindingGeneration: bigint,
    timeoutMs: number,
    signal?: AbortSignal
  ): Promise<void>;
}

export interface ZLinkBackendActorJoinResult {
  readonly result: RequestResult;
  readonly joinResultCode: number;
  readonly actor: ZLinkBackendActorRef;
  readonly joinedSpotId: RoutingId;
  readonly joinEpoch: bigint;
  readonly flags: number;
}

export interface ZLinkBackendActorJoinEntrySpotResult {
  readonly result: RequestResult;
  readonly joinResultCode: number;
  readonly actor: ZLinkBackendActorRef;
  readonly targetNodeRid: RoutingId;
  readonly joinedSpotId: RoutingId;
  readonly joinEpoch: bigint;
  readonly flags: number;
}

export type ZLinkBackendActorJoinCallback = (
  result: ZLinkBackendActorJoinResult,
  parts: readonly Message[]
) => void;

export type ZLinkBackendActorJoinEntrySpotCallback = (
  result: ZLinkBackendActorJoinEntrySpotResult,
  parts: readonly Message[]
) => void;

export interface ZLinkBackendActorPart {
  readonly actor: ZLinkBackendActorRef;
  readonly sourceNodeRid: RoutingId;
  readonly sourceSessionRid: RoutingId;
  readonly requestId: bigint;
  readonly flags: number;
  readonly message: Message;
  readonly more: boolean;
}

export interface ZLinkBackendActorRecvInfo {
  readonly actor: ZLinkBackendActorRef;
  readonly sourceNodeRid: RoutingId;
  readonly sourceSessionRid: RoutingId;
  readonly requestId: bigint;
  readonly flags: number;
}

export interface ZLinkBackendActorJoinInfo {
  readonly sourceActor: ZLinkBackendActorRef;
  readonly targetActor: ZLinkBackendActorRef;
  readonly sourceNodeRid: RoutingId;
  readonly sourceSpotId: RoutingId;
  readonly targetNodeRid: RoutingId;
  readonly targetSpotId: RoutingId;
  readonly joinEpoch: bigint;
  readonly flags: number;
}

export interface ZLinkBackendActorJoinRequest {
  readonly info: ZLinkBackendActorJoinInfo;
  readonly message: Message;
}

export interface ZLinkBackendActorJoinReplyOperation {
  message(message: Message): ZLinkBackendActorJoinReplyOperation;
  submit(): void;
}

export interface ZLinkBackendSpotDispatchInfo {
  readonly event: ZLinkBackendSpotDispatchEvent;
  readonly subjectKind?: number;
  readonly subjectHandle?: bigint;
  readonly routed?: Received | null;
}

export interface ZLinkBackendSocketMonitorEvent {
  readonly nativeEvent: MonitorEventType;
  readonly routingId?: unknown;
  readonly localAddr: string;
  readonly remoteAddr: string;
  readonly value: number;
}

export interface ZLinkBackendObject {
  readonly nativeInstance: unknown;
}

export interface ZLinkBackendContext extends ZLinkBackendObject {
  shutdown(): void;
  dispose(): Promise<void>;
}

export interface ZLinkBackendSocket extends ZLinkBackendObject {
  readonly lastEndpoint?: string;
  bind(endpoint: string): void;
  setChannelName(channelName: string): void;
  dispose(): Promise<void>;
}

export interface ZLinkBackendConnectableSocket extends ZLinkBackendSocket {
  connect(endpoint: string): void;
  disconnect(endpoint: string): void;
}

export interface ZLinkBackendDealerSocket extends ZLinkBackendConnectableSocket {
  setRoutingId(routingId: RoutingId): void;
  peerWeight: number;
  sendHighWaterMark: number;
  receiveHighWaterMark: number;
  sendTimeoutMs: number;
  maxMessageSize: number;
  onSendReady(handler: () => void): void;
  send(message: Message | readonly Message[], flags: ZLinkBackendSendFlags): boolean;
  request(
    message: Message | readonly Message[],
    callback: RequestCallback,
    flags: ZLinkBackendSendFlags,
    timeoutMs?: number
  ): boolean;
  recv(flags?: ZLinkBackendRecvFlags): Received | undefined;
}

export interface ZLinkBackendReplyOperation {
  message(message: unknown): ZLinkBackendReplyOperation;
  submit(): void;
  flags(flags: ZLinkBackendSendFlags): { submit(): void };
}

export interface ZLinkBackendSendOperation {
  message(message: MessageLike): ZLinkBackendSendOperation;
  flags(flags: ZLinkBackendSendFlags): ZLinkBackendSendOperation;
  submit(): boolean;
}

export interface ZLinkBackendRequestOperation {
  message(message: MessageLike): ZLinkBackendRequestOperation;
  timeout(timeoutMs: number): ZLinkBackendRequestOperation;
  flags(flags: ZLinkBackendSendFlags): ZLinkBackendRequestOperation;
  submit(callback: RequestCallback): boolean;
}

export interface ZLinkBackendRouterSocket extends ZLinkBackendConnectableSocket {
  readonly options?: {
    probe?: boolean;
    setConnectRoutingId?(routingId: RoutingId): void;
  };
  peerWeight: number;
  sendHighWaterMark: number;
  receiveHighWaterMark: number;
  sendTimeoutMs: number;
  maxMessageSize: number;
  onSendReady(handler: () => void): void;
  setRoutingId(routingId: RoutingId): void;
  recv(flags?: ZLinkBackendRecvFlags): Received | undefined;
  send(
    routingId: RoutingId,
    message: Message | readonly Message[],
    flags: ZLinkBackendSendFlags
  ): boolean;
  request(
    routingId: RoutingId,
    message: Message | readonly Message[],
    callback: RequestCallback,
    flags: ZLinkBackendSendFlags,
    timeoutMs?: number
  ): boolean;
  sendToSpot(
    targetNodeRid: RoutingId,
    targetSpotId: RoutingId,
    parts: readonly Message[],
    flags: ZLinkBackendSendFlags
  ): boolean;
  requestToSpot(
    targetNodeRid: RoutingId,
    targetSpotId: RoutingId,
    parts: readonly Message[],
    callback: RequestCallback,
    flags: ZLinkBackendSendFlags,
    timeoutMs?: number
  ): boolean;
  disconnectPeer(routingId: RoutingId): void;
  reply(routingId: RoutingId, requestSeq: bigint): ZLinkBackendReplyOperation;
  reply(routingId: RoutingId, requestSeq: bigint, message: Message | readonly Message[]): void;
}

export interface ZLinkBackendPublisherSocket extends ZLinkBackendSocket {
  sendHighWaterMark: number;
  onSendReady(handler: () => void): void;
  publish(topic: string, message: Message | readonly Message[], flags: ZLinkBackendSendFlags): boolean;
}

export interface ZLinkBackendSubscriberSocket extends ZLinkBackendConnectableSocket {
  setSubscription(topic: string): void;
  subscribe(result: TopicMessage, flags?: ZLinkBackendRecvFlags): boolean;
}

export interface ZLinkBackendReadablePoller {
  wait(timeoutMs: number): boolean;
  dispose(): void;
}

export interface ZLinkBackendStreamSocket extends ZLinkBackendSocket {
  readonly sendTimeoutMs: number;
  readonly sendHighWaterMark: number;
  /** Framework stream ingress uses this bound while assembling raw recv parts. */
  maxMessageSize: number;
  onSendReady(handler: () => void): void;
  setTlsServer(cert: string, key: string, requireClientCert?: boolean): void;
  recv(flags?: ZLinkBackendRecvFlags): Received | undefined;
  send(routingId: RoutingId, payload: Message | readonly Message[], flags: ZLinkBackendSendFlags): boolean;
  disconnectPeer(routingId: RoutingId): void;
  bindActor(
    sessionRid: RoutingId,
    actor: ZLinkBackendActorRef,
    timeoutMs: number,
    signal?: AbortSignal
  ): Promise<void>;
  unbindActor(
    sessionRid: RoutingId,
    actorId: string,
    timeoutMs: number,
    signal?: AbortSignal
  ): Promise<void>;
  sendBoundActor(
    sessionRid: RoutingId,
    actorId: string,
    parts: readonly Message[],
    flags: ZLinkBackendSendFlags
  ): boolean;
}

export interface ZLinkBackendSocketMonitor extends ZLinkBackendObject {
  onEvent(handler: (event: ZLinkBackendSocketMonitorEvent) => void): void;
  recv(): ZLinkBackendSocketMonitorEvent;
  dispose(): Promise<void>;
}

export interface ZLinkBackendSpotRouteBridge extends ZLinkBackendObject {
  attachRouterChannel(
    channelName: string,
    router: ZLinkBackendRouterSocket,
    options?: { readonly capabilities?: number }
  ): void;
  send(channelName: string, targetNodeRid: RoutingId, targetSpotId: RoutingId): ZLinkBackendSendOperation;
  request(channelName: string, targetNodeRid: RoutingId, targetSpotId: RoutingId): ZLinkBackendRequestOperation;
  handleRouterReceived(
    channelName: string,
    sourceNodeRid: RoutingId,
    requestSeq: bigint | number,
    parts: readonly MessageLike[]
  ): boolean;
  dispose(): Promise<void>;
}

export interface ZLinkBackendSpotNode extends ZLinkBackendObject {
  readonly routingId: RoutingId;
  setRoutingId(routingId: RoutingId): void;
  setPublisherRoutingId(routingId: RoutingId): void;
  setSubscriberRoutingId(routingId: RoutingId): void;
  setRouterBind(endpoint: string): void;
  setPubBind(endpoint: string): void;
  connectPeer(endpoint: string): void;
  connectPeerRid(targetNodeRid: RoutingId, endpoint: string): void;
  disconnectPeerRid(targetNodeRid: RoutingId): void;
  disconnectPeer(endpoint: string): void;
  createSpot(): ZLinkBackendSpot;
  getOrCreateSpot(spotId: RoutingId): { readonly spot: ZLinkBackendSpot; readonly created: boolean };
  status(): ZLinkBackendMeshNodeStatus;
  peers(): readonly ZLinkBackendMeshPeerEntry[];
  subjects(): readonly unknown[];
  createRouteBridge(): ZLinkBackendSpotRouteBridge;
  entrySpot(): ZLinkBackendSpot;
  createActor(actorId: string, request?: Message | readonly Message[]): ZLinkBackendActorRef;
  actorLookup(actorId: string): ZLinkBackendActorRef | undefined;
  joinActor(
    actor: ZLinkBackendActorRef,
    destNodeRid: RoutingId,
    destSpotId: RoutingId,
    payload: Message | readonly Message[],
    callback: RequestCallback | ZLinkBackendActorJoinCallback,
    timeoutMs?: number
  ): boolean;
  joinActorEntrySpot(
    actor: ZLinkBackendActorRef,
    destNodeRid: RoutingId,
    request: Message,
    callback: ZLinkBackendActorJoinEntrySpotCallback,
    timeoutMs?: number
  ): boolean;
  destroyActor(actor: ZLinkBackendActorRef, timeoutMs: number, signal?: AbortSignal): Promise<void>;
  sendActorBoundSession(
    actor: ZLinkBackendActorRef,
    expectedBindingGeneration: bigint,
    parts: readonly Message[],
    flags: ZLinkBackendSendFlags
  ): boolean;
  sendToActor(
    actor: ZLinkBackendActorRef,
    parts: readonly Message[],
    flags: ZLinkBackendSendFlags
  ): boolean | Promise<boolean>;
  requestToActor(
    actor: ZLinkBackendActorRef,
    parts: readonly Message[],
    callback: RequestCallback,
    flags: ZLinkBackendSendFlags,
    timeoutMs?: number
  ): boolean;
  replyActorNoBind(
    info: ZLinkBackendActorRecvInfo,
    parts: readonly Message[],
    result: RequestResult
  ): void;
  bindRemoteActorSession(
    actor: ZLinkBackendActorRef,
    sourceNodeRid: RoutingId,
    sourceSessionRid: RoutingId
  ): void;
  closeActorBoundSession(
    actor: ZLinkBackendActorRef,
    expectedBindingGeneration: bigint,
    timeoutMs: number,
    signal?: AbortSignal
  ): Promise<void>;
  dispose(): Promise<void>;
}

export interface ZLinkBackendSpot extends ZLinkBackendObject {
  readonly routingId: RoutingId;
  /** Core lifecycle generation when this object adapts a formal RouteMesh Spot. */
  readonly lifecycleGeneration?: bigint;
  setRoutingId(routingId: RoutingId): void;
  status(): { readonly routingId: RoutingId; readonly lifecycleGeneration: bigint };
  close(): void;
  setSubscription(channelName: string, topic: string): void;
  subscribe(result: TopicMessage, flags: ZLinkBackendRecvFlags): boolean;
  recvActorLifecycle(flags: ZLinkBackendRecvFlags): unknown | null;
  drainReply(): number;
  drainChannelReply(subjectHandle: bigint): number;
  recvRoute(result: Received, flags: ZLinkBackendRecvFlags): boolean;
  setDispatchHandler(handler: (info: ZLinkBackendSpotDispatchInfo) => void): void;
  onSendReady(handler: () => void): void;
  requestToChannel(
    channelName: string,
    payload: Message | readonly Message[],
    callback: RequestCallback,
    flags: ZLinkBackendSendFlags,
    timeoutMs?: number
  ): boolean;
  sendToChannel(channelName: string, payload: Message | readonly Message[], flags: ZLinkBackendSendFlags): boolean;
  publish(topic: string, payload: Message | readonly Message[], flags: ZLinkBackendSendFlags): boolean;
  sendToSpot(
    targetRid: RoutingId,
    spotId: RoutingId,
    payload: Message | readonly Message[],
    flags: ZLinkBackendSendFlags
  ): boolean;
  requestToSpot(
    targetRid: RoutingId,
    spotId: RoutingId,
    payload: Message | readonly Message[],
    callback: RequestCallback,
    flags: ZLinkBackendSendFlags,
    timeoutMs?: number
  ): boolean;
  recvActorJoin(flags: ZLinkBackendRecvFlags): ZLinkBackendActorJoinRequest | null;
  replyActorJoin(
    request: ZLinkBackendActorJoinRequest,
    joinResultCode: number
  ): ZLinkBackendActorJoinReplyOperation;
  dispose(): Promise<void>;
}

export interface ZLinkChannelBackendAdapter {
  createContext(): ZLinkBackendContext;
  createTopicMessage(): TopicMessage;
  createDealerSocket(context: ZLinkBackendContext): ZLinkBackendDealerSocket;
  createRouterSocket(context: ZLinkBackendContext): ZLinkBackendRouterSocket;
  createPublisherSocket(context: ZLinkBackendContext): ZLinkBackendPublisherSocket;
  createSubscriberSocket(context: ZLinkBackendContext): ZLinkBackendSubscriberSocket;
  createReadablePoller(socket: ZLinkBackendConnectableSocket): ZLinkBackendReadablePoller;
}

export interface ZLinkMeshBackendAdapter {
  createMeshNode(
    context: ZLinkBackendContext,
    options: {
      readonly meshName: string;
      readonly routingId?: RoutingId;
      readonly trustProfile?: string;
    }
  ): ZLinkBackendMeshNode;
}

export interface ZLinkStreamBackendAdapter {
  createStreamSocket(context: ZLinkBackendContext): ZLinkBackendStreamSocket;
  createReadablePoller(socket: ZLinkBackendStreamSocket): ZLinkBackendReadablePoller;
}

export interface ZLinkMonitoringBackendAdapter {
  openSocketMonitor(socket: ZLinkBackendSocket): ZLinkBackendSocketMonitor;
}

export interface ZLinkBackendAdapterFactory {
  createReceived(): Received;
  createTopicMessage(): TopicMessage;
  createChannelAdapter(): ZLinkChannelBackendAdapter;
  createMeshAdapter(): ZLinkMeshBackendAdapter;
  createStreamAdapter(): ZLinkStreamBackendAdapter;
  createMonitoringAdapter(): ZLinkMonitoringBackendAdapter;
}
