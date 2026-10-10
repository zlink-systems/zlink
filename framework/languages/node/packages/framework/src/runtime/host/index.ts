import { listAllMeshNodeDescriptors } from '../locations/location-store-pages';
import { ZlinkStreamContentType } from '@zlink-systems/stream-wire';
import { randomBytes, randomUUID } from 'node:crypto';
import {
  type ActorRef,
  type RoutingId,
  type ZLinkClientServerRuntime,
  type ZLinkFanoutRuntime,
  type ZLinkFrameworkLifecycleOptions,
  type ZLinkFrameworkRelocationOptions,
  type ZLinkFrameworkRelocationResult,
  type ZLinkFrameworkRuntime,
  type ZLinkFrameworkRuntimeStatus,
  type ZLinkFrameworkTerminationResult,
  type ZLinkListenerKind,
  type ZLinkListenerStatus,
  type ZLinkMeshNodeDescriptor,
  type ZLinkMessageFlowControl,
  type ZLinkMessageFlowLogMode,
  type ZLinkObservedStatus,
  type ZLinkRouteMeshRuntime,
  ZLinkFrameworkRelocationMode,
  ZLinkFrameworkRelocationOutcome,
  ZLinkFrameworkRelocationReason,
  ZLinkFrameworkRuntimeState,
  ZLinkFrameworkTerminationOutcome,
  ZLinkFrameworkTerminationReason,
  zlinkMessageMetadata,
  ZLinkObjectRole,
  ZLinkPeerState,
  ZLinkSpotCreateState,
  ZLinkSpotKind
} from '../../contracts';
import type { Type } from '../../contracts/Common/CoreTypes';
import type { ZLinkProviderResolver } from '../../contracts/Common/ZLinkProviderResolver';
import { ZLinkConfigurationException } from '../../contracts/Configuration/ConfigurationException';
import { requireMessageFlowLogMode } from '../../contracts/Configuration/DiagnosticsValidation';
import { DEFAULT_ACTIVATION_CONCURRENCY_LIMIT } from '../../contracts/Configuration/InternalDefaults';
import { DEFAULT_REQUEST_TIMEOUT_MS } from '../../contracts/Configuration/Registration';
import {
  ZLinkDispatchErrorSurface,
  ZLinkDispatchMessageKind,
  ZLinkRuntimeMessageFlowOutcome as ZLinkMessageFlowOutcome
} from '../../contracts/Dispatch/ZLinkDispatchOptions';
import {
  type ZLinkLocationOwnerToken,
  zlinkDefaultLocationOptions,
  type ZLinkLocationRuntimeQuery
} from '../../contracts/Locations';
import { ZLINK_PROVIDER_MAX_PAGE_SIZE } from '../../contracts/Locations/Stores';
import { createDeadlineExceededError, isAbortError, isDeadlineExceededError } from '../abort';
import { ZLinkActivationAdmission } from '../activation-admission';
import {
  type DefaultZLinkActorManager,
  type ZLinkActorManagerOptions,
  decodeRemoteActorSourceLeaveTerminal,
  DefaultZLinkActorClient,
  isActorAuthorityPayload,
  publishInitialActorAuthority,
  rewriteActorAuthorityOwner,
  ZLINK_REMOTE_ACTOR_PACKET_RELAY_PACKET,
  ZLinkActorHandoffCoordinator,
  ZLinkActorTransferRegistry
} from '../actors';
import { messageFollowOwnerNodeRid, ownerFence } from '../actors/actor-message-follow-context';
import {
  ZLINK_INTERNAL_ACTOR_TRANSPORT_DELIVERY_GATE,
  type ZLinkInternalActorTransportDeliveryGate
} from '../actors/actor-transport-delivery-gate';
import { ZLinkRuntimeAdmissionGate } from '../admission';
import {
  ZLINK_INTERNAL_APPLICATION_JOB_QUEUE_HANDLER_START_GATE,
  type ZLinkInternalApplicationJobQueueHandlerStartGate
} from '../application-jobs/application-job-queue-handler-start-gate';
import {
  releaseApplicationJobPermitBeforeHandler,
  runWithApplicationJobPermit
} from '../application-jobs/application-job-queue-scope';
import {
  type ZLinkBackendAdapterFactory,
  type ZLinkBackendContext,
  type ZLinkBackendMeshNode,
  meshActorSessionNodeAdapter,
  ZLinkNodeBackendAdapterFactory
} from '../backend';
import { ZLinkBufferMessage as RuntimeMessage } from '../backend/runtime-message';
import {
  RequestResult,
  SubmitResult,
  type ZLinkBackendMessageLike as MessageLike
} from '../backend/runtime-values';
import {
  DefaultZLinkChannelRuntimeOptions,
  ZLinkChannelRuntimeManager,
  ZLinkDispatchErrorReporter,
  ZLinkRuntimeRouteTransport,
  type ZLinkDispatchErrorSink
} from '../channels';
import {
  decodeChannelEnvelope,
  decodeChannelPayload,
  encodeChannelErrorReplyParts,
  encodeChannelReplyParts
} from '../channels/channel-envelope';
import type { ZLinkFrameworkRegistration } from '../configuration';
import {
  type ZLinkRuntimeEventPublisher,
  createDiagnosticsContext,
  createInboundFlow,
  currentOrCreateFlow,
  DefaultZLinkRuntimeEventPublisher,
  flowIfEnabled,
  runWithFlow,
  ZLinkRuntimeMetrics,
  type ZLinkDiagnosticsContext,
  type ZLinkMessageFlowModeCell,
  type ZLinkRuntimeMetricMeshSnapshot
} from '../diagnostics';
import { METRIC_NAMES, MILLISECONDS_PER_SECOND } from '../diagnostics/runtime-metrics';
import { RuntimeEventQueue } from '../diagnostics/runtime-observation-queue';
import {
  ZLinkClientServerRuntimeProjection,
  ZLinkFanoutRuntimeProjection
} from '../diagnostics/topology-runtime-projections';
import { ZLinkFrameworkExecutionState, ZLinkRuntimeTaskErrorSink } from '../execution';
import { ZLinkListenerRecords } from '../foundation/listener-records';
import {
  runtimeAcceptsWork,
  runtimeObservationIsTerminal,
  runtimeStateIsReady
} from '../foundation/runtime-state-projections';
import { rewriteServiceAuthorityOwner } from '../foundation/service-authority-payload-codec';
import { ServiceRelocationAuthorityError } from '../foundation/service-relocation-coordinator';
import {
  replaceServiceRelocationAuthorityApplicationPayload,
  serviceRelocationAuthorityApplicationPayload
} from '../foundation/service-relocation-runtime';
import {
  MeshPeerRuntimeState,
  OperationKind,
  ReceiveKind,
  type ReadyRecord,
  type ReceiveRecord
} from '../foundation/service-runtime-contracts';
import type {
  ServiceAsyncInstanceActivationAuthority,
  ServiceInstanceApplicationLifecycle
} from '../foundation/service-stateful-runtime';
import type { ServiceMessageFollowRecord } from '../foundation/service-stateful-wire-codec';
import {
  createInternalFrameworkException,
  internalFrameworkWireReply,
  ZLinkFrameworkInternalErrorKind
} from '../framework-errors-internal';
import {
  ZLinkAuthoritySpotRouteResolver,
  ZLinkOwnerCleanupError,
  type ZLinkLocationRuntime,
  type ZLinkStoreLocationResolvers
} from '../locations';
import {
  decodeFrameworkCreationPayload,
  encodeFrameworkCreationPayload
} from '../messaging/creation-payload-codec';
import {
  decodeFrameworkPayloadMessage,
  encodeFrameworkPayloadMessage
} from '../messaging/payload-codec';
import { ZLinkSubmitStatus } from '../messaging/submission-result';
import { decodeRoutingId, encodeRoutingIdStorageHex, routingIdsEqual } from '../routing-id';
import {
  DefaultZLinkSpotManager,
  ZLinkPublicSpotManager,
  ZLinkRuntimeSpotPublisherTransport,
  ZLinkSpotNodeRuntimeManager,
  type ZLinkDetachedTaskRunner,
  type ZLinkSpotManagerOptions
} from '../spots';
import type { ZLinkSpotRouteResolver } from '../spots/spot-routing-internal';
import {
  DefaultZLinkBoundSessionFactory,
  ZLinkStreamBindingRuntime,
  ZLinkStreamRuntimeManager,
  type DefaultZLinkBoundSession,
  type ZLinkStreamPayloadCodec
} from '../streams';
import { ZLinkActorPlacementCoordinator } from './actor-placement-coordinator';
import { ZLinkActorRuntimeOptionsFactory } from './actor-runtime-options-factory';
import { ZLinkActorTransferAuthorityRuntime } from './actor-transfer-authority-runtime';
import { ZLinkActorTransferRuntime } from './actor-transfer-runtime';
import {
  ApplicationJobQueue,
  nodeEffectiveProcessorCount,
  resolveApplicationJobQueueConfiguration
} from './application-job-queue';
import { ZLinkBoundSessionRelay } from './bound-session-relay';
import { ZLinkChannelRuntimeOptionsFactory } from './channel-runtime-options-factory';
import { ZLinkEntryActorRuntimeService } from './entry-actor-runtime';
import { HostCapacityStatusProjection } from './host-capacity-status';
import { ZLinkInstanceActivationAuthority } from './instance-activation-authority';
import { ZLinkLocationRuntimeOwner } from './location-runtime-owner';
import { MeshRouterResolver } from './mesh-router-resolver';
import {
  ZLinkDrainingStatePublishError,
  ZLinkRetiringRollbackError,
  ZLinkRouteMeshRuntimeCoordinator
} from './route-mesh-runtime';
import { DefaultZLinkRouteMeshRuntimeOptions } from './route-mesh-runtime-options';
import { rollbackRuntimeStart, stopRuntimeParts } from './runtime-shutdown';
import {
  ZLinkHostServiceRelocationRuntime,
  ZLinkRelocationStateIncompatibleError
} from './service-relocation-host-runtime';
import { hasObjectClientCapability, ZLinkHostSpotAddressTransport } from './spot-address-transport';
import { ZLinkSpotNodeRuntimeOptionsFactory } from './spot-node-runtime-options-factory';
import { ZLinkSpotRuntimeOptionsFactory } from './spot-runtime-options-factory';
import { ZLinkStatefulAuthorityRouteRuntime } from './stateful-authority-route-runtime';
import { ZLinkUserSpotCreationCoordinator } from './user-spot-creation-coordinator';

const HOST_SHUTDOWN_POLL_INTERVAL_MS = 100;
const HANDOFF_ACCEPTANCE_POLL_INTERVAL_MS = 10;
const DEFAULT_HOST_CONTROL_TIMEOUT_MS = 30_000;

export interface ZLinkFrameworkRuntimeLifecycle {
  readonly isStarted: boolean;
  readonly locationRuntimeQuery?: ZLinkLocationRuntimeQuery;
  start(): Promise<void>;
  stop(): Promise<void>;
}

export interface ZLinkFrameworkRuntimeHostOptions {
  readonly registration: ZLinkFrameworkRegistration;
  readonly lifecycleSink?: string[];
  readonly providerResolver?: ZLinkProviderResolver;
  readonly runtimeEventPublisher?: ZLinkRuntimeEventPublisher;
}

