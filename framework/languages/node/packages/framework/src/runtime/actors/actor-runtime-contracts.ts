import type { ZLinkProviderResolver } from '../../contracts/Common/ZLinkProviderResolver';
import type {
  RoutingId,
  ActorRef,
  Type,
  ZLinkActor,
  ZLinkActorFactory,
  ZLinkBoundSession,
  ZLinkMessage,
  ZLinkActorCreateResponse,
  ZLinkMessageSerializer,
} from '../../contracts';
import type { Message } from '../../contracts/Common/Message';
import type { ZLinkBackendMeshNode, ZLinkMeshCompletionTable } from '../backend';
import type { ZLinkLocationLifecycle } from '../locations';
import type { ZLinkActorRuntimeState } from './actor-runtime-state';
import type { ZLinkActorTransferRegistry } from './actor-transfer-registry';
import type { ZLinkRuntimeAdmissionGate } from '../admission';

export interface ZLinkActorJoinRuntimeResult<TReply> {
  readonly accepted: boolean;
  readonly actor?: import('../../contracts').ActorRef;
  readonly reply?: TReply;
  /** Internal terminal work that must run after the completion callback. */
  readonly finalizeDeferredJoin?: () => Promise<void>;
}

export interface ZLinkActorManagerOptions {
  readonly actorFactories: ReadonlyMap<string, Type | ZLinkActorFactory>;
  readonly joinCoordinator?: ZLinkActorJoinCoordinator;
  readonly actorMeshNameProvider?: (actorType: string) => string | undefined;
  readonly actorLeaveSpot?: (
    meshName: string,
    spotId: RoutingId,
    actor: ZLinkActor,
    signal?: AbortSignal
  ) => Promise<void>;
  readonly messageSerializers?: ReadonlyMap<string, ZLinkMessageSerializer>;
  readonly nativeActorNode?: ZLinkBackendMeshNode;
  readonly nativeActorNodeProvider?: () => ZLinkBackendMeshNode | undefined;
  readonly nativeActorCompletionTableProvider?: () => ZLinkMeshCompletionTable | undefined;
  readonly actorCreatedNodeRidProvider?: () => RoutingId | undefined;
  readonly actorRefResolver?: ZLinkActorRuntimeLocationLookup;
  readonly actorCreatedNotifier?: (
    nodeRid: RoutingId,
    actor: ZLinkActor,
    createRequest: ZLinkMessage,
    signal?: AbortSignal
  ) => Promise<ZLinkActorCreateResponse | undefined>;
  readonly actorDestroyedCleanup?: (actorId: string) => void;
  readonly publishActorAuthority?: (
    actorType: string,
    actorRef: ActorRef,
    ownerNodeGeneration: bigint,
    signal?: AbortSignal
  ) => Promise<void>;
  readonly locationLifecycle?: ZLinkLocationLifecycle;
  readonly boundSessionFactory?: ZLinkActorBoundSessionFactory;
  readonly providerResolver?: ZLinkProviderResolver;
  readonly actorTransferRegistry?: ZLinkActorTransferRegistry;
  readonly shutdownSignal?: AbortSignal;
  readonly metrics?: import('../diagnostics').ZLinkRuntimeMetrics;
  readonly admission?: ZLinkRuntimeAdmissionGate;
  readonly placementCreate?: (
    actorId: string,
    actorType: string,
    createOnly: boolean,
    options: {
      readonly meshName?: string;
      readonly request?: unknown;
      readonly timeoutMs: number;
    },
    signal?: AbortSignal
  ) => Promise<import('../../contracts').ZLinkActorCreateResult>;
}

export interface ZLinkActorRuntimeLocationLookup {
  resolveActorRef(
    actorId: string,
    signal?: AbortSignal
  ): Promise<ActorRef | undefined>;
}

export type ZLinkActorBoundSessionFactory = (actorId: string) => ZLinkBoundSession;

export interface ZLinkActorJoinCoordinator {
  /**
   * Starts the internal packet handoff before a Deferred Join performs target
   * lookup. This is an internal ordering fence; it is not part of the public
   * actor context contract.
   */
  beginDeferredJoin?(
    actor: ZLinkActor,
    state: ZLinkActorRuntimeState,
    operationId: import('../../contracts').ZLinkActorJoinOperationId
  ): void;
  abortDeferredJoin?(
    actor: ZLinkActor,
    state: ZLinkActorRuntimeState,
    operationId: import('../../contracts').ZLinkActorJoinOperationId
  ): Promise<void>;
  joinSpot(
    actor: ZLinkActor,
    state: ZLinkActorRuntimeState,
    spotId: RoutingId,
    request: Message,
    timeoutMs: number | undefined,
    signal: AbortSignal | undefined,
    completionOperationId?: import('../../contracts').ZLinkActorJoinOperationId
  ): Promise<ZLinkActorJoinRuntimeResult<Message>>;
  joinEntrySpot(
    actor: ZLinkActor,
    state: ZLinkActorRuntimeState,
    nodeRid: RoutingId | undefined,
    request: Message,
    timeoutMs: number | undefined,
    signal: AbortSignal | undefined,
    completionOperationId?: import('../../contracts').ZLinkActorJoinOperationId
  ): Promise<ZLinkActorJoinRuntimeResult<Message>>;
}