export class ZLinkFrameworkRuntimeHost
  implements ZLinkFrameworkRuntimeLifecycle, ZLinkFrameworkRuntime, ZLinkMessageFlowControl
{
  private readonly backendAdapterFactory: ZLinkBackendAdapterFactory;
  private readonly lifecycleSink?: string[];
  private executionState?: ZLinkFrameworkExecutionState;
  private runtimeState = ZLinkFrameworkRuntimeState.Preparing;
  private runtimeSequence = 0n;
  private readonly runtimeObservationSource = Symbol('zlink.framework-runtime');
  private runtimeDeadline?: Date;
  private shutdownDeadline?: AbortController;
  private runtimeRelocationResult?: ZLinkFrameworkRelocationResult;
  private runtimeTerminationResult?: ZLinkFrameworkTerminationResult;
  private relocationOperation?: Promise<ZLinkFrameworkRelocationResult>;
  private relocationStopStarting?: AbortController;
  private relocationOperationKey?: string;
  private relocationOperationStartedAt?: number;
  private shutdownOperation?: Promise<ZLinkFrameworkTerminationResult>;
  private shutdownOperationStartedAt?: number;
  private relocationTargetApplicationVersion?: bigint;
  private readonly runtimeObservers = new Set<RuntimeEventQueue<ZLinkFrameworkRuntimeStatus>>();
  private channelRuntime?: ZLinkChannelRuntimeManager;
  private spotNodeRuntime?: ZLinkSpotNodeRuntimeManager;
  private streamRuntime?: ZLinkStreamRuntimeManager;
  private statefulAuthorityRoutes?: ZLinkStatefulAuthorityRouteRuntime;
  private readonly locationOwner: ZLinkLocationRuntimeOwner;
  private readonly meshRouters: MeshRouterResolver;
  private readonly boundSessionRelay: ZLinkBoundSessionRelay;
  private readonly actorHandoff: ZLinkActorHandoffCoordinator;
  private readonly actorTransferRegistry: ZLinkActorTransferRegistry;
  private readonly actorTransferRuntime: ZLinkActorTransferRuntime;
  private readonly actorTransferAuthorityRuntime: ZLinkActorTransferAuthorityRuntime;
  private readonly serviceRelocation: ZLinkHostServiceRelocationRuntime;
  private readonly entryActorRuntime: ZLinkEntryActorRuntimeService;
  private actorManager?: DefaultZLinkActorManager;
  private actorPlacement?: ZLinkActorPlacementCoordinator;
  private spotManager?: DefaultZLinkSpotManager;
  private userSpotCoordinator?: ZLinkUserSpotCreationCoordinator;
  private ownerLeaseRecoveryRuntime?: ZLinkLocationRuntime;
  private ownerLeaseRecoveryHandler?: () => void;
  private registerUserSpotHandlers?: (runtime: ZLinkSpotNodeRuntimeManager) => void;
  private readonly destroyedActorRefs = new Map<string, ActorRef>();
  private readonly runtimeEventPublisher: ZLinkRuntimeEventPublisher;
  private readonly metrics: ZLinkRuntimeMetrics;
  private readonly applicationJobQueue: ApplicationJobQueue;
  private readonly capacityStatus: HostCapacityStatusProjection;
  private readonly metricRegistrations: import('../diagnostics').ZLinkRuntimeMetricRegistration[] =
    [];
  private readonly admission = new ZLinkRuntimeAdmissionGate(() => this.ownerAdmissionOpen());
  private readonly activationAdmission = new ZLinkActivationAdmission(
    (meshName) =>
      this.options.registration.spotNodes.get(meshName)?.activationConcurrencyLimit ??
      DEFAULT_ACTIVATION_CONCURRENCY_LIMIT
  );
  private cachedLocationSpotRouteResolver?: ZLinkSpotRouteResolver;
  private actorClientLocationResolver?: ZLinkStoreLocationResolvers;
  // Shared, runtime-mutable message-flow mode cell — installed once so
  // setMessageFlowMode flips every surface live. Seeded from config at start().
  private readonly messageFlowModeCell: ZLinkMessageFlowModeCell = {
    mode: 'errors'
  };
  private readonly dispatchErrorReporters = new WeakMap<
    ZLinkDispatchErrorSink,
    ZLinkDispatchErrorReporter
  >();
  private readonly runtimeOrPreStartErrorSink: ZLinkDispatchErrorSink = {
    reportRuntimeTaskException: (taskName: string, error: unknown) =>
      (this.errorSink ?? this.preStartErrorSink).reportRuntimeTaskException(taskName, error)
  };
  private cachedDiagnosticsContext?: ZLinkDiagnosticsContext;
  private readonly preStartErrorSink = new ZLinkRuntimeTaskErrorSink();
  readonly channelTransport = () => this.channelRuntime;
  readonly channelRuntimeOptions = new DefaultZLinkChannelRuntimeOptions(() => this.channelRuntime);
  readonly routeMeshRuntimeOptions = new DefaultZLinkRouteMeshRuntimeOptions(
    () => this.spotNodeRuntime
  );
  readonly routeTransport: ZLinkRuntimeRouteTransport;
  readonly spotAddressTransport: ZLinkHostSpotAddressTransport;
  readonly spotPublisherTransport = new ZLinkRuntimeSpotPublisherTransport(
    () => this.spotNodeRuntime
  );
  readonly streamBindingRuntime: ZLinkStreamBindingRuntime;
  readonly boundSessionFactory: DefaultZLinkBoundSessionFactory;
  readonly spotRouterChannelIdForMesh: (meshName: string) => string;
  readonly routeMeshRuntime: ZLinkRouteMeshRuntime;
  readonly clientServerRuntime: ZLinkClientServerRuntime;
  readonly fanoutRuntime: ZLinkFanoutRuntime;
  private readonly routeMeshCoordinator: ZLinkRouteMeshRuntimeCoordinator;
  constructor(
    readonly options: ZLinkFrameworkRuntimeHostOptions,
    internalOptions?: unknown
  ) {
    this.backendAdapterFactory = resolveBackendAdapterFactory(internalOptions);
    this.lifecycleSink = options.lifecycleSink;
    this.runtimeEventPublisher =
      options.runtimeEventPublisher ?? new DefaultZLinkRuntimeEventPublisher();
    this.metrics = new ZLinkRuntimeMetrics(options.registration.metrics?.meterProvider);
    this.applicationJobQueue = new ApplicationJobQueue(
      resolveApplicationJobQueueConfiguration(options.registration.applicationJobQueue, () =>
        nodeEffectiveProcessorCount(options.registration.worker?.maxThreads)
      ),
      undefined,
      () => {
        try {
          return (
            this.options.providerResolver
              ?.get?.(
                ZLINK_INTERNAL_APPLICATION_JOB_QUEUE_HANDLER_START_GATE as unknown as Type<ZLinkInternalApplicationJobQueueHandlerStartGate>
              )
              ?.shouldHoldPermitBeforeHandler() === true
          );
        } catch {
          return false;
        }
      },
      (error) =>
        (this.errorSink ?? this.preStartErrorSink).reportRuntimeTaskException(
          'logger-provider',
          error
        )
    );
    this.capacityStatus = new HostCapacityStatusProjection(
      options.registration.coreHwm,
      this.applicationJobQueue
    );
    this.clientServerRuntime = new ZLinkClientServerRuntimeProjection(
      () => this.channelRuntime,
      () => this.runtimeState
    );
    this.fanoutRuntime = new ZLinkFanoutRuntimeProjection(
      () => this.channelRuntime,
      () => this.runtimeState
    );
    this.meshRouters = new MeshRouterResolver(options.registration);
    this.spotRouterChannelIdForMesh = this.meshRouters.spotRouterChannelIdByMesh();
    this.routeTransport = new ZLinkRuntimeRouteTransport(
      () => this.channelRuntime,
      (routerChannelId) => this.meshRouters.canUseRouterChannel(routerChannelId),
      () => this.spotNodeRuntime,
      { serializers: options.registration.messageSerializers },
      (meshName, targetNodeRid) =>
        this.meshRouters.classifyManualNodeTarget(meshName, targetNodeRid),
      (meshName, sourceNodeRid, parts) => this.submitLocalMeshRoute(meshName, sourceNodeRid, parts),
      this.metrics,
      () => this.flowCreationEnabled(),
      () => this.createDispatchErrorReporter(this.runtimeOrPreStartErrorSink).flow
    );
    this.spotAddressTransport = new ZLinkHostSpotAddressTransport({
      resolver: () => this.createLocationSpotRouteResolver(),
      routed: this.routeTransport,
      meshNames: () =>
        [...this.options.registration.spotNodes]
          .filter(([, node]) => hasObjectClientCapability(node.objectRole))
          .map(([meshName]) => meshName),
      isMeshConfigured: (meshName) => this.options.registration.spotNodes.has(meshName),
      meshNode: (meshName) => this.spotNodeRuntime?.meshNode(meshName),
      completions: (meshName) => this.spotNodeRuntime?.meshCompletionTable(meshName),
      codecs: { serializers: options.registration.messageSerializers },
      defaultRequestTimeoutMs: options.registration.requestTimeoutMs ?? DEFAULT_REQUEST_TIMEOUT_MS,
      dispatchErrors: this.createDispatchErrorReporter(this.runtimeOrPreStartErrorSink)
    });
    this.locationOwner = new ZLinkLocationRuntimeOwner({
      registration: options.registration,
      runtimeEventPublisher: this.runtimeEventPublisher,
      metrics: this.metrics,
      fallbackNodeRid: `node-${randomUUID()}`,
      rewriteAuthorityPayloadForOwner
    });
    this.streamBindingRuntime = new ZLinkStreamBindingRuntime({
      dispatchErrors: this.createDispatchErrorReporter(this.runtimeOrPreStartErrorSink),
      streamPayloadCodec: resolveStreamPayloadCodec(options.registration),
      streamCompression: options.registration.streamCompression,
      messageSerializers: options.registration.messageSerializers,
      sessionRelocationSealTimeoutMs:
        options.registration.locations.options.sessionRelocationSealTimeoutMs ??
        zlinkDefaultLocationOptions.sessionRelocationSealTimeoutMs,
      metrics: this.metrics,
      flowCreationEnabled: () => this.flowCreationEnabled(),
      errorSink: () => this.errorSink ?? this.preStartErrorSink,
      nativeActorNodeProvider: () => this.spotNodeRuntime?.primaryMeshNode,
      nativeActorMeshNameProvider: () => this.meshRouters.primaryMeshName(),
      actorAuthorityFenceResolver: async (actorId, signal) => {
        const resolution = await this.createActorLocationResolver()?.resolveDirectActorRoute(
          actorId,
          signal
        );
        return resolution === undefined || resolution.kind !== 'ready'
          ? undefined
          : {
              authorityOwnerGeneration: resolution.route.authorityOwnerGeneration,
              ownerLeaseGeneration: resolution.route.ownerLeaseGeneration,
              ownerId: resolution.route.ownerId,
              ownerNodeGeneration: resolution.route.ownerNodeGeneration,
              authorityStoreVersion: resolution.route.authorityStoreVersion,
              actorType: resolution.route.actorType
            };
      },
      confirmRemoteActorSessionBinding: (actor, sessionRid, signal, options) => {
        const sessionNode = this.spotNodeRuntime?.primaryMeshNode;
        return sessionNode === undefined
          ? Promise.resolve()
          : this.boundSessionRelay.actorPackets.confirmRemoteSessionBinding(
              actor,
              sessionNode.status().routingId as never,
              sessionRid,
              signal,
              options
            );
      },
      relay: (actor, header, payload) =>
        this.boundSessionRelay.actorPackets.relayActorPacket(actor, header, payload),
      notifyDisconnected: (actor, signal) =>
        this.boundSessionRelay.actorPackets.notifyBoundActorDisconnected(actor, signal)
    });
    this.actorHandoff = new ZLinkActorHandoffCoordinator({
      routedTransport: this.routeTransport,
      messageFollowDurationMs: options.registration.locations.options.messageFollowDurationMs,
      requestTimeoutMs: options.registration.requestTimeoutMs,
      requestSource: (actorId) => {
        const owner = this.locationOwner.currentRuntime?.currentOwnerToken;
        const state = this.actorManager?.getState(actorId);
        const current = state?.nativeActorRef;
        const meshName = state?.meshName;
        const node =
          meshName === undefined ? undefined : this.spotNodeRuntime?.meshNode(meshName)?.status();
        if (
          owner === undefined ||
          state === undefined ||
          current === undefined ||
          meshName === undefined ||
          node === undefined ||
          state.locationGeneration === undefined ||
          state.ownerLeaseGeneration === undefined ||
          state.ownerLeaseGeneration !== owner.leaseGeneration ||
          !routingIdsEqual(node.routingId, current.nodeRid)
        ) {
          throw new ZLinkConfigurationException(
            `Actor '${actorId}' handoff requires a committed source owner fence.`
          );
        }
        return {
          meshName,
          objectGeneration: current.generation,
          ownerId: owner.ownerId,
          ownerLeaseGeneration: state.ownerLeaseGeneration,
          nodeRid: String(current.nodeRid),
          nodeRidHex: encodeRoutingIdStorageHex(current.nodeRid),
          nodeGeneration: node.lifecycleGeneration,
          authorityOwnerGeneration: state.locationGeneration
        };
      },
      validateReplySource: (source) => {
        const owner = this.locationOwner.currentRuntime?.currentOwnerToken;
        const node = this.spotNodeRuntime?.meshNode(source.meshName)?.status();
        if (
          owner === undefined ||
          node === undefined ||
          owner.ownerId !== source.ownerId ||
          owner.leaseGeneration !== source.ownerLeaseGeneration ||
          node.lifecycleGeneration !== source.nodeGeneration
        ) {
          return false;
        }
        return routingIdsEqual(node.routingId, decodeRoutingId(source.nodeRid, source.nodeRidHex));
      },
      onMarker: (marker, actorId, index, context) => {
        if (marker === 'message_follow_relay' && context !== undefined) {
          this.options.providerResolver
            ?.get?.(
              ZLINK_INTERNAL_ACTOR_TRANSPORT_DELIVERY_GATE as unknown as Type<ZLinkInternalActorTransportDeliveryGate>
            )
            ?.recordMessageFollowRelay?.(actorId, context);
        }
        if (marker === 'message_follow_route_removed') {
          this.serviceRelocation.completeActorJoinSourceCleanup(actorId);
        }
        this.publishActorHandoffEvent({ marker, actorId, index });
      },
      onRequestFrame: (actorId, index, requestSeq, flags) => {
        this.publishActorHandoffEvent({
          marker: 'handoff_request_frame',
          actorId,
          index,
          requestSeq: requestSeq?.toString(),
          flags
        });
      },
      onMessageFollowRelayed: async (
        actorId,
        targetActorRef,
        context,
        origin,
        queuedMessages,
        queuedBytes
      ) => {
        try {
          const objectGeneration = BigInt(context.objectGeneration);
          if (targetActorRef.objectGeneration !== objectGeneration) {
            throw new Error(`Actor Message Follow changed object generation for '${actorId}'.`);
          }
          const sent = await this.spotNodeRuntime?.sendMessageFollowNotification(
            messageFollowOwnerNodeRid(context.sourceOwner),
            origin.sourceNodeRid,
            {
              source: {
                kind: 'actor',
                actor: {
                  actorId,
                  generation: objectGeneration,
                  nodeRid: messageFollowOwnerNodeRid(context.sourceOwner)
                },
                targetNodeRid: messageFollowOwnerNodeRid(context.sourceOwner),
                targetNodeGeneration: BigInt(context.sourceOwner.nodeGeneration),
                authorityOwnerGeneration: BigInt(context.sourceOwner.authorityOwnerGeneration),
                ownerLeaseGeneration: BigInt(context.sourceOwner.ownerLeaseGeneration)
              },
              target: {
                kind: 'actor',
                actor: {
                  actorId,
                  generation: objectGeneration,
                  nodeRid: messageFollowOwnerNodeRid(context.targetOwner)
                },
                targetNodeRid: messageFollowOwnerNodeRid(context.targetOwner),
                targetNodeGeneration: BigInt(context.targetOwner.nodeGeneration),
                authorityOwnerGeneration: BigInt(context.targetOwner.authorityOwnerGeneration),
                ownerLeaseGeneration: BigInt(context.targetOwner.ownerLeaseGeneration)
              },
              hopCount: context.hopCount,
              queuedMessages,
              queuedBytes,
              originalOperation: origin.originalOperation,
              originalReplyRouteId: origin.originalReplyRouteId
            }
          );
          if (sent !== true) {
            throw new Error(
              `Actor Message Follow source runtime '${context.sourceOwner.nodeRid}' is unavailable.`
            );
          }
          return true;
        } catch (error) {
          this.runtimeOrPreStartErrorSink.reportRuntimeTaskException(
            'actor Message Follow notification',
            error
          );
          return false;
        }
      },
      isStaleActorRef: (actorId, actorRef) => {
        const state = this.actorManager?.getState(actorId);
        const current = state?.nativeActorRef;
        if (actorRef === undefined) return state?.remoteActorPacketTarget !== undefined;
        return (
          current !== undefined &&
          (current.generation !== actorRef.objectGeneration ||
            !routingIdsEqual(current.nodeRid, actorRef.nodeRid))
        );
      },
      isCurrentHandoffTarget: (actorId, spotId) => {
        const currentSpotId = this.actorManager?.getState(actorId)?.spotId;
        return currentSpotId !== undefined && String(currentSpotId) === spotId;
      },
      isCurrentActorRef: (actorId, actorRef) => {
        const current = this.actorManager?.getState(actorId)?.nativeActorRef;
        return (
          current !== undefined &&
          current.generation === actorRef.objectGeneration &&
          routingIdsEqual(current.nodeRid, actorRef.nodeRid)
        );
      },
      currentOwnerFence: (actorId) => {
        const state = this.actorManager?.getState(actorId);
        const current = state?.nativeActorRef;
        const owner = this.locationOwner.currentRuntime?.currentOwnerToken;
        const node =
          state?.meshName === undefined
            ? undefined
            : this.spotNodeRuntime?.meshNode(state.meshName)?.status();
        if (
          state === undefined ||
          current === undefined ||
          owner === undefined ||
          node === undefined ||
          state.locationGeneration === undefined ||
          state.ownerLeaseGeneration === undefined ||
          !routingIdsEqual(node.routingId, current.nodeRid)
        ) {
          return undefined;
        }
        return ownerFence({
          ownerId: owner.ownerId,
          ownerLeaseGeneration: state.ownerLeaseGeneration,
          nodeRid: String(current.nodeRid),
          nodeRidHex: encodeRoutingIdStorageHex(current.nodeRid),
          nodeGeneration: node.lifecycleGeneration,
          authorityOwnerGeneration: state.locationGeneration
        });
      }
    });
    this.actorTransferRegistry = new ZLinkActorTransferRegistry(
      options.registration.spotNodes,
      options.providerResolver,
      options.registration.messageSerializers
    );
    this.boundSessionFactory = new DefaultZLinkBoundSessionFactory(this.streamBindingRuntime);
    this.boundSessionRelay = new ZLinkBoundSessionRelay({
      requestTimeoutMs: options.registration.requestTimeoutMs,
      sessionRelocationSealTimeoutMs:
        options.registration.locations.options.sessionRelocationSealTimeoutMs ??
        zlinkDefaultLocationOptions.sessionRelocationSealTimeoutMs,
      routeTransport: this.routeTransport,
      streamBindingRuntime: () => this.streamBindingRuntime,
      meshRouters: this.meshRouters,
      actorManager: () => this.actorManager,
      spotManager: () => this.spotManager,
      spotNodeRuntime: () => this.spotNodeRuntime,
      detachedTaskRunner: this.detachedTaskRunner(),
      actorSessionNode: (actorId) => {
        const state = this.actorManager?.getState(actorId);
        const meshName =
          state?.meshName ??
          (state?.actorType === undefined
            ? undefined
            : this.meshRouters.actorMeshName(state.actorType));
        const runtime = this.spotNodeRuntime;
        const node =
          meshName === undefined || typeof runtime?.meshNode !== 'function'
            ? runtime?.primaryMeshNode
            : runtime.meshNode(meshName);
        return node === undefined
          ? undefined
          : meshActorSessionNodeAdapter(
              node,
              meshName === undefined || typeof runtime?.meshCompletionTable !== 'function'
                ? runtime?.primaryMeshCompletions
                : runtime.meshCompletionTable(meshName)
            );
      },
      actorLocationResolver: () => this.createActorLocationResolver(),
      authorityStore: () => this.locationOwner.currentStores?.locationStore,
      flowCreationEnabled: () => this.flowCreationEnabled(),
      destroyedActorRefs: this.destroyedActorRefs,
      errorSink: () => this.errorSink ?? this.preStartErrorSink,
      boundSessionFactory: (actorId) => {
        const factory = this.createActorManagerOptions().boundSessionFactory;
        if (factory === undefined) {
          throw new Error('Bound session factory is not configured.');
        }
        return factory(actorId) as DefaultZLinkBoundSession;
      }
    });
    this.entryActorRuntime = new ZLinkEntryActorRuntimeService({
      actorManager: () => this.actorManager,
      spotManager: () => this.spotManager,
      spotNodeRuntime: () => this.spotNodeRuntime,
      streamBindingRuntime: this.streamBindingRuntime,
      boundSessionRelay: this.boundSessionRelay,
      reportPostCommitError: (error) =>
        (this.errorSink ?? this.preStartErrorSink).reportRuntimeTaskException(
          'entry actor commit',
          error
        ),
      shutdownSignal: () => this.executionState?.abortController.signal
    });
    this.actorTransferRuntime = new ZLinkActorTransferRuntime({
      routeTransport: this.routeTransport,
      messageSerializers: options.registration.messageSerializers,
      spotManager: () => this.spotManager,
      actorManager: () => this.actorManager,
      primaryMeshNode: () => this.requirePrimaryMeshNode(),
      notifyEntrySpotActorLeft: (actor, signal) =>
        this.spotNodeRuntime?.notifyPrimaryEntrySpotActorLeft(actor, signal) ?? Promise.resolve(),
      restoreEntrySpotActorJoined: (actor, signal) =>
        this.spotNodeRuntime?.notifyPrimaryEntrySpotActorJoined(actor, signal) ?? Promise.resolve(),
      locationLifecycle: () => this.locationOwner.currentLifecycle,
      spotRouteResolver: () => this.createLocationSpotRouteResolver(),
      actorHandoff: this.actorHandoff,
      actorTransferRegistry: this.actorTransferRegistry,
      authorityStore: () => this.locationOwner.currentStores?.locationStore,
      currentOwner: () => this.locationOwner.currentRuntime?.currentOwnerToken,
      liveDescriptors: (meshName, signal) => {
        const runtime = this.locationOwner.currentRuntime;
        if (runtime === undefined) {
          throw new Error('Session owner liveness is unavailable without a Location runtime.');
        }
        return runtime.listLiveMeshNodes(meshName, signal);
      },
      sessionRelocationWire: () => this.serviceRelocation,
      clearRemoteActorPacketTarget: (actorId) =>
        this.boundSessionRelay.clearRemoteActorPacketTarget(actorId),
      prepareApplicationJob: async (preparationSignal, origin) => {
        const runtimeSignal = this.executionState?.abortController.signal;
        const signal =
          runtimeSignal === undefined
            ? preparationSignal
            : AbortSignal.any([runtimeSignal, preparationSignal]);
        const permit = await this.applicationJobQueue.acquire(signal, origin);
        let state: 'ready' | 'running' | 'closed' = 'ready';
        let closedReason: unknown;
        let abort: (() => void) | undefined;
        const cancel = (reason?: unknown): void => {
          if (state !== 'ready') return;
          state = 'closed';
          closedReason = reason;
          if (abort !== undefined) signal.removeEventListener('abort', abort);
          permit.releaseAfterInternalProcessing();
        };
        try {
          if (signal.aborted) {
            cancel(signal.reason);
            throw signal.reason;
          }
          permit.markApplicationQueued();
          abort = () => cancel(signal.reason);
          signal.addEventListener('abort', abort, { once: true });
        } catch (error) {
          cancel(error);
          throw error;
        }
        return {
          run: async <T>(operation: () => Promise<T>): Promise<T> => {
            if (state !== 'ready') {
              throw closedReason ?? new Error('Prepared application job is no longer runnable.');
            }
            state = 'running';
            signal.removeEventListener('abort', abort);
            try {
              return await runWithApplicationJobPermit(permit, operation);
            } finally {
              state = 'closed';
            }
          },
          cancel
        };
      },
      reportPostCommitError: (error) =>
        (this.errorSink ?? this.preStartErrorSink).reportRuntimeTaskException(
          'source actor departure',
          error
        ),
      onSourceDepartureCompleted: (actorId) =>
        this.publishActorHandoffEvent({ marker: 'source_cleanup', actorId }),
      shutdownSignal: () => this.executionState?.abortController.signal,
      metrics: this.metrics
    });
    this.serviceRelocation = new ZLinkHostServiceRelocationRuntime({
      registration: options.registration,
      targetAdmissionSealed: () => this.shutdownOperationStartedAt !== undefined,
      providerResolver: options.providerResolver,
      locationStore: () => this.locationOwner.currentStores?.locationStore,
      currentOwner: () => this.locationOwner.currentRuntime?.currentOwnerToken,
      liveDescriptors: (meshName, signal) =>
        this.locationOwner.currentRuntime?.listLiveMeshNodes(meshName, signal) ??
        Promise.resolve([]),
      localDescriptor: (meshName) => this.spotNodeRuntime?.meshNodeDescriptor(meshName),
      meshNode: (meshName) => this.spotNodeRuntime?.meshNode(meshName),
      completions: (meshName) => this.spotNodeRuntime?.meshCompletionTable(meshName),
      spotManager: () => this.spotManager,
      spotNodeRuntime: () => this.spotNodeRuntime,
      actorManager: () => this.actorManager,
      actorTransfer: this.actorTransferRuntime,
      boundSessionRelocation: {
        receiveSeal: (value, signal) =>
          this.boundSessionRelay.boundSessions.receiveServiceWireSessionRelocationSeal(
            value,
            signal
          ),
        receiveRoute: (value) =>
          this.boundSessionRelay.boundSessions.receiveServiceWireSessionRelocationRoute(value),
        clear: () => this.boundSessionRelay.boundSessions.clearServiceWireSessionRelocations()
      },
      trackInstanceSpot: (input) => this.locationOwner.currentLifecycle?.trackInstanceSpot(input),
      invalidateActorRoute: (actorId) =>
        this.actorClientLocationResolver?.invalidateActorRoute(actorId),
      reconcileStatefulAuthorityRoutes: (signal) =>
        this.statefulAuthorityRoutes?.reconcile(signal) ?? Promise.resolve(),
      runtimeEventPublisher: this.runtimeEventPublisher,
      metrics: this.metrics,
      activationAdmission: this.activationAdmission
    });
    this.actorTransferAuthorityRuntime = new ZLinkActorTransferAuthorityRuntime({
      store: () => this.locationOwner.actorTransferStore() as never,
      recoveryOwnerId: () => this.locationOwner.currentRuntime?.ownerId,
      recoveryLeaseTtlMs: {
        ...zlinkDefaultLocationOptions,
        ...options.registration.locations.options
      }.ownerLeaseTtlMs
    });
    this.routeMeshCoordinator = new ZLinkRouteMeshRuntimeCoordinator({
      meshNames: [...options.registration.spotNodes.keys()],
      meshOptions: options.registration.spotNodes,
      meshNode: (meshName) => this.spotNodeRuntime?.meshNode(meshName),
      meshNodeDescriptor: (meshName) => this.spotNodeRuntime?.meshNodeDescriptor(meshName),
      localPlacementCounts: (meshName) => {
        const spotManager = this.spotManager;
        const actorManager = this.actorManager;
        if (spotManager === undefined || actorManager === undefined) return undefined;
        return {
          activeActorCount: actorManager.activeActorCount(meshName),
          activeSpotCount: spotManager.activeSpotCount(meshName)
        };
      },
      isLocationStoreHealthy: () => {
        if (!hasConfiguredLocationStore(options.registration)) {
          return true;
        }
        const runtime = this.locationOwner.currentRuntime;
        return runtime !== undefined && runtime.ownerLeaseUsable && runtime.lastError === undefined;
      },
      hostState: () => this.runtimeState,
      admission: this.admission,
      activationAdmission: this.activationAdmission,
      publishRetiring: (meshName, signal) => this.publishMeshRetiring(meshName, signal),
      rollbackRetiring: (meshName, signal) => this.publishMeshServing(meshName, signal),
      publishDraining: (meshName, signal) => this.publishMeshDraining(meshName, signal),
      publishHostDraining: (signal) => this.publishHostDraining(signal),
      drainResources: (meshName, signal, stopStartingSignal) =>
        this.performMeshDrain(meshName, signal, stopStartingSignal),
      shutdownResources: (meshName, signal) => this.performMeshShutdown(meshName, signal),
      cleanupHostResources: (signal) => this.cleanupOwnerForDrain(signal),
      forceStopResources: (meshName) => this.forceStopMesh(meshName)
    });
    this.metricRegistrations.push(
      this.metrics.registerHostState(() => runtimeStateMetricName(this.runtimeState)),
      this.metrics.registerHostCapacity(() =>
        this.capacityStatus.snapshot(this.context?.getCoreHwmBudgetSnapshot())
      ),
      this.metrics.registerApplicationJobQueuePressure(() => this.applicationJobQueue.snapshot()),
      this.metrics.registerMeshSnapshots(() => this.runtimeMetricMeshSnapshots())
    );
    this.routeMeshRuntime = this.routeMeshCoordinator;
  }

  get isStarted(): boolean {
    return this.executionState !== undefined && !this.executionState.abortController.signal.aborted;
  }

  get status(): ZLinkFrameworkRuntimeStatus {
    return {
      state: this.runtimeState,
      isReady: runtimeStateIsReady(this.runtimeState),
      acceptingWork: runtimeAcceptsWork(this.runtimeState, this.admission.acceptsNewWork),
      capacity: this.capacityStatus.snapshot(this.context?.getCoreHwmBudgetSnapshot()),
      safeToShutdown: this.serviceRelocation.isSafeToShutdown(),
      deadline: this.runtimeDeadline,
      relocationResult: this.runtimeRelocationResult,
      terminationResult: this.runtimeTerminationResult,
      sequence: this.runtimeSequence,
      observedAt: new Date()
    };
  }

  // Bound listener records of the current runtime generation.
  private listenerRecords = new ZLinkListenerRecords();

  getListenerStatus(kind: ZLinkListenerKind, name: string): ZLinkListenerStatus {
    const endpoint = this.listenerRecords.endpoint(kind, name);
    if (endpoint === undefined || endpoint.length === 0) {
      throw new ZLinkConfigurationException(
        `Listener '${kind}:${name}' is not configured or has not bound.`
      );
    }
    return { kind, name, endpoint, observedAt: new Date() };
  }

  observe(signal?: AbortSignal): AsyncIterable<ZLinkObservedStatus<ZLinkFrameworkRuntimeStatus>> {
    const queue = new RuntimeEventQueue<ZLinkFrameworkRuntimeStatus>(undefined, signal);
    this.runtimeObservers.add(queue);
    queue.onClose(() => this.runtimeObservers.delete(queue));
    this.publishRuntimeStatus(queue, this.status);
    return queue;
  }

  resetCapacityMetrics(): void {
    const context = this.context;
    if (context === undefined) {
      throw new Error('Capacity metrics reset requires a started Framework runtime.');
    }
    this.capacityStatus.reset(() => context.resetCoreHwmBudgetMetrics());
  }

  relocate(options: ZLinkFrameworkRelocationOptions): Promise<ZLinkFrameworkRelocationResult> {
    const effectiveTargetApplicationVersion = validateRelocationOptions(
      options,
      this.options.registration.applicationVersion
    );
    const deadlineMs = options.deadlineMs ?? DEFAULT_HOST_CONTROL_TIMEOUT_MS;
    if (!Number.isFinite(deadlineMs) || deadlineMs <= 0) {
      return Promise.reject(new RangeError('Relocation deadlineMs must be greater than zero.'));
    }
    if (this.runtimeRelocationResult?.outcome === ZLinkFrameworkRelocationOutcome.Relocated) {
      return Promise.resolve(this.runtimeRelocationResult);
    }
    if (this.shutdownOperation !== undefined) {
      return Promise.resolve({
        mode: options.mode,
        effectiveTargetApplicationVersion,
        outcome: ZLinkFrameworkRelocationOutcome.Blocked,
        reason: ZLinkFrameworkRelocationReason.ShutdownRequested
      });
    }
    const operationKey = `${options.mode}:${effectiveTargetApplicationVersion}`;
    if (this.relocationOperation !== undefined) {
      if (this.relocationOperationKey === operationKey) {
        return waitForRuntimeOperation(this.relocationOperation, options.signal);
      }
      return Promise.resolve({
        mode: options.mode,
        effectiveTargetApplicationVersion,
        outcome: ZLinkFrameworkRelocationOutcome.Blocked,
        reason: ZLinkFrameworkRelocationReason.OperationInProgress
      });
    }
    if (this.runtimeState !== ZLinkFrameworkRuntimeState.Serving) {
      return Promise.resolve({
        mode: options.mode,
        effectiveTargetApplicationVersion,
        outcome: ZLinkFrameworkRelocationOutcome.Blocked,
        reason:
          this.runtimeState === ZLinkFrameworkRuntimeState.Relocating
            ? ZLinkFrameworkRelocationReason.OperationInProgress
            : ZLinkFrameworkRelocationReason.RuntimeNotReady
      });
    }
    if (!this.admission.acceptsNewWork) {
      return Promise.resolve({
        mode: options.mode,
        effectiveTargetApplicationVersion,
        outcome: ZLinkFrameworkRelocationOutcome.Blocked,
        reason: ZLinkFrameworkRelocationReason.StoreUnavailable
      });
    }
    if (hasUnsupportedManualTopology(this.options.registration)) {
      return Promise.resolve({
        mode: options.mode,
        effectiveTargetApplicationVersion,
        outcome: ZLinkFrameworkRelocationOutcome.Blocked,
        reason: ZLinkFrameworkRelocationReason.ManualTopologyUnsupported
      });
    }
    this.runtimeDeadline = new Date(Date.now() + deadlineMs);
    this.relocationTargetApplicationVersion = effectiveTargetApplicationVersion;
    this.relocationOperationKey = operationKey;
    this.relocationOperationStartedAt = performance.now();
    this.relocationStopStarting = new AbortController();
    this.relocationOperation = this.runRelocation(
      options.mode,
      effectiveTargetApplicationVersion,
      deadlineMs,
      this.relocationStopStarting.signal
    );
    return waitForRuntimeOperation(this.relocationOperation, options.signal);
  }

  shutdown(options?: ZLinkFrameworkLifecycleOptions): Promise<ZLinkFrameworkTerminationResult> {
    const deadlineMs = options?.deadlineMs ?? DEFAULT_HOST_CONTROL_TIMEOUT_MS;
    if (!Number.isFinite(deadlineMs) || deadlineMs <= 0) {
      return Promise.reject(new RangeError('Shutdown deadlineMs must be greater than zero.'));
    }
    if (this.shutdownOperation === undefined) {
      this.runtimeDeadline = new Date(Date.now() + deadlineMs);
      this.relocationStopStarting?.abort(new Error('Shutdown requested.'));
      this.shutdownOperationStartedAt = performance.now();
      this.shutdownOperation = this.runShutdown(deadlineMs);
    }
    return waitForRuntimeOperation(this.shutdownOperation, options?.signal);
  }

  private async runRelocation(
    mode: ZLinkFrameworkRelocationMode,
    effectiveTargetApplicationVersion: bigint,
    deadlineMs: number,
    stopStartingSignal: AbortSignal
  ): Promise<ZLinkFrameworkRelocationResult> {
    if (!this.isStarted) {
      return this.completeRelocation(
        blockedRelocation(
          mode,
          effectiveTargetApplicationVersion,
          ZLinkFrameworkRelocationReason.RuntimeNotReady
        )
      );
    }
    const deadlineAtMs = performance.now() + deadlineMs;
    const remainingDeadlineMs = () => Math.max(1, deadlineAtMs - performance.now());
    try {
      // Runtime weight changes publish a newer descriptor asynchronously. A
      // maintenance operation must observe that publication before it writes
      // Retiring/Draining, otherwise two descriptor revisions can race and
      // make a valid drain look like a Store failure.
      await this.spotNodeRuntime?.waitForRuntimeWeightPublication();
      const blocker = await this.preflightAutomaticPeerReadiness(
        deadlineAtMs,
        effectiveTargetApplicationVersion
      );
      if (blocker !== undefined) {
        return this.resetBlockedRelocation(
          blockedRelocation(mode, effectiveTargetApplicationVersion, blocker)
        );
      }
      const descriptorPreparation =
        await this.routeMeshCoordinator.prepareHostRetire(remainingDeadlineMs());
      if (descriptorPreparation !== 'prepared') {
        return this.resetBlockedRelocation(
          blockedRelocation(
            mode,
            effectiveTargetApplicationVersion,
            descriptorPreparation === 'deadline_exceeded'
              ? ZLinkFrameworkRelocationReason.DeadlineExceeded
              : ZLinkFrameworkRelocationReason.StoreUnavailable
          )
        );
      }
      this.setRuntimeState(ZLinkFrameworkRuntimeState.Relocating);
      const drained = await this.routeMeshCoordinator.relocateHost(
        remainingDeadlineMs(),
        stopStartingSignal
      );
      if (drained.kind === 'forceStopped') {
        // Settled authority on both sides or an expired source owner lease
        // ends the host in Error (spec 30 §13).
        if (drained.error instanceof ServiceRelocationAuthorityError) {
          return this.completeRelocation(
            blockedRelocation(
              mode,
              effectiveTargetApplicationVersion,
              ZLinkFrameworkRelocationReason.RelocationFailed
            )
          );
        }
        return this.resetBlockedRelocation(
          blockedRelocation(
            mode,
            effectiveTargetApplicationVersion,
            stopStartingSignal.aborted
              ? ZLinkFrameworkRelocationReason.ShutdownRequested
              : drained.error instanceof ZLinkRelocationStateIncompatibleError
                ? ZLinkFrameworkRelocationReason.StateIncompatible
                : relocationReason(drained.reason)
          )
        );
      }
      await this.publishHostRelocated();
      return this.completeRelocation({
        mode,
        effectiveTargetApplicationVersion,
        outcome: ZLinkFrameworkRelocationOutcome.Relocated,
        reason: ZLinkFrameworkRelocationReason.None
      });
    } catch (error) {
      const result = blockedRelocation(
        mode,
        effectiveTargetApplicationVersion,
        stopStartingSignal.aborted
          ? ZLinkFrameworkRelocationReason.ShutdownRequested
          : isDeadlineExceededError(error)
            ? ZLinkFrameworkRelocationReason.DeadlineExceeded
            : error instanceof ZLinkRelocationStateIncompatibleError
              ? ZLinkFrameworkRelocationReason.StateIncompatible
              : ZLinkFrameworkRelocationReason.RelocationFailed
      );
      return error instanceof ZLinkRetiringRollbackError
        ? this.completeRelocation(result)
        : this.resetBlockedRelocation(result);
    }
  }

  private async runShutdown(deadlineMs: number): Promise<ZLinkFrameworkTerminationResult> {
    return await this.runWithShutdownDeadline(deadlineMs, async () => {
      const deadlineAtMs = performance.now() + deadlineMs;
      try {
        this.admission.close();
        if (this.runtimeState === ZLinkFrameworkRuntimeState.Preparing) {
          this.executionState?.abortController.abort();
        }
        this.setRuntimeState(ZLinkFrameworkRuntimeState.Draining);
        if (
          this.relocationOperation !== undefined &&
          this.relocationStopStarting?.signal.aborted === true
        ) {
          await this.relocationOperation;
        }
        // Seal application admission before the public status reports that the
        // host no longer accepts work. This prevents status polling from racing
        // with the coordinator's synchronous seal step.
        const unscopedStreamDrain = this.streamRuntime?.notifyUnscopedServerDrain();
        const shutdown = this.routeMeshCoordinator.shutdownHost(
          this.runtimeDeadline!,
          this.shutdownDeadline!.signal
        );
        const drain = await shutdown;
        await unscopedStreamDrain;
        await this.stop();
        if (drain.kind === 'forceStopped') {
          return this.completeTermination({
            outcome: ZLinkFrameworkTerminationOutcome.ForceStopped,
            reason:
              drain.reason === 'deadline_exceeded'
                ? ZLinkFrameworkTerminationReason.DeadlineExceeded
                : ZLinkFrameworkTerminationReason.TeardownFailed
          });
        }
        if (performance.now() >= deadlineAtMs) {
          throw createDeadlineExceededError('Shutdown resource cleanup exceeded its deadline.');
        }
        return this.completeTermination({
          outcome: ZLinkFrameworkTerminationOutcome.Stopped,
          reason: ZLinkFrameworkTerminationReason.None
        });
      } catch (error) {
        await this.stop().catch(() => undefined);
        return this.completeTermination({
          outcome: ZLinkFrameworkTerminationOutcome.ForceStopped,
          reason: isDeadlineExceededError(error)
            ? ZLinkFrameworkTerminationReason.DeadlineExceeded
            : ZLinkFrameworkTerminationReason.TeardownFailed
        });
      }
    });
  }

  private async runWithShutdownDeadline<T>(
    deadlineMs: number,
    operation: () => Promise<T>
  ): Promise<T> {
    if (this.shutdownDeadline !== undefined) return await operation();
    const deadline = new AbortController();
    this.shutdownDeadline = deadline;
    const ownsRuntimeDeadline = this.runtimeDeadline === undefined;
    this.runtimeDeadline ??= new Date(Date.now() + deadlineMs);
    const timer = setTimeout(
      () => deadline.abort(createDeadlineExceededError('Shutdown deadline exceeded.')),
      Math.max(0, this.runtimeDeadline.getTime() - Date.now())
    );
    try {
      return await operation();
    } finally {
      clearTimeout(timer);
      this.shutdownDeadline = undefined;
      if (ownsRuntimeDeadline) this.runtimeDeadline = undefined;
    }
  }

  private resetBlockedRelocation(
    result: ZLinkFrameworkRelocationResult
  ): ZLinkFrameworkRelocationResult {
    this.recordRelocationResult(result);
    if (this.runtimeState === ZLinkFrameworkRuntimeState.Relocating) {
      this.setRuntimeState(ZLinkFrameworkRuntimeState.Serving);
    }
    this.runtimeDeadline = undefined;
    this.relocationTargetApplicationVersion = undefined;
    this.relocationStopStarting = undefined;
    this.relocationOperation = undefined;
    this.relocationOperationKey = undefined;
    this.relocationOperationStartedAt = undefined;
    return result;
  }

  private completeRelocation(
    result: ZLinkFrameworkRelocationResult
  ): ZLinkFrameworkRelocationResult {
    this.runtimeRelocationResult = result;
    this.runtimeDeadline = undefined;
    this.recordRelocationResult(result);
    this.setRuntimeState(
      result.outcome === ZLinkFrameworkRelocationOutcome.Relocated
        ? ZLinkFrameworkRuntimeState.Relocated
        : ZLinkFrameworkRuntimeState.Error
    );
    return result;
  }

  private recordRelocationResult(result: ZLinkFrameworkRelocationResult): void {
    if (this.relocationOperationStartedAt !== undefined) {
      this.metrics.duration(
        METRIC_NAMES.HostRelocationDuration,
        Math.max(0, performance.now() - this.relocationOperationStartedAt) /
          MILLISECONDS_PER_SECOND,
        {
          mode: relocationModeMetricName(result.mode),
          outcome:
            result.outcome === ZLinkFrameworkRelocationOutcome.Relocated ? 'relocated' : 'blocked'
        }
      );
    }
    if (result.outcome === ZLinkFrameworkRelocationOutcome.Blocked) {
      this.metrics.count(METRIC_NAMES.HostRelocationBlocked, 1, {
        mode: relocationModeMetricName(result.mode),
        reason: relocationReasonMetricName(result.reason)
      });
    }
    void this.runtimeEventPublisher.publish({
      sourceName: 'zlink.runtime.host',
      timestamp: new Date(),
      identifier: 'zlink.runtime.host.relocation_changed',
      mode: result.mode,
      effectiveTargetApplicationVersion: result.effectiveTargetApplicationVersion,
      outcome: result.outcome,
      reason: result.reason
    });
  }

  private completeTermination(
    result: ZLinkFrameworkTerminationResult
  ): ZLinkFrameworkTerminationResult {
    this.runtimeTerminationResult = result;
    this.runtimeDeadline = undefined;
    if (this.shutdownOperationStartedAt !== undefined) {
      this.metrics.duration(
        METRIC_NAMES.HostShutdownDuration,
        Math.max(0, performance.now() - this.shutdownOperationStartedAt) / MILLISECONDS_PER_SECOND,
        {
          outcome:
            result.outcome === ZLinkFrameworkTerminationOutcome.Stopped
              ? 'stopped'
              : 'force_stopped'
        }
      );
    }
    if (result.outcome === ZLinkFrameworkTerminationOutcome.ForceStopped) {
      this.metrics.count(METRIC_NAMES.HostShutdownForced, 1, {
        reason: terminationReasonMetricName(result.reason)
      });
    }
    void this.runtimeEventPublisher.publish({
      sourceName: 'zlink.runtime.host',
      timestamp: new Date(),
      identifier: 'zlink.runtime.host.termination_changed',
      outcome: result.outcome,
      reason: result.reason
    });
    this.setRuntimeState(ZLinkFrameworkRuntimeState.Stopped);
    return result;
  }

  private setRuntimeState(state: ZLinkFrameworkRuntimeState): void {
    if (this.runtimeState === state) return;
    this.runtimeState = state;
    this.runtimeSequence += 1n;
    this.notifyTopologyHostStateChanged();
    const status = this.status;
    for (const observer of this.runtimeObservers) {
      this.publishRuntimeStatus(observer, status);
    }
  }

  private publishRuntimeStatus(
    observer: RuntimeEventQueue<ZLinkFrameworkRuntimeStatus>,
    status: ZLinkFrameworkRuntimeStatus
  ): void {
    if (runtimeObservationIsTerminal(status.state)) {
      observer.complete(status, this.runtimeObservationSource);
    } else {
      observer.push(status, this.runtimeObservationSource);
    }
  }

  private notifyTopologyHostStateChanged(): void {
    const callbacks = [
      () => this.routeMeshCoordinator.hostStateChanged(),
      () => (this.clientServerRuntime as ZLinkClientServerRuntimeProjection).hostStateChanged(),
      () => (this.fanoutRuntime as ZLinkFanoutRuntimeProjection).hostStateChanged()
    ];
    for (const callback of callbacks) {
      try {
        callback();
      } catch (error) {
        try {
          this.runtimeOrPreStartErrorSink.reportRuntimeTaskException(
            'topology lifecycle observation',
            error
          );
        } catch {
          // Monitoring must not change the host lifecycle result.
        }
      }
    }
  }

  private stopTopologyObservers(): void {
    const callbacks = [
      () => this.routeMeshCoordinator.stopObservers(),
      () => (this.clientServerRuntime as ZLinkClientServerRuntimeProjection).stopObservers(),
      () => (this.fanoutRuntime as ZLinkFanoutRuntimeProjection).stopObservers()
    ];
    for (const callback of callbacks) {
      try {
        callback();
      } catch (error) {
        try {
          this.runtimeOrPreStartErrorSink.reportRuntimeTaskException(
            'topology observer shutdown',
            error
          );
        } catch {
          // Monitoring must not change the host lifecycle result.
        }
      }
    }
  }

  private publishActorHandoffEvent(event: {
    readonly marker: string;
    readonly actorId: string;
    readonly index?: number;
    readonly requestSeq?: string;
    readonly flags?: number;
  }): void {
    void this.runtimeEventPublisher.publish({
      sourceName: 'zlink.framework.actor-handoff',
      timestamp: new Date(),
      ...event
    });
  }

  get locationRuntimeQuery(): ZLinkLocationRuntimeQuery | undefined {
    return this.ensureLocationRuntime();
  }

  /**
   * Runtime toggle (ZLinkMessageFlowControl): flip the shared live-mode cell so every
   * surface starts/stops tracing without a restart. Do not call this synchronous
   * bridge from a framework execution context such as a handler or callback; use
   * setMessageFlowModeAsync there.
   */
  setMessageFlowMode(mode: ZLinkMessageFlowLogMode): void {
    void this.setMessageFlowModeAsync(mode);
  }

  setMessageFlowModeAsync(mode: ZLinkMessageFlowLogMode): Promise<void> {
    this.messageFlowModeCell.mode = requireMessageFlowLogMode(mode);
    return Promise.resolve();
  }

  messageFlowMode(): ZLinkMessageFlowLogMode {
    return this.messageFlowModeCell.mode;
  }

  get context(): ZLinkBackendContext | undefined {
    return this.executionState?.context as ZLinkBackendContext | undefined;
  }

  get taskRunner(): ZLinkFrameworkExecutionState['taskRunner'] | undefined {
    return this.executionState?.taskRunner;
  }

  get errorSink(): ZLinkFrameworkExecutionState['errorSink'] | undefined {
    return this.executionState?.errorSink;
  }

  async start(): Promise<void> {
    await this.runLifecycle(() => this.startCore());
  }

  private async startCore(): Promise<void> {
    if (this.executionState !== undefined) {
      return;
    }

    this.setRuntimeState(ZLinkFrameworkRuntimeState.Preparing);
    this.lifecycleSink?.push('framework:start');
    const listenerRecords = new ZLinkListenerRecords();
    this.listenerRecords = listenerRecords;
    const channelAdapter = this.backendAdapterFactory.createChannelAdapter();
    const context = channelAdapter.createContext();
    const coreHwm = this.options.registration.coreHwm;
    if (coreHwm !== undefined) {
      context.configureCoreHwm(coreHwm);
    }
    let channelRuntime: ZLinkChannelRuntimeManager | undefined;
    let spotNodeRuntime: ZLinkSpotNodeRuntimeManager | undefined;
    let streamRuntime: ZLinkStreamRuntimeManager | undefined;
    let startedLocationRuntime: ZLinkLocationRuntime | undefined;
    let statefulAuthorityRoutes: ZLinkStatefulAuthorityRouteRuntime | undefined;
    try {
      this.executionState = new ZLinkFrameworkExecutionState(context);
      // Seed the shared live-mode cell from the configured mode (default errorsOnly).
      this.messageFlowModeCell.mode =
        this.options.registration.dispatch?.diagnostics.messageFlow ?? 'errors';
      const dispatchErrors = this.createDispatchErrorReporter(this.executionState.errorSink);
      channelRuntime = new ZLinkChannelRuntimeManager(
        this.options.registration,
        channelAdapter,
        context,
        this.options.providerResolver,
        { ...this.createChannelRuntimeOptions(), listenerRecords }
      );
      this.channelRuntime = channelRuntime;
      channelRuntime.prepareMeshDispatch(this.executionState.taskRunner);
      spotNodeRuntime = new ZLinkSpotNodeRuntimeManager({
        ...this.createSpotNodeRuntimeOptions(context, dispatchErrors),
        listenerRecords
      });
      await spotNodeRuntime.start();
      this.spotNodeRuntime = spotNodeRuntime;
      this.registerUserSpotHandlers?.(spotNodeRuntime);
      channelRuntime.bindRouteMeshRouters();
      // Start bound receivers before publishing Serving descriptors. A
      // discovered ClientServer endpoint must already be able to dispatch.
      this.executionState.listenerTasks.push(
        ...channelRuntime.start(this.executionState.taskRunner)
      );
      const locationRuntime = await this.locationOwner.startForRuntime(
        this.meshRouters.primaryMeshName(),
        spotNodeRuntime,
        channelRuntime,
        this.executionState.abortController.signal,
        () => this.shutdownDeadline!.signal
      );
      startedLocationRuntime = locationRuntime;
      if (locationRuntime !== undefined) {
        this.installOwnerLeaseRecoveryPublication(locationRuntime, spotNodeRuntime, channelRuntime);
      }
      const locationStore = this.locationOwner.currentStores?.locationStore;
      if (locationStore !== undefined && locationRuntime?.ownerLeaseUsable !== false) {
        await spotNodeRuntime.publishMeshNodeState(
          ZLinkFrameworkRuntimeState.Preparing,
          this.executionState.abortController.signal
        );
        if (this.requiresStatefulAuthorityRuntime()) {
          this.installLocationBackedAuthorities(locationStore, spotNodeRuntime);
          statefulAuthorityRoutes = this.createStatefulAuthorityRoutes(
            locationStore,
            spotNodeRuntime
          );
          await statefulAuthorityRoutes.start(this.executionState.abortController.signal);
          this.statefulAuthorityRoutes = statefulAuthorityRoutes;
        }
      }
      streamRuntime = new ZLinkStreamRuntimeManager({
        listenerRecords,
        registration: this.options.registration,
        backendAdapterFactory: this.backendAdapterFactory,
        context,
        bindingRuntime: this.streamBindingRuntime,
        providerResolver: this.options.providerResolver,
        dispatchErrors,
        metrics: this.metrics,
        applicationJobQueue: this.applicationJobQueue,
        acceptNewSession: (meshName) =>
          meshName === undefined ? this.admission.acceptsNewWork : this.admission.accepts(meshName),
        primaryMeshName: this.meshRouters.primaryMeshName(),
        nativeMeshNode: spotNodeRuntime.primaryMeshNode,
        meshCompletions: spotNodeRuntime.primaryMeshCompletions,
        nativeMeshNodeForName: (meshName) => spotNodeRuntime?.meshNode(meshName),
        meshCompletionsForName: (meshName) => spotNodeRuntime?.meshCompletionTable(meshName)
      });
      streamRuntime.start();
      this.streamRuntime = streamRuntime;
      if (locationRuntime === undefined || locationRuntime.ownerLeaseUsable) {
        await spotNodeRuntime.publishMeshNodeState(
          ZLinkFrameworkRuntimeState.Serving,
          this.executionState.abortController.signal
        );
      }
      this.routeMeshCoordinator.markServing();
      this.setRuntimeState(ZLinkFrameworkRuntimeState.Serving);
      this.lifecycleSink?.push('framework:started');
    } catch (error) {
      if (this.shutdownDeadline !== undefined && isAbortError(error)) throw error;
      await statefulAuthorityRoutes?.stop();
      try {
        await this.runWithShutdownDeadline(DEFAULT_HOST_CONTROL_TIMEOUT_MS, async () =>
          rollbackRuntimeStart({
            context,
            startedLocationRuntime: startedLocationRuntime ?? this.locationOwner.currentRuntime,
            shutdownSignal: this.shutdownDeadline!.signal,
            streamRuntime,
            spotNodeRuntime,
            channelRuntime,
            ownedStores: registeredRuntimeStores(this.options.registration)
          })
        );
      } catch (cleanupError) {
        this.runtimeOrPreStartErrorSink.reportRuntimeTaskException(
          'framework start rollback',
          cleanupError
        );
        throw new AggregateError([error, cleanupError], 'Framework start and rollback failed.');
      }
      this.executionState = undefined;
      this.channelRuntime = undefined;
      this.spotNodeRuntime = undefined;
      this.streamRuntime = undefined;
      this.statefulAuthorityRoutes = undefined;
      this.removeOwnerLeaseRecoveryPublication();
      this.locationOwner.clearForStop();
      this.cachedLocationSpotRouteResolver = undefined;
      throw error;
    }
  }

  async stop(): Promise<void> {
    await this.runWithShutdownDeadline(DEFAULT_HOST_CONTROL_TIMEOUT_MS, async () =>
      this.runLifecycle(() => this.stopCore())
    );
  }

  private runtimeMetricMeshSnapshots(): readonly ZLinkRuntimeMetricMeshSnapshot[] {
    const snapshots: ZLinkRuntimeMetricMeshSnapshot[] = [];
    for (const [meshName, options] of this.options.registration.spotNodes) {
      let status;
      try {
        status = this.routeMeshCoordinator.snapshot(meshName);
      } catch {
        continue;
      }
      const descriptor = this.spotNodeRuntime?.meshNodeDescriptor(meshName);
      const hasManual =
        (options.router?.manualConnections?.length ?? 0) > 0 ||
        (options.router?.manualPeerConnections?.length ?? 0) > 0 ||
        (options.pubSub?.manualConnections?.length ?? 0) > 0;
      const hasDiscovery = hasConfiguredLocationStore(this.options.registration);
      const source =
        hasManual && hasDiscovery ? 'manual_and_redis' : hasManual ? 'manual' : 'redis';
      const emptyCapacity = { active: 0, reserved: 0, limit: 0 };
      snapshots.push({
        meshName,
        source,
        configuredPeers: status.peers.length,
        connectedPeers: status.peers.filter((peer) => peer.state !== ZLinkPeerState.NotConnected)
          .length,
        readyPeers: status.readyPeerCount,
        channels: status.channels.map((channel) => ({
          channelName: channel.channelName,
          readyMembers: channel.readyTargetCount
        })),
        actorCapacity: descriptor?.populationCapacity.actors ?? emptyCapacity,
        spotCapacity: descriptor?.populationCapacity.spots ?? emptyCapacity,
        spotTypeCapacities: (descriptor?.populationCapacity.spotTypes ?? []).map((capacity) => ({
          spotKind: capacity.objectKind === 'instance_spot' ? 'instance' : 'user',
          stableType: capacity.stableType,
          active: capacity.active,
          reserved: capacity.reserved,
          limit: capacity.limit
        })),
        activation: this.activationAdmission.current(meshName),
        instanceSpots: []
      });
    }
    return snapshots;
  }

  private async stopCore(): Promise<void> {
    const state = this.executionState;
    if (state === undefined) {
      return;
    }
    state.abortController.abort();

    const channelRuntime = this.channelRuntime;
    const spotNodeRuntime = this.spotNodeRuntime;
    const streamRuntime = this.streamRuntime;
    const statefulAuthorityRoutes = this.statefulAuthorityRoutes;
    this.stopTopologyObservers();
    this.removeOwnerLeaseRecoveryPublication();
    const locationSnapshot = this.locationOwner.clearForStop();
    this.lifecycleSink?.push('framework:stop');
    await statefulAuthorityRoutes?.stop();
    await stopRuntimeParts({
      state,
      locationSnapshot,
      cleanupDeadline: this.runtimeDeadline,
      shutdownSignal: this.shutdownDeadline!.signal,
      streamRuntime,
      spotNodeRuntime,
      channelRuntime,
      serviceRelocation: this.serviceRelocation,
      ownedStores: registeredRuntimeStores(this.options.registration)
    });
    this.executionState = undefined;
    this.channelRuntime = undefined;
    this.spotNodeRuntime = undefined;
    this.streamRuntime = undefined;
    this.statefulAuthorityRoutes = undefined;
    this.cachedLocationSpotRouteResolver = undefined;
    this.listenerRecords.clear();
    this.lifecycleSink?.push('framework:stopped');
    if (this.shutdownOperation === undefined) {
      this.setRuntimeState(ZLinkFrameworkRuntimeState.Stopped);
    }
  }

  private installOwnerLeaseRecoveryPublication(
    runtime: ZLinkLocationRuntime,
    spotNodeRuntime: ZLinkSpotNodeRuntimeManager,
    channelRuntime: ZLinkChannelRuntimeManager
  ): void {
    const handler = () => {
      if (!this.ownerAdmissionOpen()) return;
      void this.resumeOwnerLeaseOwnedWork(runtime, spotNodeRuntime, channelRuntime).catch(
        (error) => {
          runtime.closeOwnerLeaseAdmission(error);
          this.runtimeOrPreStartErrorSink.reportRuntimeTaskException('owner lease recovery', error);
        }
      );
    };
    this.ownerLeaseRecoveryRuntime = runtime;
    this.ownerLeaseRecoveryHandler = handler;
    runtime.addOwnerLeaseRenewedHandler(handler);
  }

  private async resumeOwnerLeaseOwnedWork(
    runtime: ZLinkLocationRuntime,
    spotNodeRuntime: ZLinkSpotNodeRuntimeManager,
    channelRuntime: ZLinkChannelRuntimeManager
  ): Promise<void> {
    if (!this.ownerAdmissionOpen()) return;
    const signal = this.executionState?.abortController.signal;
    await spotNodeRuntime.publishMeshNodeState(ZLinkFrameworkRuntimeState.Preparing, signal);
    const store = this.locationOwner.currentStores?.locationStore;
    if (
      this.statefulAuthorityRoutes === undefined &&
      store !== undefined &&
      this.executionState !== undefined &&
      this.requiresStatefulAuthorityRuntime()
    ) {
      const routes = this.createStatefulAuthorityRoutes(store, spotNodeRuntime);
      await routes.start(signal);
      this.statefulAuthorityRoutes = routes;
    }
    if (runtime.ownerLeaseUsable === false) return;
    await spotNodeRuntime.publishMeshNodeState(this.runtimeState, signal);
    await spotNodeRuntime.startLocationAutoConnect(signal);
    await channelRuntime.startLocationAutoConnect(signal);
    await channelRuntime.reclaimLocationOwnerRows(signal);
    await this.locationOwner.currentLifecycle?.reclaimOwnerRows();
  }

  private createStatefulAuthorityRoutes(
    locationStore: import('../locations/domain-store-contract').ZLinkDomainLocationStore,
    spotNodeRuntime: ZLinkSpotNodeRuntimeManager
  ): ZLinkStatefulAuthorityRouteRuntime {
    return new ZLinkStatefulAuthorityRouteRuntime({
      store: locationStore,
      creationStore: locationStore,
      relocationStore: this.options.registration.locations.relocationStoreInstance,
      meshNodes: spotNodeRuntime.meshNodesByName,
      pollingIntervalMs:
        this.options.registration.locations.options.pollingIntervalMs ??
        zlinkDefaultLocationOptions.pollingIntervalMs,
      pageSize: ZLINK_PROVIDER_MAX_PAGE_SIZE,
      reportError: (error) =>
        this.runtimeOrPreStartErrorSink.reportRuntimeTaskException(
          'stateful authority route reconciliation',
          error
        ),
      onSpotRouteChanged: (spotId) => this.cachedLocationSpotRouteResolver?.invalidate?.(spotId)
    });
  }

  private requiresStatefulAuthorityRuntime(): boolean {
    for (const options of this.options.registration.spotNodes.values()) {
      if (
        Object.keys(options.spotFactoryRegistrations ?? {}).length > 0 ||
        Object.keys(options.instanceSpotFactoryRegistrations ?? {}).length > 0 ||
        Object.keys(options.actorFactoryRegistrations ?? {}).length > 0
      ) {
        return true;
      }
    }
    return false;
  }

  private installLocationBackedAuthorities(
    locationStore: import('../locations/domain-store-contract').ZLinkDomainLocationStore,
    spotNodeRuntime: ZLinkSpotNodeRuntimeManager
  ): void {
    for (const [meshName, node] of spotNodeRuntime.meshNodesByName) {
      const activationNode = node as typeof node & {
        registerAsyncInstanceActivationAuthority?: (
          authority: ServiceAsyncInstanceActivationAuthority
        ) => void;
        registerInstanceIntent: (
          instanceType: string,
          route: import('../foundation/service-stateful-wire-codec').ServiceInstanceRouteFence,
          expectedCurrentRoute?:
            import('../foundation/service-stateful-wire-codec').ServiceInstanceRouteFence | null
        ) => void;
        registerInstanceApplicationLifecycle?: (
          lifecycle: ServiceInstanceApplicationLifecycle
        ) => void;
      };
      const spotManager = this.spotManager;
      if (spotManager !== undefined) {
        this.registerInstanceApplicationLifecycle(meshName, activationNode, spotManager);
      }
      activationNode.registerAsyncInstanceActivationAuthority?.(
        new ZLinkInstanceActivationAuthority({
          store: locationStore,
          relocationStore: this.options.registration.locations.relocationStoreInstance,
          meshName,
          owner: () => this.locationOwner.currentRuntime?.currentOwnerToken,
          metrics: this.metrics,
          onReady: (target, route) => {
            activationNode.registerInstanceIntent(target.stableType, route);
            this.locationOwner.currentLifecycle?.trackInstanceSpot({
              meshName,
              spotId: target.targetSpotId,
              stableType: target.stableType,
              nodeRid: route.targetNodeRid,
              nodeGeneration: route.targetNodeGeneration,
              objectGeneration: route.objectGeneration,
              authorityOwnerGeneration: route.authorityOwnerGeneration,
              ownerId: route.ownerId,
              ownerLeaseGeneration: route.leaseGeneration,
              storeVersion: route.storeVersion,
              deactivate: async () => {
                await spotManager?.close(meshName, target.targetSpotId as never);
              }
            });
          }
        })
      );
    }
  }

  private removeOwnerLeaseRecoveryPublication(): void {
    if (
      this.ownerLeaseRecoveryRuntime !== undefined &&
      this.ownerLeaseRecoveryHandler !== undefined
    ) {
      this.ownerLeaseRecoveryRuntime.removeOwnerLeaseRenewedHandler(this.ownerLeaseRecoveryHandler);
    }
    this.ownerLeaseRecoveryRuntime = undefined;
    this.ownerLeaseRecoveryHandler = undefined;
  }

  private ownerAdmissionOpen(): boolean {
    const runtime = this.locationOwner.currentRuntime;
    if (runtime !== undefined) return runtime.ownerLeaseUsable;
    return this.locationOwner.currentStores?.locationStore === undefined;
  }

  async onApplicationBootstrap(): Promise<void> {
    await this.start();
  }

  async onApplicationShutdown(): Promise<void> {
    await this.shutdown();
  }

  private async performMeshDrain(
    meshName: string,
    signal: AbortSignal,
    stopStartingSignal?: AbortSignal
  ): Promise<void> {
    await this.serviceRelocation.relocateMesh(
      meshName,
      this.relocationTargetApplicationVersion,
      signal,
      stopStartingSignal
    );
  }

  private async performMeshShutdown(meshName: string, signal: AbortSignal): Promise<void> {
    await this.spotManager?.drainForShutdown(meshName, signal, this.runtimeDeadline);
    await awaitWithDrainSignal(this.streamRuntime?.notifyServerDrain(meshName), signal);
  }

  private async publishMeshRetiring(meshName: string, signal?: AbortSignal): Promise<void> {
    await this.spotNodeRuntime?.publishMeshNodeState(
      ZLinkFrameworkRuntimeState.Relocating,
      signal,
      meshName
    );
  }

  private async publishMeshServing(meshName: string, signal?: AbortSignal): Promise<void> {
    await this.spotNodeRuntime?.reconcileAndPublishMeshNodeState(
      ZLinkFrameworkRuntimeState.Serving,
      meshName,
      signal
    );
    // A failed relocation rolls the local object barriers back before the
    // descriptor is published as Serving. Reconcile the durable authority
    // routes before returning so the next local request cannot observe the
    // brief window where the Location row is ready but the route cache still
    // reflects the failed capture.
    await this.statefulAuthorityRoutes?.reconcile(signal);
  }

  private async publishMeshDraining(meshName: string, signal?: AbortSignal): Promise<void> {
    const node = this.spotNodeRuntime?.meshNode(meshName);
    if (node !== undefined) {
      await node.publishDraining?.();
    }
    await this.spotNodeRuntime?.publishMeshNodeState(
      ZLinkFrameworkRuntimeState.Draining,
      signal,
      meshName
    );
  }

  private async preflightAutomaticPeerReadiness(
    deadlineAt: number,
    targetApplicationVersion: bigint
  ): Promise<ZLinkFrameworkRelocationReason | undefined> {
    // A host with no local stateful workload has nothing to move. The public
    // Relocate contract completes that case without requiring a replacement
    // target; the host drain still publishes its retiring state below.
    if (!this.hasLocalStatefulRelocationWork()) return undefined;
    if (this.options.registration.spotNodes.size === 0) return undefined;
    const location = this.locationOwner.currentRuntime;
    if (location === undefined) return ZLinkFrameworkRelocationReason.StoreUnavailable;

    let storeUnavailable = false;
    while (performance.now() < deadlineAt) {
      try {
        if (await this.hasExactAutomaticPeerReadiness(location, targetApplicationVersion))
          return undefined;
        storeUnavailable = false;
      } catch {
        storeUnavailable = true;
      }
      await new Promise((resolve) =>
        setTimeout(
          resolve,
          Math.min(
            this.options.registration.locations.options.pollingIntervalMs ??
              zlinkDefaultLocationOptions.pollingIntervalMs,
            Math.max(1, deadlineAt - performance.now())
          )
        )
      );
    }
    return storeUnavailable
      ? ZLinkFrameworkRelocationReason.StoreUnavailable
      : ZLinkFrameworkRelocationReason.TargetUnavailable;
  }

  private hasLocalStatefulRelocationWork(): boolean {
    const spotManager = this.spotManager;
    if (spotManager !== undefined) {
      for (const meshName of this.options.registration.spotNodes.keys()) {
        if (spotManager.relocationActivations(meshName).length > 0) return true;
      }
    }
    return this.actorManager?.snapshotStates().some((state) => state.hasActorOrCreation) ?? false;
  }

  private async hasExactAutomaticPeerReadiness(
    location: Pick<ZLinkLocationRuntime, 'listLiveMeshNodes'>,
    targetApplicationVersion: bigint
  ): Promise<boolean> {
    for (const [meshName, registration] of this.options.registration.spotNodes) {
      if (
        (registration.router?.manualConnections?.length ?? 0) > 0 ||
        (registration.router?.manualPeerConnections?.length ?? 0) > 0
      ) {
        continue;
      }
      const node = this.spotNodeRuntime?.meshNode(meshName);
      if (node === undefined) return false;
      const local = node.status();
      const localDescriptor = this.spotNodeRuntime?.meshNodeDescriptor(meshName);
      if (localDescriptor === undefined) return false;
      const descriptors = await location.listLiveMeshNodes(meshName);
      const peers = node.peers();
      if (
        !hasExactPeerReadiness(
          descriptors,
          {
            ...local,
            applicationVersion: targetApplicationVersion,
            maintenanceWave: localDescriptor.maintenanceWave
          },
          peers
        )
      )
        return false;
    }
    return true;
  }

  private async publishHostRelocated(): Promise<void> {
    await Promise.all(
      [...this.options.registration.spotNodes.keys()].map((meshName) =>
        this.spotNodeRuntime?.publishMeshNodeState(
          ZLinkFrameworkRuntimeState.Relocated,
          undefined,
          meshName
        )
      )
    );
  }

  private async publishHostDraining(signal: AbortSignal): Promise<void> {
    try {
      const runtime = this.locationOwner.currentRuntime;
      if (runtime !== undefined && !(await runtime.publishDraining(signal))) {
        throw new Error('Draining descriptor publication returned false.');
      }
    } catch (error) {
      throw new ZLinkDrainingStatePublishError(error);
    }
    await awaitWithDrainSignal(this.serviceRelocation.drainTargetAttempts(), signal);
  }

  private async cleanupOwnerForDrain(signal: AbortSignal): Promise<void> {
    const runtime = this.locationOwner.currentRuntime;
    if (runtime === undefined) return;
    try {
      await runtime.cleanupOwner(signal);
    } catch (error) {
      throw new ZLinkOwnerCleanupError(error);
    }
  }

  private async forceStopMesh(meshName: string): Promise<void> {
    await waitForForcedSessionNotification(this.streamRuntime?.notifyServerDrain(meshName));
    await this.spotManager?.drainForShutdown(meshName, undefined, this.runtimeDeadline);
  }

  private registerInstanceApplicationLifecycle(
    meshName: string,
    node: ZLinkBackendMeshNode,
    spotManager: DefaultZLinkSpotManager
  ): void {
    const activationNode = node as typeof node & {
      registerInstanceApplicationLifecycle?: (
        lifecycle: ServiceInstanceApplicationLifecycle
      ) => void;
    };
    activationNode.registerInstanceApplicationLifecycle?.({
      isMaterialized: (target) =>
        spotManager.isInstanceMaterialized(
          meshName,
          target.stableType,
          target.targetSpotId as never
        ),
      isMaterializing: (target) =>
        spotManager.isInstanceMaterializing(meshName, target.targetSpotId as never),
      isIdleEvicting: (target) =>
        spotManager.isInstanceSpotIdleEvicting(meshName, target.targetSpotId as never),
      beginIdleEviction: (target) =>
        spotManager.beginInstanceSpotIdleEviction(meshName, target.targetSpotId as never),
      materialize: (target, objectGeneration) =>
        spotManager.materializeInstance(
          meshName,
          target.stableType,
          target.targetSpotId as never,
          objectGeneration,
          this.executionState?.abortController.signal
        ),
      discard: (target) => spotManager.discardInstance(meshName, target.targetSpotId as never),
      beginTerminal: (target) =>
        spotManager.beginInstanceTerminal(
          meshName,
          target.targetSpotId as never,
          target.objectGeneration
        ),
      completeTerminal: (target) =>
        spotManager.completeInstanceTerminal(
          meshName,
          target.targetSpotId as never,
          target.objectGeneration
        )
    });
  }

  setActorManager(actorManager: DefaultZLinkActorManager): void {
    this.actorManager = actorManager;
  }

  setSpotManager(spotManager: DefaultZLinkSpotManager): void {
    this.spotManager = spotManager;
    for (const [meshName, node] of this.spotNodeRuntime?.meshNodesByName ?? []) {
      this.registerInstanceApplicationLifecycle(meshName, node, spotManager);
    }
  }

  createActorManagerOptions(): Pick<
    ZLinkActorManagerOptions,
    | 'joinCoordinator'
    | 'actorMeshNameProvider'
    | 'actorLeaveSpot'
    | 'messageSerializers'
    | 'nativeActorNode'
    | 'nativeActorNodeProvider'
    | 'actorCreatedNodeRidProvider'
    | 'actorRefResolver'
    | 'actorCreatedNotifier'
    | 'actorDestroyedCleanup'
    | 'terminateActorActivation'
    | 'locationLifecycle'
    | 'boundSessionFactory'
    | 'shutdownSignal'
    | 'admission'
    | 'activationAdmission'
    | 'placementCreate'
  > {
    this.ensureLocationRuntime();
    return {
      ...this.actorRuntimeOptionsFactory().createActorManagerOptions(),
      activationAdmission: this.activationAdmission,
      placementCreate: async (actorId, actorType, createOnly, call, signal) => {
        const placement = this.actorPlacement;
        if (placement === undefined) {
          throw new ZLinkConfigurationException('Actor placement runtime is not initialized.');
        }
        const requestPayload = encodeFrameworkCreationPayload(
          call.request,
          this.options.registration.messageSerializers
        );
        return await placement.create(
          actorId,
          actorType,
          createOnly,
          call.meshName,
          requestPayload,
          call.timeoutMs,
          signal
        );
      }
    };
  }

  createActorClientOptions(): ConstructorParameters<typeof DefaultZLinkActorClient>[0] {
    this.ensureLocationRuntime();
    const options = this.actorRuntimeOptionsFactory().createActorClientOptions();
    this.actorClientLocationResolver = options.locationResolver();
    return options;
  }

  createLocationHandleResolver(): ZLinkStoreLocationResolvers | undefined {
    this.ensureLocationRuntime();
    return this.locationOwner.createRefResolver(this.meshRouters.spotLocationMeshNames());
  }

  createSpotManagerOptions(): Partial<ZLinkSpotManagerOptions> {
    this.ensureLocationRuntime();
    return {
      ...new ZLinkSpotRuntimeOptionsFactory({
        registration: this.options.registration,
        channelTransport: this.channelTransport,
        routeTransport: this.routeTransport,
        addressTransport: this.spotAddressTransport,
        spotPublisherTransport: this.spotPublisherTransport,
        meshRouters: this.meshRouters,
        runtimeEventPublisher: this.runtimeEventPublisher,
        spotNodeRuntime: () => this.spotNodeRuntime,
        actorManager: () => this.actorManager,
        // User Spot visibility is published only by the generic authority
        // coordinator after application initialization. The legacy location
        // claim would expose a Creating object before that Ready barrier.
        locationLifecycle: () => undefined,
        releaseInstanceAuthority: async (meshName, spotId, objectGeneration) => {
          await this.locationOwner.currentLifecycle?.releaseSpot(
            meshName,
            spotId,
            objectGeneration
          );
          this.spotNodeRuntime
            ?.meshNode(meshName)
            ?.completeClosedInstance?.(spotId, objectGeneration);
        },
        beginInstanceIdleClosingAuthority: (meshName, spotId, onCommitted) =>
          this.locationOwner.currentLifecycle?.beginInstanceSpotClosing(
            meshName,
            spotId,
            onCommitted
          ) ?? Promise.resolve(undefined),
        beginInstanceClosingAuthority: (meshName, spotId, onCommitted) =>
          this.locationOwner.currentLifecycle?.beginInstanceSpotClosing(
            meshName,
            spotId,
            onCommitted
          ) ?? Promise.resolve(undefined),
        beginUserClosingAuthority: async (meshName, spotId, objectGeneration, onCommitted) => {
          const coordinator = this.userSpotCoordinator;
          const nodeRid = this.spotNodeRuntime?.meshNode(meshName)?.status().routingId;
          if (coordinator === undefined || nodeRid === undefined) {
            throw new ZLinkConfigurationException(
              'User Spot context Close requires its authority coordinator.'
            );
          }
          return await coordinator.beginOwnerClose(
            { spotId: spotId as never, objectGeneration, meshName, nodeRid },
            onCommitted
          );
        },
        createLocationSpotRouteResolver: () => this.createLocationSpotRouteResolver(),
        boundSessionRelay: this.boundSessionRelay,
        actorHandoff: this.actorHandoff,
        dispatchErrorReporter: (errorSink) => this.createDispatchErrorReporter(errorSink),
        runtimeOrPreStartErrorSink: this.runtimeOrPreStartErrorSink,
        detachedTaskRunner: this.detachedTaskRunner(),
        metrics: this.metrics,
        admission: this.admission,
        statefulExecution: {
          admissionOpen: () => this.ownerAdmissionOpen(),
          hostState: () => this.runtimeState
        }
      }).create(this.actorTransferRuntime),
      activationAdmission: this.activationAdmission
    };
  }

  createPublicSpotManager(
    local: DefaultZLinkSpotManager
  ): import('../../contracts').ZLinkSpotManager {
    const locationStore = this.locationOwner.locationStore();
    if (locationStore === undefined) {
      throw new ZLinkConfigurationException('User Spot creation requires a Location Store.');
    }
    this.actorPlacement ??= new ZLinkActorPlacementCoordinator({
      store: locationStore,
      actorRelocationPolicy: (meshName, stableType) =>
        this.options.registration.spotNodes.get(meshName)?.actorFactoryRegistrations?.[stableType]
          ?.relocation.kind,
      remoteCreate: (meshName, targetNodeRid, request, timeoutMs) => {
        const node = this.spotNodeRuntime?.meshNode(meshName);
        if (node === undefined) {
          throw createInternalFrameworkException(
            ZLinkFrameworkInternalErrorKind.ObjectClientNotConfigured,
            `RouteMesh '${meshName}' has no local client node.`
          );
        }
        return node.requestActorCreate(targetNodeRid, request, timeoutMs);
      },
      decodeRemoteReply: (payload) => {
        const message = RuntimeMessage.from(payload);
        try {
          return decodeFrameworkPayloadMessage(
            message,
            this.options.registration.messageSerializers
          );
        } finally {
          message.close();
        }
      },
      target: async (requestedMesh, stableType, signal, excludedNodeRids) => {
        const clientMeshes = [...this.options.registration.spotNodes]
          .filter(([, node]) => hasObjectClientCapability(node.objectRole))
          .map(([meshName]) => meshName);
        const meshes = requestedMesh === undefined ? clientMeshes : [requestedMesh];
        if (meshes.length === 0) {
          throw createInternalFrameworkException(
            ZLinkFrameworkInternalErrorKind.ObjectClientNotConfigured,
            'No object-client RouteMesh is configured.'
          );
        }
        if (requestedMesh === undefined && meshes.length > 1) {
          throw createInternalFrameworkException(
            ZLinkFrameworkInternalErrorKind.MeshSelectionRequired,
            'Multiple object-client RouteMeshes are configured; call inMesh(...).'
          );
        }
        const meshName = meshes[0]!;
        if (!clientMeshes.includes(meshName)) {
          throw createInternalFrameworkException(
            ZLinkFrameworkInternalErrorKind.ObjectClientNotConfigured,
            `RouteMesh '${meshName}' has no object-client role.`
          );
        }
        const meshNode = this.spotNodeRuntime?.meshNode(meshName);
        const descriptors = await listAllMeshNodeDescriptors(locationStore, meshName, signal);
        const selected = await selectReadyLocationPlacementDescriptor(
          descriptors,
          meshNode,
          signal,
          (descriptor) => {
            const capability = descriptor.objectCapabilities.find(
              (candidate) => candidate.objectKind === 'actor' && candidate.stableType === stableType
            );
            return (
              descriptor.state === ZLinkFrameworkRuntimeState.Serving &&
              descriptor.objectRole === 'server' &&
              descriptor.placementWeight > 0 &&
              descriptor.entrySpotId !== undefined &&
              excludedNodeRids?.has(String(descriptor.rid)) !== true &&
              capability !== undefined
            );
          }
        );
        if (selected === undefined) return undefined;
        const localStatus = meshNode?.status();
        if (localStatus === undefined) {
          throw createInternalFrameworkException(
            ZLinkFrameworkInternalErrorKind.ObjectClientNotConfigured,
            `RouteMesh '${meshName}' has no local client node.`
          );
        }
        return {
          meshName,
          nodeRid: selected.rid,
          nodeGeneration: selected.lifecycleGeneration,
          sourceNodeRid: String(localStatus.routingId),
          sourceNodeGeneration: localStatus.lifecycleGeneration,
          entrySpotId: selected.entrySpotId!,
          owner: {
            ownerId: selected.ownerId,
            leaseGeneration: selected.leaseGeneration
          },
          isLocal:
            String(localStatus.routingId) === String(selected.rid) &&
            localStatus.lifecycleGeneration === selected.lifecycleGeneration
        };
      }
    });
    const coordinator = (this.userSpotCoordinator = new ZLinkUserSpotCreationCoordinator({
      store: locationStore,
      publishReadyRoute: (meshName, route) => {
        this.cachedLocationSpotRouteResolver?.invalidate?.(route.spot.spotId);
        this.spotNodeRuntime?.meshNode(meshName)?.rememberSpotRoute?.(route);
      },
      forgetReadyRoute: (meshName, route) => {
        this.cachedLocationSpotRouteResolver?.invalidate?.(route.spot.spotId);
        this.spotNodeRuntime
          ?.meshNode(meshName)
          ?.forgetSpotRoute?.(route.spot, route.authorityOwnerGeneration, route.storeVersion);
      },
      remoteCreate: (meshName, targetNodeRid, request, timeoutMs) => {
        const node = this.spotNodeRuntime?.meshNode(meshName);
        if (node === undefined) {
          throw createInternalFrameworkException(
            ZLinkFrameworkInternalErrorKind.ObjectClientNotConfigured,
            `RouteMesh '${meshName}' has no local client node.`
          );
        }
        return node.requestUserSpotCreate(targetNodeRid, request, timeoutMs);
      },
      decodeRemoteReply: (payload) => {
        const message = RuntimeMessage.from(payload);
        try {
          return decodeFrameworkPayloadMessage(
            message,
            this.options.registration.messageSerializers
          );
        } finally {
          message.close();
        }
      },
      target: async (request, signal, excludedNodeRids) => {
        const clientMeshes = [...this.options.registration.spotNodes]
          .filter(([, node]) => hasObjectClientCapability(node.objectRole))
          .map(([meshName]) => meshName);
        const meshes = request.meshName === undefined ? clientMeshes : [request.meshName];
        if (meshes.length === 0) {
          throw createInternalFrameworkException(
            ZLinkFrameworkInternalErrorKind.ObjectClientNotConfigured,
            'No object-client RouteMesh is configured.'
          );
        }
        if (request.meshName === undefined && meshes.length > 1) {
          throw createInternalFrameworkException(
            ZLinkFrameworkInternalErrorKind.MeshSelectionRequired,
            'Multiple object-client RouteMeshes are configured; call inMesh(...).'
          );
        }
        const meshName = meshes[0]!;
        if (!this.options.registration.spotNodes.has(meshName)) {
          throw createInternalFrameworkException(
            ZLinkFrameworkInternalErrorKind.MeshNotFound,
            `RouteMesh '${meshName}' is not configured.`
          );
        }
        if (!clientMeshes.includes(meshName)) {
          throw createInternalFrameworkException(
            ZLinkFrameworkInternalErrorKind.ObjectClientNotConfigured,
            `RouteMesh '${meshName}' has no object-client role.`
          );
        }
        const meshNode = this.spotNodeRuntime?.meshNode(meshName);
        const descriptors = await listAllMeshNodeDescriptors(locationStore, meshName, signal);
        const selected = await selectReadyLocationPlacementDescriptor(
          descriptors,
          meshNode,
          signal,
          (descriptor) => {
            const capability = descriptor.objectCapabilities.find(
              (candidate) =>
                candidate.objectKind === 'user_spot' && candidate.stableType === request.stableType
            );
            const spotTypeCapacity = descriptor.populationCapacity.spotTypes.find(
              (candidate) =>
                candidate.objectKind === 'user_spot' && candidate.stableType === request.stableType
            );
            const spots = descriptor.populationCapacity.spots;
            return (
              descriptor.state === 1 &&
              descriptor.objectRole === 'server' &&
              descriptor.placementWeight > 0 &&
              excludedNodeRids?.has(String(descriptor.rid)) !== true &&
              (spots.limit === 0 || spots.active + spots.reserved < spots.limit) &&
              capability !== undefined &&
              (spotTypeCapacity === undefined ||
                spotTypeCapacity.limit === 0 ||
                spotTypeCapacity.active + spotTypeCapacity.reserved < spotTypeCapacity.limit)
            );
          }
        );
        if (selected === undefined) return undefined;
        const localStatus = meshNode?.status();
        return {
          meshName,
          nodeRid: selected.rid,
          nodeGeneration: selected.lifecycleGeneration,
          owner: {
            ownerId: selected.ownerId,
            leaseGeneration: selected.leaseGeneration
          },
          isLocal:
            localStatus !== undefined &&
            String(localStatus.routingId) === String(selected.rid) &&
            localStatus.lifecycleGeneration === selected.lifecycleGeneration
        };
      }
    }));
    const factories = new Map(
      [...this.options.registration.spotNodes].map(([meshName, node]) => [
        meshName,
        new Map(Object.entries(node.spotFactoryRegistrations ?? {}))
      ])
    );
    const registerUserSpotHandlers = (runtime: ZLinkSpotNodeRuntimeManager) => {
      for (const [meshName] of this.options.registration.spotNodes) {
        const node = runtime.meshNode(meshName);
        if (node === undefined) continue;
        node.registerUserSpotOperationHandler({
          create: async (record, signal) => {
            const coordinated = await coordinator.handleRemoteCreate(
              record,
              async (requestPayload, authority, createSignal) => {
                const selected = factories.get(meshName)?.get(record.stableType);
                if (selected === undefined) {
                  throw new ZLinkConfigurationException(
                    `User Spot factory '${record.stableType}' is not registered on RouteMesh '${meshName}'.`
                  );
                }
                const request = decodeFrameworkCreationPayload(
                  requestPayload,
                  this.options.registration.messageSerializers
                );
                local.beginUserSpotPublication(meshName, record.spotId as never);
                try {
                  const result = await local.getOrCreateWithAuthority(
                    meshName,
                    selected.implementation as never,
                    record.spotId as never,
                    request,
                    {
                      stableType: record.stableType,
                      objectGeneration: authority.objectGeneration,
                      authorityOwnerGeneration: authority.authorityOwnerGeneration
                    },
                    createSignal
                  );
                  return {
                    ...result,
                    publication: {
                      publish: () => local.publishUserSpot(meshName, record.spotId as never),
                      abort: () => local.abortUserSpotPublication(meshName, record.spotId as never)
                    }
                  };
                } catch (error) {
                  local.abortUserSpotPublication(meshName, record.spotId as never);
                  throw error;
                }
              },
              async (cleanupSignal) => {
                await local.close(meshName, record.spotId as never, cleanupSignal);
              },
              signal
            );
            const reply = coordinated.result.reply;
            let payload;
            if (reply !== undefined && coordinated.result.state !== ZLinkSpotCreateState.Existing) {
              const message = encodeFrameworkPayloadMessage(
                reply,
                this.options.registration.messageSerializers
              );
              try {
                payload = {
                  packetName: 'ZLinkFrameworkUserSpotReply',
                  contentType: ZlinkStreamContentType.Raw,
                  payload: Buffer.from(message.data())
                };
              } finally {
                message.close();
              }
            }
            return {
              terminalResult: RequestResult.Ok,
              failureCode: 0,
              tail: {
                kind: 'userSpotCreate' as const,
                createResult:
                  coordinated.result.state === ZLinkSpotCreateState.Existing
                    ? ('existing' as const)
                    : coordinated.result.state === ZLinkSpotCreateState.Created
                      ? ('created' as const)
                      : ('rejected' as const),
                spotId: String(coordinated.spot.spotId),
                objectGeneration: coordinated.spot.objectGeneration
              },
              ...(payload === undefined ? {} : { payload })
            };
          },
          createActor: async (record, signal) => {
            const actorPlacement = this.actorPlacement;
            const actors = this.actorManager;
            if (actorPlacement === undefined || actors === undefined) {
              throw new ZLinkConfigurationException(
                'Actor placement target runtime is not initialized.'
              );
            }
            return await actorPlacement.handleRemoteCreate(
              record,
              async (requestPayload, authority, createSignal) => {
                const request = decodeFrameworkCreationPayload(
                  requestPayload,
                  this.options.registration.messageSerializers
                );
                const entrySpotId = this.spotNodeRuntime?.meshNodeDescriptor(meshName)?.entrySpotId;
                if (entrySpotId === undefined) {
                  throw new ZLinkConfigurationException(
                    `Remote Actor creation requires the '${meshName}' MeshNode Entry Spot descriptor.`
                  );
                }
                const entrySpot = node.entrySpot().status();
                const nativeActorRef = node.restoreActorAuthority?.(
                  record.actorId,
                  record.stableType,
                  authority.objectGeneration,
                  authority.authorityOwnerGeneration,
                  String(entrySpot.routingId),
                  entrySpot.lifecycleGeneration,
                  1n
                );
                if (nativeActorRef === undefined) {
                  throw new ZLinkConfigurationException(
                    'Remote Actor creation requires native authority restoration support.'
                  );
                }
                const local = await actors.createReservedActorResult(
                  record.actorId,
                  record.stableType,
                  request,
                  createSignal,
                  nativeActorRef
                );
                if (local.status === 'failed') {
                  return { result: 'failed' as const, error: local.error };
                }
                if (local.status === 'rejected') {
                  let reply: Buffer | undefined;
                  if (local.reply !== undefined) {
                    const encoded = encodeFrameworkPayloadMessage(
                      local.reply,
                      this.options.registration.messageSerializers
                    );
                    try {
                      reply = Buffer.from(encoded.data());
                    } finally {
                      encoded.close();
                    }
                  }
                  return {
                    result: 'rejected' as const,
                    ...(reply === undefined ? {} : { reply })
                  };
                }
                if (
                  local.actorRef.objectGeneration !== authority.objectGeneration ||
                  String(local.actorRef.nodeRid) !== record.reservation.targetNodeRid
                ) {
                  throw createInternalFrameworkException(
                    ZLinkFrameworkInternalErrorKind.ActorCreateFailed,
                    `Actor '${record.actorId}' native authority does not match its placement reservation.`
                  );
                }
                let reply: Buffer | undefined;
                if (local.reply !== undefined) {
                  const encoded = encodeFrameworkPayloadMessage(
                    local.reply,
                    this.options.registration.messageSerializers
                  );
                  try {
                    reply = Buffer.from(encoded.data());
                  } finally {
                    encoded.close();
                  }
                }
                return {
                  result: 'created' as const,
                  actor: {
                    ...local.actorRef,
                    meshName
                  },
                  entrySpotId,
                  entrySpotGeneration: entrySpot.lifecycleGeneration,
                  ...(reply === undefined ? {} : { reply }),
                  onPublished: () =>
                    actors.adoptCreatedAuthority(
                      record.actorId,
                      authority.authorityOwnerGeneration,
                      authority.ownerLeaseGeneration
                    )
                };
              },
              signal
            );
          },
          close: async (record, signal) => {
            return {
              terminalResult: RequestResult.Ok,
              failureCode: 0,
              tail: {
                kind: 'userSpotClose' as const,
                closed: await coordinator.handleRemoteClose(
                  record,
                  (spot, beginAuthority, closeSignal) =>
                    local.closeUserWithAuthority(
                      spot.meshName,
                      spot.spotId,
                      beginAuthority,
                      closeSignal
                    ),
                  signal,
                  (spot) => local.isSpotClosing(spot.meshName, spot.spotId)
                )
              }
            };
          }
        });
      }
    };
    this.registerUserSpotHandlers = registerUserSpotHandlers;
    if (this.spotNodeRuntime !== undefined) {
      registerUserSpotHandlers(this.spotNodeRuntime);
    }
    return new ZLinkPublicSpotManager({
      local,
      coordinator,
      factories,
      resolver: () => this.createLocationSpotRouteResolver(),
      forgetReadyRoute: (spot, snapshot) => {
        coordinator.forgetReadyRoute(spot.spotId, snapshot);
      },
      isLocalNode: (meshName, nodeRid) => {
        const status = this.spotNodeRuntime?.meshNode(meshName)?.status();
        return status !== undefined && String(status.routingId) === String(nodeRid);
      },
      remoteClose: (meshName, targetNodeRid, request, timeoutMs) => {
        const node = this.spotNodeRuntime?.meshNode(meshName);
        if (node === undefined) {
          throw createInternalFrameworkException(
            ZLinkFrameworkInternalErrorKind.ObjectClientNotConfigured,
            `RouteMesh '${meshName}' has no local client node.`
          );
        }
        return node.requestUserSpotClose(targetNodeRid, request, timeoutMs);
      },
      defaultTimeoutMs: this.options.registration.requestTimeoutMs ?? DEFAULT_REQUEST_TIMEOUT_MS,
      messageSerializers: this.options.registration.messageSerializers
    });
  }

  private actorRuntimeOptionsFactory(): ZLinkActorRuntimeOptionsFactory {
    return new ZLinkActorRuntimeOptionsFactory({
      terminateActorActivation: (actor, terminal) =>
        this.spotNodeRuntime?.terminateActorActivation(actor, terminal) ?? terminal(),
      registration: this.options.registration,
      messageFlow: () => this.createDispatchErrorReporter(this.runtimeOrPreStartErrorSink).flow,
      routeTransport: this.routeTransport,
      streamBindingRuntime: this.streamBindingRuntime,
      providerResolver: this.options.providerResolver,
      spotManager: () => this.spotManager,
      actorManager: () => this.actorManager,
      primaryMeshNode: () => this.requirePrimaryMeshNode(),
      primaryMeshNodeOrUndefined: () => this.spotNodeRuntime?.primaryMeshNode,
      primaryMeshCompletions: () => this.spotNodeRuntime?.primaryMeshCompletions,
      meshNode: (meshName) => this.spotNodeRuntime?.meshNode(meshName),
      meshCompletions: (meshName) => this.spotNodeRuntime?.meshCompletionTable(meshName),
      notifyEntrySpotActorCreated: (nodeRid, actor, createRequest, signal) =>
        this.spotNodeRuntime?.notifyEntrySpotActorCreated(nodeRid, actor, createRequest, signal) ??
        Promise.resolve(undefined),
      notifyEntrySpotActorJoined: (actor, signal) =>
        this.spotNodeRuntime?.notifyPrimaryEntrySpotActorJoined(actor, signal) ?? Promise.resolve(),
      notifyEntrySpotActorLeft: (actor, signal) =>
        this.spotNodeRuntime?.notifyPrimaryEntrySpotActorLeft(actor, signal) ?? Promise.resolve(),
      locationLifecycle: () => this.locationOwner.currentLifecycle,
      primaryMeshName: () => this.meshRouters.primaryMeshName(),
      actorMeshName: (actorType) => this.meshRouters.actorMeshName(actorType),
      createLocationSpotRouteResolver: () => this.createLocationSpotRouteResolver(),
      createActorLocationResolver: () => this.createActorLocationResolver(),
      forgetDestroyedActorRef: (actorId) => this.destroyedActorRefs.delete(actorId),
      rememberDestroyedActorRef: (actorId, actorRef) =>
        this.destroyedActorRefs.set(actorId, actorRef),
      invalidateActorRoute: (actorId) =>
        this.actorClientLocationResolver?.invalidateActorRoute(actorId),
      publishActorAuthority: async (actorType, actorRef, ownerNodeGeneration, signal) => {
        const store = this.locationOwner.currentStores?.locationStore;
        if (store === undefined) return;
        const owner = this.locationOwner.currentRuntime?.currentOwnerToken;
        const meshName = this.meshRouters.actorMeshName(actorType);
        if (owner === undefined || meshName === undefined) {
          throw new ZLinkConfigurationException(
            `Actor '${actorRef.actorId}' authority requires an active owner and Actor RouteMesh.`
          );
        }
        const entrySpotId = this.spotNodeRuntime?.entrySpotIdForMesh(meshName);
        if (entrySpotId === undefined) {
          throw new ZLinkConfigurationException(
            `Actor '${actorRef.actorId}' authority requires its RouteMesh Entry Spot.`
          );
        }
        await publishInitialActorAuthority(
          store,
          {
            actorType,
            actor: actorRef,
            meshName,
            ownerNodeGeneration,
            owner,
            spotId: entrySpotId,
            spotGeneration: ownerNodeGeneration,
            spotKind: ZLinkSpotKind.Entry
          },
          signal
        );
        this.actorClientLocationResolver?.invalidateActorRoute(actorRef.actorId);
      },
      reportPostCommitError: (error) =>
        this.runtimeOrPreStartErrorSink.reportRuntimeTaskException(
          'post-commit actor binding',
          error
        ),
      reportBoundSessionSendError: (error) =>
        this.runtimeOrPreStartErrorSink.reportRuntimeTaskException(
          'bound session one-way submit',
          error
        ),
      actorHandoff: this.actorHandoff,
      actorTransferRuntime: this.actorTransferRuntime,
      actorJoinRelocation: this.serviceRelocation,
      actorTransferRegistry: this.actorTransferRegistry,
      shutdownSignal: () => this.executionState?.abortController.signal,
      metrics: this.metrics,
      admission: this.admission,
      actorPacketTargetForState: (actorId) =>
        this.boundSessionRelay.actorPackets.actorPacketTargetForState(actorId),
      flowCreationEnabled: () => this.flowCreationEnabled(),
      traceBoundSessionSend: (actorId, packetName) => {
        const flow = this.createDispatchErrorReporter(this.runtimeOrPreStartErrorSink).flow;
        flowIfEnabled(flow, ZLinkMessageFlowOutcome.Sent)?.trace({
          outcome: ZLinkMessageFlowOutcome.Sent,
          surface: ZLinkDispatchErrorSurface.SpotActor,
          messageKind: ZLinkDispatchMessageKind.ActorSend,
          packetName,
          actorId
        });
      }
    });
  }

  private createChannelRuntimeOptions() {
    return new ZLinkChannelRuntimeOptionsFactory({
      monitoringAdapter: this.backendAdapterFactory.createMonitoringAdapter(),
      messageFlowModeCell: this.messageFlowModeCell,
      boundSessionRelay: this.boundSessionRelay,
      applicationJobQueue: this.applicationJobQueue,
      spotManager: () => this.spotManager,
      oneWayFailureSink: (error) =>
        this.runtimeOrPreStartErrorSink.reportRuntimeTaskException('channel one-way submit', error)
    }).create();
  }

  private createSpotNodeRuntimeOptions(
    context: ZLinkBackendContext,
    dispatchErrors: ZLinkDispatchErrorReporter
  ) {
    const options = new ZLinkSpotNodeRuntimeOptionsFactory({
      registration: this.options.registration,
      backendAdapterFactory: this.backendAdapterFactory,
      context,
      channelTransport: this.channelTransport,
      routeTransport: this.routeTransport,
      spotPublisherTransport: this.spotPublisherTransport,
      meshRouters: this.meshRouters,
      providerResolver: this.options.providerResolver,
      dispatchErrors,
      runtimeEventPublisher: this.runtimeEventPublisher,
      entryActorRuntime: this.entryActorRuntime,
      actorTransferRuntime: this.actorTransferRuntime,
      boundSessionRelay: this.boundSessionRelay,
      actorHandoff: this.actorHandoff,
      detachedTaskRunner: this.detachedTaskRunner(),
      metrics: this.metrics,
      applicationJobQueue: this.applicationJobQueue,
      applicationJobReceiveFlowFailureSink: (error) =>
        this.runtimeOrPreStartErrorSink.reportRuntimeTaskException(
          'RouteMesh receive-flow configuration',
          error
        )
    }).create();
    return {
      ...options,
      errorSink: this.runtimeOrPreStartErrorSink,
      peerAdmissionSealed: (meshName: string) => !this.admission.accepts(meshName),
      messageFollowReceiver: (record: ServiceMessageFollowRecord) => {
        if (record.source.kind === 'actor') {
          this.actorClientLocationResolver?.invalidateActorRouteIfMatches({
            actorId: record.source.actor.actorId,
            objectGeneration: record.source.actor.generation,
            targetNodeRid: record.source.targetNodeRid,
            targetNodeGeneration: record.source.targetNodeGeneration,
            authorityOwnerGeneration: record.source.authorityOwnerGeneration,
            ownerLeaseGeneration: record.source.ownerLeaseGeneration
          });
          return;
        }
        this.actorClientLocationResolver?.invalidateSpotRouteIfMatches({
          spotId: record.source.spot.spotId,
          objectGeneration: record.source.spot.generation,
          targetNodeRid: record.source.targetNodeRid,
          targetNodeGeneration: record.source.targetNodeGeneration,
          authorityOwnerGeneration: record.source.authorityOwnerGeneration,
          ownerLeaseGeneration: record.source.ownerLeaseGeneration
        });
      },
      meshRecordDispatcher: (meshName: string, owner: ReadyRecord, record: ReceiveRecord) =>
        this.dispatchMeshRecord(
          meshName,
          owner,
          record,
          this.executionState?.abortController.signal
        ),
      activationConcurrency: (meshName: string) => this.activationAdmission.current(meshName)
    };
  }

  private async dispatchMeshRecord(
    meshName: string,
    owner: ReadyRecord,
    record: ReceiveRecord,
    signal?: AbortSignal
  ): Promise<void> {
    if (record.operationKind === OperationKind.ActorJoin) {
      if (this.spotManager === undefined) {
        throw new ZLinkConfigurationException(
          'MeshNode Actor join dispatch requires the Spot manager.'
        );
      }
      return this.spotManager.dispatchMeshActorJoin(meshName, owner, record);
    }
    if (record.kind === ReceiveKind.ActorBinding) {
      if (record.kindData?.kind !== 'actorBinding') {
        throw new ZLinkConfigurationException(
          'MeshNode Actor binding record has no binding generation.'
        );
      }
      const binding = record.kindData;
      const replyBindingFailure = (kind: ZLinkFrameworkInternalErrorKind): void => {
        const terminal = internalFrameworkWireReply(kind);
        if (
          record.replyFailure?.(terminal.terminalResult, terminal.failureCode) !== SubmitResult.Ok
        ) {
          throw new ZLinkConfigurationException(
            `MeshNode Actor binding target '${binding.actor.actorId}' failure acknowledgement failed.`
          );
        }
      };
      const replyBindingSuccess = (): void => {
        if (record.reply(Buffer.alloc(0)) !== SubmitResult.Ok) {
          throw new ZLinkConfigurationException(
            `MeshNode Actor binding target '${binding.actor.actorId}' acknowledgement failed.`
          );
        }
      };
      const state = this.actorManager?.getState(binding.actor.actorId);
      const nativeActorRef = state?.nativeActorRef;
      if (
        state === undefined ||
        nativeActorRef === undefined ||
        nativeActorRef.actorId !== binding.actor.actorId ||
        !routingIdsEqual(nativeActorRef.nodeRid, binding.actor.nodeRid)
      ) {
        replyBindingFailure(ZLinkFrameworkInternalErrorKind.ActorLocationStale);
        return;
      }
      if (nativeActorRef.generation !== binding.actor.generation) {
        replyBindingFailure(ZLinkFrameworkInternalErrorKind.ActorGenerationStale);
        return;
      }
      const node =
        state.meshName === meshName ? this.spotNodeRuntime?.meshNode(meshName) : undefined;
      const localStatus = node?.status();
      // ownsLocation tracks local cleanup responsibility, not Actor authority.
      // A relocation target can exact-read committed authority without owning that cleanup.
      if (
        localStatus === undefined ||
        !routingIdsEqual(localStatus.routingId, binding.actor.nodeRid) ||
        localStatus.lifecycleGeneration !== binding.actorNodeGeneration ||
        state.locationGeneration !== binding.authorityOwnerGeneration ||
        state.isMoving
      ) {
        replyBindingFailure(ZLinkFrameworkInternalErrorKind.ActorLocationStale);
        return;
      }
      const sessionOwnerIsLocal =
        routingIdsEqual(localStatus.routingId, binding.sessionNodeRid) &&
        localStatus.lifecycleGeneration === binding.sessionOwnerNodeGeneration;
      if (binding.transition === 'tombstone') {
        const actorRef: ActorRef = {
          actorId: binding.actor.actorId,
          objectGeneration: binding.actor.generation,
          meshName,
          nodeRid: binding.actor.nodeRid
        };
        if (sessionOwnerIsLocal) {
          releaseApplicationJobPermitBeforeHandler();
          await this.streamBindingRuntime.retireRemoteBinding(
            actorRef,
            binding.sessionRid,
            binding.bindingGeneration,
            signal
          );
        }
        state.retireBoundSessionBinding(binding);
        replyBindingSuccess();
        return;
      }
      const target = this.boundSessionRelay.boundSessions.resolveRemoteBoundSessionTarget(
        binding.sessionNodeRid,
        binding.sessionRid
      );
      if (target === undefined) {
        replyBindingFailure(ZLinkFrameworkInternalErrorKind.ActorLocationStale);
        return;
      }
      const installed = state.installBoundSessionBinding({
        ...target,
        sessionNodeRid: binding.sessionNodeRid,
        sessionRid: binding.sessionRid,
        sessionOwnerNodeGeneration: binding.sessionOwnerNodeGeneration,
        sessionOwnerId: binding.sessionOwnerId,
        sessionOwnerLeaseGeneration: binding.sessionOwnerLeaseGeneration,
        bindingGeneration: binding.bindingGeneration
      });
      if (!installed) {
        replyBindingFailure(ZLinkFrameworkInternalErrorKind.ActorLocationStale);
        return;
      }
      replyBindingSuccess();
      return Promise.resolve();
    }
    switch (record.kind) {
      case ReceiveKind.TransferControl:
        if (record.kindData?.kind !== 'transferControl') {
          throw new ZLinkConfigurationException(
            'MeshNode transfer control record has no typed control payload.'
          );
        }
        return this.actorTransferAuthorityRuntime.handle(meshName, record.kindData, signal);
      case ReceiveKind.ChannelSend:
      case ReceiveKind.ChannelRequest: {
        const channelRuntime = this.channelRuntime;
        if (channelRuntime === undefined) {
          throw new ZLinkConfigurationException(
            'MeshNode channel dispatch requires the channel runtime.'
          );
        }
        return this.admission.run(meshName, 'RouteMesh channel dispatch', () =>
          channelRuntime.dispatchMeshChannel(meshName, record, signal)
        );
      }
      case ReceiveKind.NodeSend:
      case ReceiveKind.NodeRequest: {
        return this.admission.run(meshName, 'RouteMesh node dispatch', async () => {
          if (await this.serviceRelocation.tryHandleControl(meshName, record, signal)) {
            return;
          }
          if (record.kind === ReceiveKind.NodeRequest && record.parts.length === 1) {
            const terminal = decodeRemoteActorSourceLeaveTerminal(record.parts[0]!.data());
            if (terminal !== undefined) {
              if (
                (await this.spotManager?.completeFormalSourceLeaveTerminal(
                  terminal.actorId,
                  terminal.transferId,
                  terminal.succeeded
                )) !== true
              ) {
                throw new ZLinkConfigurationException(
                  `Actor '${terminal.actorId}' has no matching formal transfer terminal gate.`
                );
              }
              if (record.reply(Buffer.alloc(0)) !== SubmitResult.Ok) {
                throw new ZLinkConfigurationException(
                  `Actor '${terminal.actorId}' formal transfer terminal acknowledgement failed.`
                );
              }
              return;
            }
          }
          const channelRuntime = this.channelRuntime;
          if (channelRuntime === undefined) {
            throw new ZLinkConfigurationException(
              'MeshNode node-direct dispatch requires the channel runtime.'
            );
          }
          return await channelRuntime.dispatchMeshRoute(meshName, record, signal);
        });
      }
      case ReceiveKind.SpotSend:
      case ReceiveKind.SpotRequest:
      case ReceiveKind.SpotMulticast:
        return this.admission.run(meshName, 'RouteMesh Spot dispatch', () =>
          this.dispatchMeshSpotRecord(meshName, owner, record)
        );
      case ReceiveKind.InstanceSpotActivation:
        if (this.spotManager === undefined) {
          throw new ZLinkConfigurationException(
            'MeshNode Instance Spot dispatch requires the Spot manager.'
          );
        }
        return this.admission.run(meshName, 'RouteMesh Instance Spot dispatch', () =>
          this.spotManager!.dispatchMeshInstance(meshName, owner, record)
        );
      case ReceiveKind.SpotControl:
        if (this.spotManager === undefined) {
          throw new ZLinkConfigurationException(
            'MeshNode Spot control dispatch requires the Spot manager.'
          );
        }
        return this.spotManager.dispatchMeshSpotControl(meshName, owner, record);
      case ReceiveKind.ActorSend:
      case ReceiveKind.ActorRequest:
        if (this.spotManager === undefined) {
          throw new ZLinkConfigurationException(
            'MeshNode Actor dispatch requires the Spot manager.'
          );
        }
        return this.admission.run(meshName, 'RouteMesh Actor dispatch', () =>
          this.spotManager!.dispatchMeshActor(meshName, owner, record)
        );
      case ReceiveKind.SendReady:
        return Promise.resolve();
      default:
        throw new ZLinkConfigurationException(
          `MeshNode record kind '${record.kind}' does not yet have a registered framework consumer.`
        );
    }
  }

  private submitLocalMeshRoute(
    meshName: string,
    sourceNodeRid: string,
    parts: readonly MessageLike[]
  ) {
    const state = this.executionState;
    const channelRuntime = this.channelRuntime;
    if (state === undefined || channelRuntime === undefined || !this.admission.accepts(meshName)) {
      return { status: ZLinkSubmitStatus.Shutdown } as const;
    }
    if (!channelRuntime.canDispatchLocalMeshRoute(meshName)) {
      return { status: ZLinkSubmitStatus.TargetNotFound } as const;
    }
    const claim = this.admission.claim(meshName, 'RouteMesh local node dispatch');
    const ownedParts: RuntimeMessage[] = [];
    try {
      for (const part of parts) {
        ownedParts.push(
          RuntimeMessage.from(
            typeof (part as { data?: unknown }).data === 'function'
              ? (part as { data(): Buffer }).data()
              : part
          )
        );
      }
    } catch (error) {
      for (const part of ownedParts) part.close();
      claim.close();
      throw error;
    }
    try {
      state.taskRunner.runDetached('mesh-node-local-route-dispatch', async () => {
        try {
          const permit = await this.applicationJobQueue.acquire(
            state.abortController.signal,
            'local'
          );
          permit.markApplicationQueued();
          await runWithApplicationJobPermit(permit, () =>
            channelRuntime.dispatchLocalMeshRoute(meshName, sourceNodeRid, ownedParts)
          );
        } finally {
          for (const part of ownedParts) part.close();
          claim.close();
        }
      });
    } catch (error) {
      for (const part of ownedParts) part.close();
      claim.close();
      throw error;
    }
    return { status: ZLinkSubmitStatus.Submitted } as const;
  }

  private async dispatchMeshSpotRecord(
    meshName: string,
    owner: ReadyRecord,
    record: ReceiveRecord
  ): Promise<void> {
    const envelope = decodeChannelEnvelope(record.parts, undefined, this.flowCreationEnabled());
    if (envelope.packetName === ZLINK_REMOTE_ACTOR_PACKET_RELAY_PACKET) {
      const payload = decodeChannelPayload(envelope, {
        serializers: this.options.registration.messageSerializers
      });
      const context = {
        meshName,
        packetName: envelope.packetName,
        contentType: envelope.header.contentType,
        sourceNodeRid: String(record.sourceNodeRid ?? '') as RoutingId,
        metadata: zlinkMessageMetadata(envelope.header.metadata),
        correlationId: envelope.header.correlationId ?? undefined
      };
      try {
        const response = await this.boundSessionRelay.actorPackets.receiveRemoteActorPacketRelay(
          payload,
          context
        );
        if (record.kind === ReceiveKind.SpotRequest) {
          record.reply(encodeChannelReplyParts(envelope.header, response));
        }
      } catch (error) {
        if (record.kind !== ReceiveKind.SpotRequest) {
          throw error;
        }
        record.reply(
          encodeChannelErrorReplyParts(
            envelope.header,
            error instanceof Error ? error.message : String(error)
          )
        );
      }
      return;
    }
    if ((await this.spotNodeRuntime?.dispatchEntrySpotRecord(meshName, owner, record)) === true) {
      return;
    }
    if (this.spotManager === undefined) {
      throw new ZLinkConfigurationException('MeshNode Spot dispatch requires the Spot manager.');
    }
    await this.spotManager.dispatchMeshSpot(meshName, owner, record);
  }

  private createDispatchErrorReporter(
    errorSink: ZLinkDispatchErrorSink
  ): ZLinkDispatchErrorReporter {
    const existing = this.dispatchErrorReporters.get(errorSink);
    if (existing !== undefined) {
      return existing;
    }
    const reporter = new ZLinkDispatchErrorReporter(
      undefined,
      undefined,
      errorSink,
      this.diagnosticsContext(),
      this.metrics
    );
    this.dispatchErrorReporters.set(errorSink, reporter);
    return reporter;
  }

  detachedTaskRunner(): ZLinkDetachedTaskRunner {
    return {
      runDetached: (taskName, callback) => {
        const runner = this.executionState?.taskRunner;
        if (runner !== undefined) {
          runner.runDetached(taskName, () => callback());
          return;
        }
        void callback().catch((error) =>
          this.preStartErrorSink.reportRuntimeTaskException(taskName, error)
        );
      }
    };
  }

  private diagnosticsContext(): ZLinkDiagnosticsContext {
    this.cachedDiagnosticsContext ??= createDiagnosticsContext(
      this.options.registration.dispatch,
      this.options.providerResolver,
      this.messageFlowModeCell
    );
    return this.cachedDiagnosticsContext;
  }

  private flowCreationEnabled(): boolean {
    const mode =
      this.executionState === undefined
        ? (this.options.registration.dispatch?.diagnostics.messageFlow ?? 'errors')
        : this.messageFlowModeCell.mode;
    return mode !== 'off';
  }

  private runLifecycle<T>(operation: () => T): T {
    const current = currentOrCreateFlow('Lifecycle', false);
    const flow =
      current?.flowOrigin === 'Lifecycle'
        ? current
        : createInboundFlow(undefined, 'Lifecycle', this.flowCreationEnabled(), undefined);
    return runWithFlow(flow, operation);
  }

  requirePrimaryMeshNode() {
    const node = this.spotNodeRuntime?.primaryMeshNode;
    if (node === undefined) {
      throw new Error('Primary Entry Spot node is not started.');
    }
    return node;
  }

  private ensureLocationRuntime(): ZLinkLocationRuntime | undefined {
    return this.locationOwner.ensureRuntime(this.meshRouters.primaryMeshName());
  }

  private createLocationSpotRouteResolver(): ZLinkSpotRouteResolver | undefined {
    if (this.cachedLocationSpotRouteResolver !== undefined) {
      return this.cachedLocationSpotRouteResolver;
    }
    this.ensureLocationRuntime();
    const legacy = this.locationOwner.createSpotRouteResolver(
      this.meshRouters.spotLocationMeshNames(),
      this.meshRouters.spotRouterChannelIdByMesh(),
      (spotId) => this.spotManager?.resolveLocalSpotRoute(spotId)
    );
    const authority = this.locationOwner.currentStores?.locationStore;
    const resolver =
      authority === undefined
        ? legacy
        : new ZLinkAuthoritySpotRouteResolver(
            authority,
            this.meshRouters.spotRouterChannelIdByMesh(),
            legacy,
            this.locationOwner.currentLeaseTracker,
            this.options.registration.locations.options.routeCacheMaxAgeMs,
            undefined,
            async (meshName, nodeRid, expectedLifecycleGeneration, signal) => {
              const descriptor = (
                await listAllMeshNodeDescriptors(authority, meshName, signal)
              ).find(
                (candidate) =>
                  routingIdsEqual(candidate.rid, nodeRid) &&
                  candidate.lifecycleGeneration === expectedLifecycleGeneration
              );
              return descriptor?.state;
            }
          );
    this.cachedLocationSpotRouteResolver = resolver;
    this.locationOwner.setSpotRouteInvalidator(
      resolver === undefined ? undefined : (spotId) => resolver.invalidate?.(spotId)
    );
    return resolver;
  }

  private createActorLocationResolver(): ZLinkStoreLocationResolvers | undefined {
    return this.locationOwner.createActorLocationResolver(this.meshRouters.spotLocationMeshNames());
  }
}

export function hasUnsupportedManualTopology(registration: ZLinkFrameworkRegistration): boolean {
  if (
    [...registration.routeChannelOptions.values()].some(
      (route) => (route.manualConnections?.length ?? 0) > 0
    )
  ) {
    return true;
  }

  for (const node of registration.spotNodes.values()) {
    if (
      (node.router?.manualConnections?.length ?? 0) > 0 ||
      (node.router?.manualPeerConnections?.length ?? 0) > 0
    ) {
      return true;
    }
  }

  for (const channel of registration.channels.values()) {
    if (
      (channel.client?.manualConnections?.length ?? 0) > 0 ||
      (channel.subscriber?.manualConnections?.length ?? 0) > 0
    ) {
      return true;
    }
    if (
      channel.publisher !== undefined &&
      !registration.locations.useInMemoryStores &&
      registration.locations.storeInstance === undefined
    ) {
      return true;
    }
  }

  return false;
}

function hasConfiguredLocationStore(registration: ZLinkFrameworkRegistration): boolean {
  return (
    registration.locations.useInMemoryStores || registration.locations.storeInstance !== undefined
  );
}

export function hasExactPeerReadiness(
  descriptors: readonly ZLinkMeshNodeDescriptor[],
  local: {
    readonly routingId: unknown;
    readonly lifecycleGeneration: bigint;
    readonly applicationVersion?: bigint;
    readonly maintenanceWave?: string;
  },
  peers: readonly {
    readonly routingId: unknown | null;
    readonly lifecycleGeneration: bigint;
    readonly state: number;
  }[]
): boolean {
  const replacementDescriptors = descriptors.filter(
    (descriptor) =>
      !(
        String(descriptor.rid) === String(local.routingId) &&
        descriptor.lifecycleGeneration === local.lifecycleGeneration
      ) &&
      descriptor.state === ZLinkFrameworkRuntimeState.Serving &&
      local.applicationVersion !== undefined &&
      descriptor.applicationVersion === local.applicationVersion &&
      descriptor.objectRole === ZLinkObjectRole.Server &&
      (local.maintenanceWave === undefined || descriptor.maintenanceWave !== local.maintenanceWave)
  );
  return (
    replacementDescriptors.length > 0 &&
    replacementDescriptors.some((descriptor) =>
      peers.some(
        (peer) =>
          peer.routingId !== null &&
          String(peer.routingId) === String(descriptor.rid) &&
          peer.lifecycleGeneration === descriptor.lifecycleGeneration &&
          peer.state === MeshPeerRuntimeState.Serving
      )
    )
  );
}

export {
  ZLinkActorTransferAuthorityRuntime,
  transferIdString
} from './actor-transfer-authority-runtime';
export {
  ZLinkDrainingStatePublishError,
  ZLinkRetiringRollbackError,
  ZLinkRouteMeshRuntimeCoordinator
} from './route-mesh-runtime';
export { ZLinkRelocationStateIncompatibleError } from './service-relocation-host-runtime';
export {
  ZLinkHostSpotAddressTransport,
  type ZLinkHostSpotAddressTransportOptions
} from './spot-address-transport';
export {
  ZLinkUserSpotCreationCoordinator,
  type ZLinkUserSpotCreationCoordinatorOptions,
  type ZLinkUserSpotCreationRequest,
  type ZLinkUserSpotCreationResult
} from './user-spot-creation-coordinator';

async function waitForForcedSessionNotification(
  operation: Promise<void> | undefined
): Promise<void> {
  if (operation === undefined) return;
  let timeoutHandle: ReturnType<typeof setTimeout> | undefined;
  const timeout = new Promise<void>((resolve) => {
    timeoutHandle = setTimeout(resolve, HOST_SHUTDOWN_POLL_INTERVAL_MS);
  });
  try {
    await Promise.race([operation.catch(() => undefined), timeout]);
  } finally {
    if (timeoutHandle !== undefined) clearTimeout(timeoutHandle);
  }
}

async function awaitWithDrainSignal(
  operation: Promise<void> | undefined,
  signal: AbortSignal
): Promise<void> {
  if (operation === undefined) return;
  if (signal.aborted) throw signal.reason;
  await new Promise<void>((resolve, reject) => {
    const onAbort = () => {
      reject(signal.reason);
    };
    signal.addEventListener('abort', onAbort, { once: true });
    void operation.then(
      () => {
        signal.removeEventListener('abort', onAbort);
        resolve();
      },
      (error) => {
        signal.removeEventListener('abort', onAbort);
        reject(error);
      }
    );
  });
}

function resolveBackendAdapterFactory(internalOptions: unknown): ZLinkBackendAdapterFactory {
  if (
    typeof internalOptions === 'object' &&
    internalOptions !== null &&
    'backendAdapterFactory' in internalOptions
  ) {
    const factory = (
      internalOptions as { readonly backendAdapterFactory?: ZLinkBackendAdapterFactory }
    ).backendAdapterFactory;
    if (factory !== undefined) {
      return factory;
    }
  }
  return new ZLinkNodeBackendAdapterFactory();
}

function resolveStreamPayloadCodec(
  registration: ZLinkFrameworkRegistration
): ZLinkStreamPayloadCodec | undefined {
  const codec = registration.codecs.streamCodecs.values().next().value;
  if (isStreamPayloadCodec(codec)) {
    return codec;
  }
  return undefined;
}

function runtimeStateMetricName(state: ZLinkFrameworkRuntimeState): string {
  switch (state) {
    case ZLinkFrameworkRuntimeState.Preparing:
      return 'preparing';
    case ZLinkFrameworkRuntimeState.Serving:
      return 'serving';
    case ZLinkFrameworkRuntimeState.Relocating:
      return 'relocating';
    case ZLinkFrameworkRuntimeState.Relocated:
      return 'relocated';
    case ZLinkFrameworkRuntimeState.Draining:
      return 'draining';
    case ZLinkFrameworkRuntimeState.Stopped:
      return 'stopped';
    case ZLinkFrameworkRuntimeState.Error:
      return 'error';
  }
}

function relocationModeMetricName(mode: ZLinkFrameworkRelocationMode): string {
  return mode === ZLinkFrameworkRelocationMode.RollingUpdate
    ? 'rolling_update'
    : 'planned_maintenance';
}

function relocationReasonMetricName(reason: ZLinkFrameworkRelocationReason): string {
  switch (reason) {
    case ZLinkFrameworkRelocationReason.None:
      return 'none';
    case ZLinkFrameworkRelocationReason.TargetUnavailable:
      return 'target_unavailable';
    case ZLinkFrameworkRelocationReason.StoreUnavailable:
      return 'store_unavailable';
    case ZLinkFrameworkRelocationReason.RelocationDisabled:
      return 'relocation_disabled';
    case ZLinkFrameworkRelocationReason.StateIncompatible:
      return 'state_incompatible';
    case ZLinkFrameworkRelocationReason.DeadlineExceeded:
      return 'deadline_exceeded';
    case ZLinkFrameworkRelocationReason.RelocationFailed:
      return 'relocation_failed';
    case ZLinkFrameworkRelocationReason.RuntimeNotReady:
      return 'runtime_not_ready';
    case ZLinkFrameworkRelocationReason.ManualTopologyUnsupported:
      return 'manual_topology_unsupported';
    case ZLinkFrameworkRelocationReason.ShutdownRequested:
      return 'shutdown_requested';
    case ZLinkFrameworkRelocationReason.OperationInProgress:
      return 'operation_in_progress';
  }
}

function terminationReasonMetricName(reason: ZLinkFrameworkTerminationReason): string {
  switch (reason) {
    case ZLinkFrameworkTerminationReason.None:
      return 'none';
    case ZLinkFrameworkTerminationReason.DeadlineExceeded:
      return 'deadline_exceeded';
    case ZLinkFrameworkTerminationReason.TeardownFailed:
      return 'teardown_failed';
  }
}

function isStreamPayloadCodec(codec: unknown): codec is ZLinkStreamPayloadCodec {
  return (
    typeof codec === 'object' &&
    codec !== null &&
    typeof (codec as { encode?: unknown }).encode === 'function'
  );
}

function selectLocationPlacementDescriptor(
  descriptors: readonly ZLinkMeshNodeDescriptor[]
): ZLinkMeshNodeDescriptor | undefined {
  const total = descriptors.reduce(
    (sum, descriptor) => sum + BigInt(descriptor.placementWeight),
    0n
  );
  if (total === 0n) return undefined;
  let point = randomBigIntBelow(total);
  for (const descriptor of descriptors) {
    const weight = BigInt(descriptor.placementWeight);
    if (point < weight) return descriptor;
    point -= weight;
  }
  return descriptors.at(-1);
}

async function selectReadyLocationPlacementDescriptor(
  descriptors: readonly ZLinkMeshNodeDescriptor[],
  node: ZLinkBackendMeshNode | undefined,
  signal: AbortSignal | undefined,
  isEligible: (descriptor: ZLinkMeshNodeDescriptor) => boolean
): Promise<ZLinkMeshNodeDescriptor | undefined> {
  const eligible = descriptors.filter(isEligible);
  if (eligible.length === 0) return undefined;
  for (;;) {
    const localStatus = node?.status();
    const selected = selectLocationPlacementDescriptor(
      eligible.filter((descriptor) => isPlacementTargetReady(node, localStatus, descriptor))
    );
    if (selected !== undefined) return selected;
    try {
      await waitForPlacementReadiness(signal);
    } catch {
      return undefined;
    }
  }
}

function isPlacementTargetReady(
  node: ZLinkBackendMeshNode | undefined,
  localStatus: ReturnType<ZLinkBackendMeshNode['status']> | undefined,
  descriptor: ZLinkMeshNodeDescriptor
): boolean {
  if (node === undefined) return true;
  if (
    localStatus !== undefined &&
    String(localStatus.routingId) === String(descriptor.rid) &&
    localStatus.lifecycleGeneration === descriptor.lifecycleGeneration
  ) {
    return true;
  }
  return node.isPeerRouteReady?.(descriptor.rid, descriptor.lifecycleGeneration) ?? true;
}

function waitForPlacementReadiness(signal?: AbortSignal): Promise<void> {
  if (signal?.aborted === true) return Promise.reject(signal.reason);
  return new Promise<void>((resolve, reject) => {
    const timer = setTimeout(() => {
      signal?.removeEventListener('abort', abort);
      resolve();
    }, HANDOFF_ACCEPTANCE_POLL_INTERVAL_MS);
    const abort = () => {
      clearTimeout(timer);
      signal?.removeEventListener('abort', abort);
      reject(signal?.reason);
    };
    signal?.addEventListener('abort', abort, { once: true });
  });
}

function randomBigIntBelow(exclusiveUpperBound: bigint): bigint {
  if (exclusiveUpperBound <= 0n) {
    throw new RangeError('Random selection upper bound must be positive.');
  }
  const bitLength = exclusiveUpperBound.toString(2).length;
  const byteLength = Math.ceil(bitLength / 8);
  const highBitMask = 0xff >> (byteLength * 8 - bitLength);
  for (;;) {
    const bytes = randomBytes(byteLength);
    bytes[0] = bytes[0]! & highBitMask;
    const value = BigInt(`0x${bytes.toString('hex')}`);
    if (value < exclusiveUpperBound) return value;
  }
}

function relocationReason(reason: string): ZLinkFrameworkRelocationReason {
  switch (reason) {
    case 'deadline_exceeded':
      return ZLinkFrameworkRelocationReason.DeadlineExceeded;
    case 'store_unavailable':
      return ZLinkFrameworkRelocationReason.StoreUnavailable;
    case 'target_unavailable':
      return ZLinkFrameworkRelocationReason.TargetUnavailable;
    default:
      return ZLinkFrameworkRelocationReason.RelocationFailed;
  }
}

function rewriteAuthorityPayloadForOwner(
  payload: Uint8Array,
  owner: ZLinkLocationOwnerToken
): Uint8Array {
  const applicationPayload = serviceRelocationAuthorityApplicationPayload(payload);
  const rewritten = isActorAuthorityPayload(applicationPayload)
    ? rewriteActorAuthorityOwner(applicationPayload, owner)
    : (rewriteServiceAuthorityOwner(applicationPayload, owner) ?? applicationPayload);
  return replaceServiceRelocationAuthorityApplicationPayload(payload, rewritten);
}

function validateRelocationOptions(
  options: ZLinkFrameworkRelocationOptions | undefined,
  sourceApplicationVersion: bigint
): bigint {
  if (options === undefined) {
    throw new TypeError('Relocation mode is required.');
  }
  const mode = options.mode as number;
  if (
    mode !== ZLinkFrameworkRelocationMode.PlannedMaintenance &&
    mode !== ZLinkFrameworkRelocationMode.RollingUpdate
  ) {
    throw new TypeError('Relocation mode is required.');
  }
  if (options.mode === ZLinkFrameworkRelocationMode.PlannedMaintenance) {
    if (options.targetApplicationVersion !== undefined) {
      throw new TypeError('PlannedMaintenance relocation cannot define targetApplicationVersion.');
    }
    return sourceApplicationVersion;
  }
  if (
    typeof options.targetApplicationVersion !== 'bigint' ||
    options.targetApplicationVersion <= sourceApplicationVersion
  ) {
    throw new TypeError(
      'RollingUpdate relocation requires a targetApplicationVersion greater than the source version.'
    );
  }
  return options.targetApplicationVersion;
}

function blockedRelocation(
  mode: ZLinkFrameworkRelocationMode,
  effectiveTargetApplicationVersion: bigint,
  reason: ZLinkFrameworkRelocationReason
): ZLinkFrameworkRelocationResult {
  return {
    mode,
    effectiveTargetApplicationVersion,
    outcome: ZLinkFrameworkRelocationOutcome.Blocked,
    reason
  };
}

function waitForRuntimeOperation<T>(operation: Promise<T>, signal?: AbortSignal): Promise<T> {
  if (signal === undefined) return operation;
  signal.throwIfAborted();
  return new Promise((resolve, reject) => {
    const aborted = () => reject(signal.reason);
    signal.addEventListener('abort', aborted, { once: true });
    operation.then(resolve, reject).finally(() => signal.removeEventListener('abort', aborted));
  });
}

function registeredRuntimeStores(
  registration: ZLinkFrameworkRegistration
): readonly { dispose?(): void | Promise<void> }[] {
  const stores = [
    registration.locations.storeInstance,
    registration.locations.relocationStoreInstance
  ];
  return stores.filter((store): store is NonNullable<typeof store> => store !== undefined);
}
