import type {
  ActorRef,
  RoutingId,
  Type,
  ZLinkMessageSerializer,
  ZLinkInstanceSpot,
  ZLinkActor,
  ZLinkChannelClient,
  ZLinkFanoutClient,
  ZLinkSpot,
  ZLinkSpotActorJoinResult,
  ZLinkSpotInfo,
  ZLinkSpotPublisherClient,
} from '../../contracts';
import type { ZLinkProviderResolver } from '../../contracts/Common/ZLinkProviderResolver';
import type {
  ZLinkSpotActorRequestHandlerRegistration,
  ZLinkSpotActorSendHandlerRegistration,
  ZLinkSpotPacketHandlerRegistration,
  ZLinkSpotSubscriptionHandlerRegistration,
  ZLinkSpotTimerHandlerRegistration
} from '../../contracts/Configuration/RegistrationTypes';
export { createSpotHandle, resolveSpotHandle } from './spot-handle';
export { requestToSpotHandle, sendToSpotHandle } from './spot-outbound';
export { ZLinkSpotRoutedBoundSessionDispatch } from './spot-routed-bound-session-dispatch';
export { ZLinkSpotActorAdmissionCoordinator } from './spot-actor-admission-coordinator';
import type { ZLinkSpotRouteResolver, ZLinkSpotRouteTarget } from './spot-routing-internal';
import type { Message } from '../../contracts/Common/Message';
import type { ZLinkMessageFollowOrigin } from '../foundation/service-runtime-contracts';
import { awaitWithAbort, isAbortError, throwIfAborted } from '../abort';
import {
  ZLinkMessage,
  ZLinkFrameworkException,
  ZLinkFrameworkErrorKind,
  ZLinkSpotCloseReason,
  ZLinkSpotKind
} from '../../contracts';
import {
  ZLinkRuntimeMessageFlowOutcome as ZLinkMessageFlowOutcome,
  ZLinkRuntimeDispatchErrorAction as ZLinkDispatchErrorAction,
  ZLinkRuntimeDispatchErrorReason as ZLinkDispatchErrorReason,
  ZLinkDispatchErrorSurface,
  ZLinkDispatchMessageKind
} from '../../contracts/Dispatch/ZLinkDispatchOptions';
import {
  createInboundFlow,
  flowIfEnabled,
  runWithFlow,
  type ZLinkRuntimeEventPublisher
} from '../diagnostics';
import { ZLinkBufferMessage as RuntimeMessage } from '../backend/runtime-message';
import { SubmitResult } from '../backend/runtime-values';
import {
  ActorLifecycleKind,
  OperationKind,
  ReceiveKind,
  type ReadyRecord,
  type ReceiveRecord
} from '../foundation/service-runtime-contracts';
import { zlinkMetadataByteLength, zlinkSerialWorkOptions } from '../execution/serial-work-size';
import {
  ZLinkConfigurationException
} from '../configuration';
import {
  ZLinkFrameworkInternalErrorKind,
  createInternalFrameworkException,
  internalFrameworkErrorKind
} from '../framework-errors-internal';
import type {
  ZLinkBackendSpot,
  ZLinkBackendSpotNode
} from '../backend/contracts';

import { ZLinkDispatchErrorReporter } from '../channels';
import { ZLinkWorkerRuntime } from '../workers';
import {
  type ZLinkDeferredJoinAcceptedRoot,
  type ZLinkRemoteBoundSessionTarget
} from '../actors';
import type { ZLinkLocationLifecycle } from '../locations';
import {
  encodeFrameworkPayloadMessage,
  wrapFrameworkPayloadMessage
} from '../messaging/payload-codec';
import {
  decodeChannelEnvelope,
  decodeChannelPayload,
  encodeChannelErrorReplyParts,
  encodeChannelReplyParts
} from '../channels/channel-envelope';
import {
  decodeStreamHeader,
  encodeStreamHeader,
  ZLinkStreamCodec,
  ZLinkStreamHeaderFlags,
  ZLinkStreamMessageKind
} from '../streams/protocol';
import {
  REMOTE_ACTOR_JOIN_PACKET,
  REMOTE_ACTOR_JOIN_ABORT,
  REMOTE_ACTOR_JOIN_ADMISSION,
  REMOTE_ACTOR_JOIN_COMMIT,
  type ZLinkRemoteActorJoinWirePayload
} from '../actors/actor-remote-wire';
import { replayActorHandoffBacklog, type ZLinkActorHandoffPacket } from '../actors/actor-handoff';
import {
  decodeHandoffBacklog,
  decodeRemoteActorRef,
  decodeRemoteBoundSessionTarget
} from './spot-remote-codec';
import { decodeRoutingId } from '../routing-id';

export { ZLinkSpotSerialExecutor } from './spot-serial-executor';
export {
  ZLinkManagedTimer,
  ZLinkSpotTimerRegistry
} from './spot-timer';
export {
  DefaultZLinkSpotOutbound,
  type ZLinkSpotAddressCallOptions,
  type ZLinkSpotAddressTransport,
  type ZLinkSpotRoutedRequestOptions,
  type ZLinkSpotRoutedSendOptions,
  type ZLinkSpotRoutedTransport
} from './spot-outbound';
import {
  type ZLinkSpotRoutedTransport
} from './spot-outbound';
export {
  DefaultZLinkSpotHandlerRegistry,
  type ZLinkSpotHandlerRegistration
} from './spot-handler-registry';
export { ZLinkRuntimeSpotPublisherTransport } from './spot-publisher-transport';
import {
  ZLinkSpotActivationRegistry
} from './spot-activation-registry';
import type { ZLinkSpotActivation } from './spot-activation-state';
import {
  ZLinkSpotActivationLifecycle,
  type ZLinkNativeSpotAuthority
} from './spot-activation';
import { ZLinkSpotActorMembership, type ZLinkActorJoinRollback } from './spot-actor-membership';
import { ZLinkFormalRemoteActorTransferRegistry } from './formal-remote-actor-transfer-registry';
import {
  ZLinkFormalRemoteActorAdmissionRegistry,
  type ZLinkFormalRemoteActorAdmissionResult,
  type ZLinkFormalRemoteActorAdmissionRecord
} from './formal-remote-actor-admission-registry';
import type {
  ZLinkSpotActorHandoffRuntime,
  ZLinkSpotActorTransferRuntime,
  ZLinkSpotBoundSessionRuntime
} from './spot-runtime-ports';
import type { ZLinkRuntimeAdmissionGate } from '../admission';
import type { ZLinkDetachedTaskRunner } from './spot-actor-join-dispatch';
import type { ZLinkLocalSpotCreateResult } from './spot-manager-internal-contracts';
export type { ZLinkLocalSpotCreateResult } from './spot-manager-internal-contracts';
export type { ZLinkDetachedTaskRunner } from './spot-actor-join-dispatch';
import { ZLinkSpotLocationClaim } from './spot-location-claim';
import { ZLinkRoutedSpotPacketDispatch } from './spot-routed-spot-packet-dispatch';
export { ZLinkEntrySpotActivation } from './spot-entry-activation';
export {
  createFrameworkEntrySpotId,
  ZLinkSpotNodeRuntimeManager,
  type ZLinkSpotNodeRuntimeManagerOptions
} from './spot-node-runtime-manager';
export {
  ZLinkPublicSpotManager,
  type ZLinkPublicSpotManagerOptions
} from './spot-manager-public';

export interface ZLinkSpotManagerOptions {
  readonly spotFactories: readonly Type<ZLinkSpot>[];
  readonly instanceSpotFactories?: ReadonlyMap<
    string,
    ReadonlyMap<string, Type<ZLinkInstanceSpot>>
  >;
  readonly instanceSpotIdleTimeoutMs?: ReadonlyMap<string, number>;
  readonly spotTimerHandlers?: readonly ZLinkSpotTimerHandlerRegistration[];
  readonly spotPacketHandlers?: readonly ZLinkSpotPacketHandlerRegistration[];
  readonly spotSubscriptionHandlers?: readonly ZLinkSpotSubscriptionHandlerRegistration[];
  readonly spotActorSendHandlers?: readonly ZLinkSpotActorSendHandlerRegistration[];
  readonly spotActorRequestHandlers?: readonly ZLinkSpotActorRequestHandlerRegistration[];
  readonly nodeRid?: RoutingId;
  readonly nodeRidProvider?: (meshName: string) => RoutingId | undefined;
  readonly nodeGenerationProvider?: (meshName: string) => bigint | undefined;
  readonly entryNodeRid?: RoutingId;
  readonly entryNodeRidProvider?: () => RoutingId | undefined;
  readonly entrySpotIdProvider?: (meshName: string) => string | undefined;
  readonly entrySpotCallbacks?: {
    onLeaveActor(
      actor: ZLinkActor,
      signal?: AbortSignal,
      actorRef?: ActorRef,
      membershipEpoch?: bigint
    ): Promise<void>;
  };
  readonly dispatchEntryActorPacket?: (
    actorId: string,
    parts: readonly Message[],
    returnResponse?: boolean,
    remoteBoundSessionTarget?: ZLinkRemoteBoundSessionTarget,
    fallbackActorRef?: ActorRef,
    requestTerminal?: (response: unknown) => Promise<void> | void,
    messageFollowOrigin?: ZLinkMessageFollowOrigin
  ) => Promise<unknown>;
  /**
   * Commits a stateful MeshNode Actor return to the Entry Spot after the
   * stateful reply has committed Core membership.
   */
  readonly dispatchEntryActorJoin?: (
    meshName: string,
    actor: ZLinkActor,
    handoffBacklog?: readonly ZLinkActorHandoffPacket[]
  ) => Promise<void>;
  readonly actorCountProvider?: (spotId: RoutingId) => number;
  readonly userSpotExecutionMode?: (
    meshName: string,
    spotType: Type<ZLinkSpot>
  ) => import('../../contracts').ZLinkUserSpotExecutionMode;
  readonly userSpotRelocationReadiness?: (
    meshName: string,
    spotType: Type<ZLinkSpot>
  ) => import('../../contracts').ZLinkSpotRelocationReadinessMode;
  readonly actorDispatchOwnerResolver?: (actorId: string) => {
    readonly actorRef?: ActorRef;
    readonly spotId?: RoutingId;
  };
  readonly actorBindingGenerationObserver?: (actorId: string, generation: bigint) => void;
  readonly channelClient?: ZLinkChannelClient;
  readonly fanoutClient?: ZLinkFanoutClient;
  readonly spotPublisherClient?: ZLinkSpotPublisherClient;
  readonly spotRouteResolver?: ZLinkSpotRouteResolver;
  readonly routedTransport?: ZLinkSpotRoutedTransport;
  readonly addressTransport?: import('./spot-outbound').ZLinkSpotAddressTransport;
  readonly spotRouterChannelIdForMesh?: (meshName: string) => string;
  readonly channelMeshNameForChannel?: (channelName: string) => string | undefined;
  readonly providerResolver?: ZLinkProviderResolver;
  readonly dispatchErrors?: ZLinkDispatchErrorReporter;
  readonly runtimeEventPublisher?: ZLinkRuntimeEventPublisher;
  readonly workerRuntime?: ZLinkWorkerRuntime;
  readonly messageSerializers?: ReadonlyMap<string, ZLinkMessageSerializer>;
  readonly locationLifecycle?: ZLinkLocationLifecycle;
  readonly releaseInstanceAuthority?: (
    meshName: string,
    spotId: RoutingId,
    objectGeneration: bigint
  ) => Promise<void>;
  readonly beginInstanceIdleClosingAuthority?: (
    meshName: string,
    spotId: RoutingId
  ) => Promise<boolean>;
  readonly beginInstanceClosingAuthority?: (
    meshName: string,
    spotId: RoutingId
  ) => Promise<boolean>;
  readonly instanceSpotApplicationTargetProvider?: (
    meshName: string,
    spotId: RoutingId
  ) => { readonly stableType: string; readonly objectGeneration: bigint } | undefined;
  readonly instanceSpotApplicationQuiescenceProvider?: (
    meshName: string,
    spotId: RoutingId,
    signal?: AbortSignal
  ) => Promise<void>;
  // Backs each user Spot with a core-native Spot object (registered for join
  // routing by rid) so actor-join admission uses the same recv/reply round-trip
  // as the Entry Spot and .NET, for local and remote callers alike.
  readonly createNativeSpot?: (
    meshName: string,
    spotId: RoutingId,
    authority?: ZLinkNativeSpotAuthority
  ) => ZLinkBackendSpot | undefined;
  readonly createReceived?: () => import('../backend').ZLinkBackendReceived;
  readonly createTopicMessage?: () => import('../backend').ZLinkBackendTopicMessage;
  readonly nativeSpotNodeProvider?: (meshName: string) => ZLinkBackendSpotNode | undefined;
  readonly actorResolver?: (actorId: string) => ZLinkActor | undefined;
  readonly actorLifecycleResolver?: (actorId: string) => ZLinkActor | undefined;
  readonly detachedTaskRunner?: ZLinkDetachedTaskRunner;
  readonly actorTransferRuntime?: ZLinkSpotActorTransferRuntime;
  readonly boundSessionRuntime?: ZLinkSpotBoundSessionRuntime;
  readonly actorHandoffRuntime?: ZLinkSpotActorHandoffRuntime;
  readonly metrics?: import('../diagnostics').ZLinkRuntimeMetrics;
  readonly admission?: ZLinkRuntimeAdmissionGate;
}

export class DefaultZLinkSpotManager {
  private readonly factories: ReadonlySet<Type<ZLinkSpot>>;
  private readonly activations: ZLinkSpotActivationRegistry;
  private readonly workerRuntime: ZLinkWorkerRuntime;
  private readonly locationClaim: ZLinkSpotLocationClaim;
  private readonly routedSpotPackets: ZLinkRoutedSpotPacketDispatch;
  private readonly actorMembership: ZLinkSpotActorMembership;
  private readonly activationLifecycle: ZLinkSpotActivationLifecycle;
  private readonly formalRemoteTransfers = new ZLinkFormalRemoteActorTransferRegistry();
  private readonly formalRemoteActorAdmissions = new ZLinkFormalRemoteActorAdmissionRegistry();
  private readonly pendingInstanceMaterializations = new Map<
    string,
    Promise<ZLinkSpotActivation>
  >();
  private readonly pendingInstanceCloses = new Map<string, Promise<boolean>>();
  private readonly pendingInstanceTerminals = new Map<string, number>();
  private readonly pendingInstanceTerminalGenerations = new Map<string, Map<string, number>>();
  private readonly deferredInstanceAuthorityReleases = new Map<string, Set<bigint>>();
  private idleSweepTimer?: ReturnType<typeof setTimeout>;
  private idleSweepRunning = false;

  constructor(private readonly options: ZLinkSpotManagerOptions) {
    this.activations = new ZLinkSpotActivationRegistry(options.metrics);
    this.factories = new Set(options.spotFactories);
    this.workerRuntime = options.workerRuntime ?? new ZLinkWorkerRuntime();
    this.locationClaim = new ZLinkSpotLocationClaim({
      lifecycle: options.locationLifecycle,
      nodeRid: options.nodeRid,
      nodeRidProvider: options.nodeRidProvider,
      nodeGenerationProvider: options.nodeGenerationProvider
    });
    this.routedSpotPackets = new ZLinkRoutedSpotPacketDispatch({
      resolveActivation: (spotId) => this.activations.resolveUnique(spotId),
      claimApplicationWork: options.admission === undefined
        ? undefined
        : (meshName) => options.admission!.claim(meshName, 'Spot route dispatch'),
      providerResolver: options.providerResolver,
      dispatchErrors: options.dispatchErrors
    });
    this.actorMembership = new ZLinkSpotActorMembership({
      resolveActivation: (spotId, meshName) => meshName === undefined
        ? this.activations.resolveUnique(spotId)
        : this.activations.resolve(meshName, spotId),
      providerResolver: options.providerResolver,
      messageSerializers: options.messageSerializers,
      dispatchErrors: options.dispatchErrors,
      entrySpotCallbacks: options.entrySpotCallbacks,
      nodeRid: options.nodeRid,
      nodeRidProvider: options.nodeRidProvider,
      entryNodeRid: options.entryNodeRid,
      entryNodeRidProvider: options.entryNodeRidProvider,
      entrySpotIdProvider: options.entrySpotIdProvider,
      spotRouteResolver: options.spotRouteResolver,
      actorTransferRuntime: options.actorTransferRuntime
    });
    this.activationLifecycle = new ZLinkSpotActivationLifecycle({
      spotTimerHandlers: options.spotTimerHandlers,
      spotPacketHandlers: options.spotPacketHandlers,
      spotSubscriptionHandlers: options.spotSubscriptionHandlers,
      spotActorSendHandlers: options.spotActorSendHandlers,
      spotActorRequestHandlers: options.spotActorRequestHandlers,
      nodeRid: options.nodeRid,
      nodeRidProvider: options.nodeRidProvider,
      actorCountProvider: options.actorCountProvider,
      userSpotExecutionMode: options.userSpotExecutionMode,
      userSpotRelocationReadiness: options.userSpotRelocationReadiness,
      channelClient: options.channelClient,
      fanoutClient: options.fanoutClient,
      spotPublisherClient: options.spotPublisherClient,
      routedTransport: options.routedTransport,
      addressTransport: options.addressTransport,
      spotRouterChannelIdForMesh: options.spotRouterChannelIdForMesh,
      providerResolver: options.providerResolver,
      dispatchErrors: options.dispatchErrors,
      runtimeEventPublisher: options.runtimeEventPublisher,
      workerRuntime: this.workerRuntime,
      messageSerializers: options.messageSerializers,
      locationClaim: this.locationClaim,
      createNativeSpot: options.createNativeSpot,
      createReceived: options.createReceived,
      createTopicMessage: options.createTopicMessage,
      nativeSpotNodeProvider: options.nativeSpotNodeProvider,
      actorResolver: options.actorResolver,
      detachedTaskRunner: options.detachedTaskRunner,
      actorTransferRuntime: options.actorTransferRuntime,
      boundSessionRuntime: options.boundSessionRuntime,
      actorHandoffRuntime: options.actorHandoffRuntime,
      admission: options.admission,
      leaveActor: (spotId, actor, signal, meshName) =>
        this.actorMembership.leaveActor(spotId, actor, signal, meshName),
      closeSpot: (meshName, spotId, signal, reason) =>
        this.closeWithReason(meshName, spotId, signal, reason),
      registerActivation: (activation) => {
        this.activations.register(activation);
        this.scheduleIdleSweep();
      },
      releaseLocation: (activation, meshName, spotId) => {
        if (!this.isInstanceFactory(meshName, activation.spotType)) {
          return this.locationClaim.release(meshName, spotId);
        }
        const currentApplication = this.options.instanceSpotApplicationTargetProvider?.(
          meshName,
          spotId
        );
        if (
          activation.objectGeneration !== undefined
          && currentApplication?.objectGeneration !== activation.objectGeneration
        ) {
          // A superseded local application must not release the authority row
          // that now belongs to the newer object generation.
          return Promise.resolve();
        }
        const key = `${meshName}\0${String(spotId)}`;
        const objectGeneration = activation.objectGeneration;
        if (objectGeneration === undefined) {
          return Promise.resolve();
        }
        if (this.pendingInstanceTerminals.has(key)) {
          this.deferInstanceAuthorityRelease(key, objectGeneration);
          return Promise.resolve();
        }
        this.removeDeferredInstanceAuthorityRelease(key, objectGeneration);
        return this.releaseInstanceAuthority(meshName, spotId, objectGeneration);
      },
      metrics: options.metrics
    });
    this.scheduleIdleSweep();
  }

  entrySpotIdForMesh(meshName: string): string | undefined {
    return this.options.entrySpotIdProvider?.(meshName);
  }

  relocationActivations(meshName: string): readonly ZLinkSpotActivation[] {
    return this.activations.activeActivations().filter(activation =>
      activation.meshName === meshName
      && this.activations.resolve(meshName, activation.spotId) === activation);
  }

  activeSpotCount(meshName: string): number {
    return this.activations.list(meshName).length;
  }

  resolveRelocationActivation(
    meshName: string,
    spotId: RoutingId
  ): ZLinkSpotActivation | undefined {
    return this.activations.resolve(meshName, spotId);
  }

  async prepareRelocationSpot(
    meshName: string,
    objectKind: 'user_spot' | 'instance_spot',
    stableType: string,
    implementation: Type<ZLinkSpot | ZLinkInstanceSpot>,
    spotId: RoutingId,
    objectGeneration: bigint,
    authorityOwnerGeneration: bigint,
    signal?: AbortSignal
  ): Promise<ZLinkSpotActivation> {
    this.activations.stage(meshName, spotId);
    try {
      return await this.activationLifecycle.materializeRelocation(
        meshName,
        objectKind,
        stableType,
        implementation,
        spotId,
        objectGeneration,
        authorityOwnerGeneration,
        signal
      );
    } catch (error) {
      this.activations.abandonStage(meshName, spotId);
      throw error;
    }
  }

  async publishRelocationSpot(
    activation: ZLinkSpotActivation,
    beforeAdmission?: () => Promise<void>
  ): Promise<void> {
    await activation.serial.execute(() => activation.spot.onInitialize?.());
    await beforeAdmission?.();
    this.activations.publish(activation.meshName, activation.spotId);
  }

  async abortRelocationSpot(activation: ZLinkSpotActivation): Promise<void> {
    this.activations.detachRelocated(activation.meshName, activation.spotId);
    await activation.timers.dispose();
    await activation.nativeSpot?.dispose();
  }

  async completeRelocationSource(
    activation: ZLinkSpotActivation
  ): Promise<void> {
    this.activations.detachRelocated(activation.meshName, activation.spotId);
    await activation.nativeSpot?.dispose();
  }

  async materializeInstance(
    meshName: string,
    instanceType: string,
    spotId: RoutingId,
    objectGeneration: bigint,
    signal?: AbortSignal
  ): Promise<void> {
    const factory = this.requireInstanceFactory(meshName, instanceType);
    const key = `${meshName}\0${String(spotId)}`;
    const materializationKey = instanceMaterializationKey(meshName, spotId, objectGeneration);
    const materializationPrefix = instanceMaterializationPrefix(meshName, spotId);
    for (;;) {
      // A close seals admission before cleanup removes the activation. An
      // explicit Instance intent arriving at that boundary must wait for the
      // old resources to be released before materializing the replacement.
      // This is lifecycle convergence; it does not resubmit application work.
      const pendingClose = this.pendingInstanceCloses.get(key);
      if (pendingClose !== undefined) {
        await awaitWithAbort(pendingClose, signal);
        continue;
      }
      const closing = this.activations.closingOperation(meshName, spotId);
      if (closing !== undefined) {
        await awaitWithAbort(closing.ready, signal);
        continue;
      }

      const current = this.activations.resolve(meshName, spotId);
      if (current !== undefined) {
        if (current.spotType !== factory) {
          throw new ZLinkConfigurationException(
            `Instance Spot '${String(spotId)}' is assigned to another type.`
          );
        }
        if (current.objectGeneration === objectGeneration) return;
        if (
          current.objectGeneration === undefined
          || current.objectGeneration > objectGeneration
        ) {
          throw createInternalFrameworkException(
            ZLinkFrameworkInternalErrorKind.SpotGenerationStale,
            `Instance Spot '${String(spotId)}' has a newer application generation.`
          );
        }
        // A newer authority route may become visible while the previous
        // application factory is still completing. Dispose that older
        // activation before publishing the requested generation. The
        // generation-aware release callback prevents this cleanup from
        // releasing the newer authority row.
        await this.discardInstance(meshName, spotId);
        continue;
      }

      const lifecycleActivation = this.activations.activeActivations().find((activation) =>
        activation.meshName === meshName && String(activation.spotId) === String(spotId)
      );
      if (lifecycleActivation?.isIdleEvicting === true) {
        throw createInternalFrameworkException(
          ZLinkFrameworkInternalErrorKind.SpotGenerationStale,
          `Instance Spot '${String(spotId)}' is entering idle eviction.`
        );
      }

      let pending = this.pendingInstanceMaterializations.get(materializationKey);
      if (pending === undefined) {
        const otherPending = [...this.pendingInstanceMaterializations.entries()]
          .find(([pendingKey]) => pendingKey.startsWith(`${materializationPrefix}\0`))?.[1];
        if (otherPending !== undefined) {
          await awaitWithAbort(otherPending, signal);
          continue;
        }
      }
      if (pending === undefined) {
        const metric = this.options.metrics?.startInstanceSpotActivation(meshName, instanceType);
        pending = this.activationLifecycle.materializeInstance(
            meshName,
            instanceType,
            factory,
            spotId,
            objectGeneration,
            signal
          ).then(
            activation => {
              metric?.complete('ready');
              return activation;
            },
            error => {
              metric?.complete(instanceActivationOutcome(error));
              throw error;
            }
          );
        this.pendingInstanceMaterializations.set(materializationKey, pending);
        void pending.finally(() => {
          if (this.pendingInstanceMaterializations.get(materializationKey) === pending) {
            this.pendingInstanceMaterializations.delete(materializationKey);
          }
        }).catch(() => undefined);
      }
      await pending;
      return;
    }
  }

  isInstanceMaterialized(
    meshName: string,
    instanceType: string,
    spotId: RoutingId
  ): boolean {
    const activation = this.activations.resolve(meshName, spotId);
    return activation !== undefined
      && activation.spotType === this.requireInstanceFactory(meshName, instanceType);
  }

  isInstanceMaterializing(meshName: string, spotId: RoutingId): boolean {
    const prefix = instanceMaterializationPrefix(meshName, spotId);
    return [...this.pendingInstanceMaterializations.keys()]
      .some(key => key.startsWith(`${prefix}\0`));
  }

  isInstanceClosing(meshName: string, spotId: RoutingId): boolean {
    const key = `${meshName}\0${String(spotId)}`;
    return this.pendingInstanceCloses.has(key)
      || this.activations.closingOperation(meshName, spotId) !== undefined;
  }

  async discardInstance(meshName: string, spotId: RoutingId): Promise<void> {
    const activation = this.activations.resolve(meshName, spotId);
    if (activation === undefined) return;
    activation.requestClose();
    const operation = this.activations.startClose(
      meshName,
      spotId,
      (current) => this.activationLifecycle.discardInstance(current),
      () => true
    );
    await operation?.ready;
  }

  isInstanceSpotIdleEvicting(meshName: string, spotId: RoutingId): boolean {
    const activation = this.activations.activeActivations().find((candidate) =>
      candidate.meshName === meshName && String(candidate.spotId) === String(spotId)
    );
    return activation !== undefined
      && this.isInstanceFactory(meshName, activation.spotType)
      && activation.isIdleEvicting;
  }

  beginInstanceSpotIdleEviction(meshName: string, spotId: RoutingId): boolean {
    const activation = this.activations.activeActivations().find((candidate) =>
      candidate.meshName === meshName && String(candidate.spotId) === String(spotId)
    );
    if (
      activation === undefined
      || !this.isInstanceFactory(meshName, activation.spotType)
      || !activation.isIdleFor(
        Date.now(),
        this.options.instanceSpotIdleTimeoutMs?.get(meshName) ?? 0
      )
    ) {
      return false;
    }
    return activation.beginIdleEviction();
  }

  async completeInstanceTerminal(
    meshName: string,
    spotId: RoutingId,
    objectGeneration: bigint
  ): Promise<boolean> {
    const key = `${meshName}\0${String(spotId)}`;
    const pending = this.pendingInstanceTerminals.get(key);
    if (pending === undefined) return false;
    this.removePendingInstanceTerminalGeneration(key, objectGeneration);
    if (pending > 1) {
      this.pendingInstanceTerminals.set(key, pending - 1);
      return false;
    }
    this.pendingInstanceTerminals.delete(key);
    this.pendingInstanceTerminalGenerations.delete(key);
    const pendingClose = this.pendingInstanceCloses.get(key);
    if (pendingClose !== undefined) {
      this.deferInstanceAuthorityRelease(key, objectGeneration);
      await pendingClose;
      return true;
    }
    const deferredRelease = this.deferredInstanceAuthorityReleases.get(key)?.has(objectGeneration) === true;
    if (
      !deferredRelease
      && this.activations.closingOperation(meshName, spotId) === undefined
    ) {
      return false;
    }
    const currentApplication = this.options.instanceSpotApplicationTargetProvider?.(meshName, spotId);
    if (
      currentApplication !== undefined
      && currentApplication.objectGeneration !== objectGeneration
    ) {
      this.removeDeferredInstanceAuthorityRelease(key, objectGeneration);
      return false;
    }
    this.removeDeferredInstanceAuthorityRelease(key, objectGeneration);
    await this.releaseInstanceAuthority(meshName, spotId, objectGeneration);
    return true;
  }

  beginInstanceTerminal(meshName: string, spotId: RoutingId, objectGeneration: bigint): void {
    const key = `${meshName}\0${String(spotId)}`;
    this.pendingInstanceTerminals.set(
      key,
      (this.pendingInstanceTerminals.get(key) ?? 0) + 1
    );
    const generations = this.pendingInstanceTerminalGenerations.get(key) ?? new Map<string, number>();
    const generationKey = objectGeneration.toString();
    generations.set(generationKey, (generations.get(generationKey) ?? 0) + 1);
    this.pendingInstanceTerminalGenerations.set(key, generations);
  }

  private deferInstanceAuthorityRelease(key: string, objectGeneration: bigint): void {
    const generations = this.deferredInstanceAuthorityReleases.get(key) ?? new Set<bigint>();
    generations.add(objectGeneration);
    this.deferredInstanceAuthorityReleases.set(key, generations);
  }

  private removeDeferredInstanceAuthorityRelease(key: string, objectGeneration: bigint): void {
    const generations = this.deferredInstanceAuthorityReleases.get(key);
    if (generations === undefined) return;
    generations.delete(objectGeneration);
    if (generations.size === 0) this.deferredInstanceAuthorityReleases.delete(key);
  }

  private removePendingInstanceTerminalGeneration(key: string, objectGeneration: bigint): void {
    const generations = this.pendingInstanceTerminalGenerations.get(key);
    if (generations === undefined) return;
    const generationKey = objectGeneration.toString();
    const pending = generations.get(generationKey);
    if (pending === undefined || pending <= 1) generations.delete(generationKey);
    else generations.set(generationKey, pending - 1);
  }

  private releaseInstanceAuthority(
    meshName: string,
    spotId: RoutingId,
    objectGeneration: bigint
  ): Promise<void> {
    return this.options.releaseInstanceAuthority?.(meshName, spotId, objectGeneration)
      ?? Promise.resolve();
  }

  private requireInstanceFactory(
    meshName: string,
    instanceType: string
  ): Type<ZLinkInstanceSpot> {
    const factory = this.options.instanceSpotFactories?.get(meshName)?.get(instanceType);
    if (factory === undefined) {
      throw new ZLinkConfigurationException(
        `Instance Spot factory '${instanceType}' is not registered on RouteMesh '${meshName}'.`
      );
    }
    return factory;
  }

  private isInstanceFactory(
    meshName: string,
    implementation: Type<ZLinkSpot>
  ): boolean {
    return [...(this.options.instanceSpotFactories?.get(meshName)?.values() ?? [])]
      .some(factory => factory === implementation);
  }

  private scheduleIdleSweep(): void {
    if (this.idleSweepTimer !== undefined) return;
    if (!this.hasIdleSweepTarget()) return;
    const delays = [...(this.options.instanceSpotIdleTimeoutMs?.values() ?? [])]
      .filter((value) => value > 0);
    if (delays.length === 0) return;
    const delay = Math.max(1, Math.min(...delays));
    const timer = setTimeout(() => {
      this.idleSweepTimer = undefined;
      void this.runIdleSweep();
    }, delay);
    timer.unref();
    this.idleSweepTimer = timer;
  }

  private hasIdleSweepTarget(): boolean {
    return this.activations.activeActivations().some((activation) =>
      (this.options.instanceSpotIdleTimeoutMs?.get(activation.meshName) ?? 0) > 0
      && this.isInstanceFactory(activation.meshName, activation.spotType)
    );
  }

  private async runIdleSweep(): Promise<void> {
    if (this.idleSweepRunning) return;
    this.idleSweepRunning = true;
    try {
      const now = Date.now();
      for (const activation of this.activations.activeActivations()) {
        const timeoutMs = this.options.instanceSpotIdleTimeoutMs?.get(activation.meshName) ?? 0;
        if (
          timeoutMs <= 0
          || !this.isInstanceFactory(activation.meshName, activation.spotType)
          || !activation.isIdleFor(now, timeoutMs)
          || !activation.beginIdleEviction()
        ) {
          continue;
        }
        let durableClosing = false;
        try {
          durableClosing = await (
            this.options.beginInstanceIdleClosingAuthority?.(
              activation.meshName,
              activation.spotId
            ) ?? Promise.resolve(true)
          );
        } catch (error) {
          activation.abortIdleEviction();
          throw error;
        }
        if (!durableClosing) {
          activation.abortIdleEviction();
          continue;
        }
        const close = this.closeWithReason(
          activation.meshName,
          activation.spotId,
          undefined,
          ZLinkSpotCloseReason.IdleEvicted
        );
        const run = async () => {
          try {
            const closed = await close;
            if (!closed) activation.abortIdleEviction();
          } catch (error) {
            activation.abortIdleEviction();
            throw error;
          }
        };
        this.options.detachedTaskRunner?.runDetached(
          `instance idle eviction ${String(activation.spotId)}`,
          run
        );
        if (this.options.detachedTaskRunner === undefined) {
          void run().catch(() => undefined);
        }
      }
    } finally {
      this.idleSweepRunning = false;
      this.scheduleIdleSweep();
    }
  }

  async create<TSpot extends ZLinkSpot>(
    meshName: string,
    spotType: Type<TSpot>,
    signal?: AbortSignal
  ): Promise<ZLinkLocalSpotCreateResult>;
  async create<TSpot extends ZLinkSpot>(
    meshName: string,
    spotType: Type<TSpot>,
    request: ZLinkMessage,
    signal?: AbortSignal
  ): Promise<ZLinkLocalSpotCreateResult>;
  async create<TSpot extends ZLinkSpot, TRequest>(
    meshName: string,
    spotType: Type<TSpot>,
    request: TRequest,
    signal?: AbortSignal
  ): Promise<ZLinkLocalSpotCreateResult>;
  async create<TSpot extends ZLinkSpot, TRequest>(
    meshName: string,
    spotType: Type<TSpot>,
    requestOrSignal?: ZLinkMessage | TRequest | AbortSignal,
    signal?: AbortSignal
  ): Promise<ZLinkLocalSpotCreateResult> {
    requireMeshName(meshName);
    const args = normalizeSpotCreateArgs(requestOrSignal, signal);
    this.options.admission?.requireRequest('SPOT create', meshName);
    const spotId = this.activations.allocateSpotId(meshName);
    const ownedRequest = args.request === undefined
      ? RuntimeMessage.from(Buffer.alloc(0))
      : encodeFrameworkPayloadMessage(args.request, this.options.messageSerializers);
    try {
      return await this.createActivation(meshName, spotType, spotId, ownedRequest, args.signal);
    } finally {
      ownedRequest.close();
    }
  }

  async getOrCreate<TSpot extends ZLinkSpot>(
    meshName: string,
    spotType: Type<TSpot>,
    spotId: RoutingId,
    signal?: AbortSignal
  ): Promise<ZLinkLocalSpotCreateResult>;
  async getOrCreate<TSpot extends ZLinkSpot>(
    meshName: string,
    spotType: Type<TSpot>,
    spotId: RoutingId,
    request: ZLinkMessage,
    signal?: AbortSignal
  ): Promise<ZLinkLocalSpotCreateResult>;
  async getOrCreate<TSpot extends ZLinkSpot, TRequest>(
    meshName: string,
    spotType: Type<TSpot>,
    spotId: RoutingId,
    request: TRequest,
    signal?: AbortSignal
  ): Promise<ZLinkLocalSpotCreateResult>;
  async getOrCreate<TSpot extends ZLinkSpot, TRequest>(
    meshName: string,
    spotType: Type<TSpot>,
    spotId: RoutingId,
    requestOrSignal?: ZLinkMessage | TRequest | AbortSignal,
    signal?: AbortSignal
  ): Promise<ZLinkLocalSpotCreateResult> {
    return await this.getOrCreateWithAuthority(
      meshName,
      spotType,
      spotId,
      requestOrSignal,
      undefined,
      signal
    );
  }

  async getOrCreateWithAuthority<TSpot extends ZLinkSpot, TRequest>(
    meshName: string,
    spotType: Type<TSpot>,
    spotId: RoutingId,
    requestOrSignal: ZLinkMessage | TRequest | AbortSignal | undefined,
    authority: ZLinkNativeSpotAuthority | undefined,
    signal?: AbortSignal
  ): Promise<ZLinkLocalSpotCreateResult> {
    requireMeshName(meshName);
    const args = normalizeSpotCreateArgs(requestOrSignal, signal);
    throwIfAborted(args.signal);
    const operation = this.activations.getOrBegin(meshName, spotType, spotId, async () => {
      this.options.admission?.requireRequest('SPOT create', meshName);
      const ownedRequest = args.request === undefined
        ? RuntimeMessage.from(Buffer.alloc(0))
        : encodeFrameworkPayloadMessage(args.request, this.options.messageSerializers);
      try {
        return await this.createActivation(
          meshName,
          spotType,
          spotId,
          ownedRequest,
          undefined,
          authority
        );
      } finally {
        ownedRequest.close();
      }
    });
    return await awaitWithAbort(operation.ready, args.signal);
  }

  async find(meshName: string, spotId: RoutingId): Promise<ZLinkSpotInfo | null> {
    requireMeshName(meshName);
    return this.activations.has(meshName, spotId) ? { spotId } : null;
  }

  async list(meshName: string): Promise<readonly ZLinkSpotInfo[]> {
    requireMeshName(meshName);
    return this.activations.list(meshName);
  }

  async drainForShutdown(meshName: string, signal?: AbortSignal): Promise<void> {
    for (const activation of this.activations.activeActivations()) {
      if (activation.meshName !== meshName) continue;
      activation.requestDrainClose(ZLinkSpotCloseReason.HostShutdown);
    }
    await this.activations.whenMeshEmpty(meshName, signal);
  }

  async close(meshName: string, spotId: RoutingId, signal?: AbortSignal): Promise<boolean> {
    return await this.closeWithReason(
      meshName,
      spotId,
      signal,
      ZLinkSpotCloseReason.ExplicitClose
    );
  }

  private async closeWithReason(
    meshName: string,
    spotId: RoutingId,
    signal?: AbortSignal,
    reason = ZLinkSpotCloseReason.ExplicitClose
  ): Promise<boolean> {
    requireMeshName(meshName);
    const key = `${meshName}\0${String(spotId)}`;
    const closing = this.activations.closingOperation(meshName, spotId);
    if (closing !== undefined) {
      return await closing.ready;
    }
    const pendingClose = this.pendingInstanceCloses.get(key);
    if (pendingClose !== undefined) {
      const pendingActivation = this.activations.resolve(meshName, spotId);
      if (pendingActivation?.serial.isCurrentTurn === true) return true;
      return await pendingClose;
    }
    const activation = this.activations.resolve(meshName, spotId);
    if (activation === undefined) {
      return false;
    }
    const currentTurn = activation.serial.isCurrentTurn;
    const isInstance = this.isInstanceFactory(meshName, activation.spotType);
    const beginClose = () => {
      const seal = activation.sealExecution();
      const operation = this.activations.startClose(
        meshName,
        spotId,
        (target) => this.activationLifecycle.closeAfterSeal(target, seal, signal, reason),
        (target) => this.activationLifecycle.resourcesReleased(target)
      );
      if (operation === undefined) {
        activation.abortExecutionSeal(seal);
      }
      return operation;
    };

    if (isInstance) {
      const beginAuthorityClose = reason === ZLinkSpotCloseReason.ExplicitClose
        ? this.options.beginInstanceClosingAuthority
        : undefined;
      // Register the close gate before waiting for the current application
      // and activation-authority terminal completions. A close requested from
      // a handler cannot publish Closing until that handler's durable inbox
      // completion has removed the recovery fence.
      const trackedClosePromise = (async () => {
        const waitForApplication = this.options.instanceSpotApplicationQuiescenceProvider?.(
          meshName,
          spotId,
          signal
        );
        if (waitForApplication !== undefined) {
          await waitForApplication;
        }
        if (
          beginAuthorityClose !== undefined
          && !await beginAuthorityClose(meshName, spotId)
        ) {
          return false;
        }
        const operation = beginClose();
        if (operation === undefined) return false;
        return await operation.ready;
      })().finally(() => {
        if (this.pendingInstanceCloses.get(key) === trackedClosePromise) {
          this.pendingInstanceCloses.delete(key);
        }
      });
      this.pendingInstanceCloses.set(key, trackedClosePromise);
      if (currentTurn) {
        // The current turn must return before its terminal completion can
        // release the activation recovery fence and allow the durable close
        // CAS. The registered close gate prevents a second admission while
        // that detached close finishes.
        this.options.detachedTaskRunner?.runDetached(
          `instance spot close ${String(spotId)}`,
          async () => { await trackedClosePromise; }
        );
        if (this.options.detachedTaskRunner === undefined) {
          void trackedClosePromise.catch(() => undefined);
        }
        return true;
      }
      return await trackedClosePromise;
    }

    const operation = beginClose();
    if (operation === undefined) {
      return false;
    }
    if (currentTurn) {
      if (operation.started) {
        this.options.detachedTaskRunner?.runDetached(
          `spot close ${String(spotId)}`,
          async () => { await operation.ready; }
        );
        if (this.options.detachedTaskRunner === undefined) {
          void operation.ready.catch(() => undefined);
        }
      }
      return true;
    }
    return await operation.ready;
  }

  async executeOnSpot<TSpot extends ZLinkSpot, TResult>(
    spotType: Type<TSpot>,
    spotId: RoutingId,
    operation: (spot: TSpot) => Promise<TResult> | TResult,
    signal?: AbortSignal
  ): Promise<TResult> {
    throwIfAborted(signal);
    const activation = this.activations.resolveUnique(spotId);
    if (activation === undefined) {
      throw new ZLinkConfigurationException(`Spot '${spotId}' is not active.`);
    }
    if (activation.spotType !== spotType) {
      throw new ZLinkConfigurationException(`Spot '${spotId}' has a different spot type.`);
    }
    if (activation.serial.isCurrentTurn) {
      throw createInternalFrameworkException(
        ZLinkFrameworkInternalErrorKind.InvalidOperation,
        `Spot '${String(spotId)}' cannot start a public operation from its current serial turn.`
      );
    }
    return activation.serial.execute(() => operation(activation.spot as TSpot));
  }

  hasActiveSpot(spotId: RoutingId): boolean {
    return this.activations.resolveUnique(spotId) !== undefined;
  }

  canCloseUserSpot(meshName: string, spotId: RoutingId): boolean {
    return this.activations.canClose(meshName, spotId);
  }

  beginUserSpotPublication(meshName: string, spotId: RoutingId): void {
    this.activations.stage(meshName, spotId);
  }

  publishUserSpot(meshName: string, spotId: RoutingId): void {
    this.activations.publish(meshName, spotId);
  }

  abortUserSpotPublication(meshName: string, spotId: RoutingId): void {
    this.activations.abandonStage(meshName, spotId);
  }

  resolveLocalSpotRoute(spotId: RoutingId): ZLinkSpotRouteTarget | undefined {
    const activation = this.activations.resolveUnique(spotId);
    const generation = activation?.nativeSpot?.lifecycleGeneration;
    if (activation === undefined || generation === undefined || generation <= 0n) {
      return undefined;
    }
    const nodeRid = this.options.nodeRidProvider?.(activation.meshName) ?? this.options.nodeRid;
    if (nodeRid === undefined) {
      return undefined;
    }
    return {
      routerChannelId: this.options.spotRouterChannelIdForMesh?.(activation.meshName) ?? activation.meshName,
      targetNodeRid: nodeRid,
      spotId: activation.spotId,
      spotKind: ZLinkSpotKind.User,
      targetSpotGeneration: generation
    };
  }

  async admitActorJoin(
    spotId: RoutingId,
    actor: ZLinkActor,
    request: Message,
    commit: (spot: ZLinkSpot) => Promise<ZLinkActorJoinRollback | void> | ZLinkActorJoinRollback | void,
    signal?: AbortSignal,
    leaveSource?: () => Promise<void>
  ): Promise<ZLinkSpotActorJoinResult> {
    const meshName = this.activations.resolveUnique(spotId)?.meshName;
    this.options.admission?.requireRequest('Actor join admission', meshName);
    return await this.actorMembership.admitActorJoin(
      spotId,
      actor,
      request,
      commit,
      signal,
      leaveSource
    );
  }

  async leaveActor(
    spotId: RoutingId,
    actor: ZLinkActor,
    signal?: AbortSignal
  ): Promise<void> {
    const meshName = this.activations.resolveUnique(spotId)?.meshName;
    await this.actorMembership.leaveActor(spotId, actor, signal, meshName);
  }

  async leaveActorInMesh(
    meshName: string,
    spotId: RoutingId,
    actor: ZLinkActor,
    signal?: AbortSignal
  ): Promise<void> {
    requireMeshName(meshName);
    await this.actorMembership.leaveActor(spotId, actor, signal, meshName);
  }

  async notifyActorLeftAfterTransfer(
    spotId: RoutingId,
    actor: ZLinkActor,
    signal?: AbortSignal
  ): Promise<void> {
    await this.actorMembership.notifyActorLeftAfterTransfer(spotId, actor, signal);
  }

  async prepareActorLeaveForTransfer(
    spotId: RoutingId,
    actor: ZLinkActor,
    signal?: AbortSignal
  ): Promise<void> {
    await this.actorMembership.prepareActorLeaveForTransfer(spotId, actor, signal);
  }

  async commitActorLeaveAfterTransfer(spotId: RoutingId, actorId: string): Promise<void> {
    await this.actorMembership.commitActorLeaveAfterTransfer(spotId, actorId);
  }

  async restoreActorAfterFailedTransfer(
    spotId: RoutingId,
    actor: ZLinkActor,
    signal?: AbortSignal
  ): Promise<void> {
    await this.actorMembership.restoreActorAfterFailedTransfer(spotId, actor, signal);
  }

  async beginActorTransfer(spotId: RoutingId, actorId: string): Promise<void> {
    await this.actorMembership.beginActorTransfer(spotId, actorId);
  }

  async cancelActorTransfer(spotId: RoutingId, actorId: string): Promise<void> {
    await this.actorMembership.cancelActorTransfer(spotId, actorId);
  }

  async notifyJoinedSpotActorDisconnected(
    spotId: RoutingId,
    actor: ZLinkActor,
    signal?: AbortSignal
  ): Promise<boolean> {
    return await this.actorMembership.notifyJoinedActorDisconnected(spotId, actor, signal);
  }

  dispatchRoutedActorPacket(
    spotId: RoutingId,
    actorId: string,
    parts: readonly Message[],
    returnResponse = false,
    remoteBoundSessionTarget?: ZLinkRemoteBoundSessionTarget,
    fallbackActorRef?: ActorRef
  ): Promise<unknown> {
    const activation = this.activations.resolveUnique(spotId);
    if (activation === undefined) {
      throw new ZLinkConfigurationException(`Spot '${spotId}' is not active.`);
    }
    return this.dispatchActorPacket(
      activation,
      actorId,
      parts,
      returnResponse,
      remoteBoundSessionTarget,
      fallbackActorRef
    );
  }

  async dispatchRoutedSpotSend(
    spotId: RoutingId,
    packetName: string | undefined,
    message: unknown,
    context: {
      readonly channelName: string;
      readonly contentType?: string;
      readonly workOptions?: import('../execution/serial-scheduler').ZLinkSerialWorkOptions;
    }
  ): Promise<void> {
    await this.routedSpotPackets.send(spotId, packetName, message, context);
  }

  async dispatchRoutedSpotRequest<TReply>(
    spotId: RoutingId,
    packetName: string | undefined,
    request: unknown,
    context: {
      readonly channelName: string;
      readonly contentType?: string;
      readonly workOptions?: import('../execution/serial-scheduler').ZLinkSerialWorkOptions;
    }
  ): Promise<TReply> {
    return await this.routedSpotPackets.request<TReply>(spotId, packetName, request, context);
  }

  async dispatchMeshSpot(meshName: string, owner: ReadyRecord, record: ReceiveRecord): Promise<void> {
    const spotId = owner.spotId as unknown as RoutingId | null;
    if (spotId === null) {
      throw new ZLinkConfigurationException('MeshNode Spot record is missing the owner Spot RID.');
    }
    if (record.kind === ReceiveKind.SpotMulticast) {
      const activation = this.activations.resolve(meshName, spotId);
      if (activation?.actorDispatch === undefined) {
        throw new ZLinkConfigurationException(
          `MeshNode Spot multicast target '${String(spotId)}' is not active.`
        );
      }
      if (record.topic === null || record.topic.length === 0) {
        throw new ZLinkConfigurationException('MeshNode Spot multicast record is missing its topic.');
      }
      await activation.actorDispatch.dispatchSubscriptionRecord(
        record.topic,
        record.parts,
        record.sourceNodeRid as unknown as RoutingId | null
      );
      return;
    }
    const envelope = decodeChannelEnvelope(record.parts);
    const packetName = envelope.packetName;
    const codecs = this.options.messageSerializers === undefined
      ? undefined
      : { serializers: this.options.messageSerializers };
    const decodePayload = () => decodeChannelPayload(envelope, codecs);
    const context = {
      channelName: envelope.header.channelName,
      contentType: envelope.header.contentType,
      workOptions: zlinkSerialWorkOptions(
        envelope.payload.byteLength,
        record.applicationMetadata?.byteLength ?? zlinkMetadataByteLength(envelope.header.metadata)
      )
    };
    if (record.kind === ReceiveKind.SpotSend) {
      await this.ensureInstanceApplicationActivation(meshName, spotId);
      await this.routedSpotPackets.sendEncoded(spotId, packetName, decodePayload, context);
      return;
    }
    if (record.kind !== ReceiveKind.SpotRequest) {
      throw new ZLinkConfigurationException(`Unsupported MeshNode Spot record kind '${record.kind}'.`);
    }
    try {
      await this.ensureInstanceApplicationActivation(meshName, spotId);
      const response = await this.routedSpotPackets.requestEncoded(
        spotId,
        packetName,
        decodePayload,
        context
      );
      requireMeshSpotReply(record.reply(encodeChannelReplyParts(envelope.header, response, codecs)));
    } catch (error) {
      requireMeshSpotReply(record.reply(encodeChannelErrorReplyParts(
        envelope.header,
        error instanceof Error ? error.message : String(error)
      )));
    }
  }

  private async ensureInstanceApplicationActivation(
    meshName: string,
    spotId: RoutingId
  ): Promise<void> {
    let target = this.options.instanceSpotApplicationTargetProvider?.(meshName, spotId);
    if (target === undefined) return;
    for (;;) {
      const current = this.activations.resolve(meshName, spotId);
      if (
        current !== undefined
        && current.objectGeneration === target.objectGeneration
        && current.spotType === this.requireInstanceFactory(meshName, target.stableType)
      ) return;
      await this.materializeInstance(
        meshName,
        target.stableType,
        spotId,
        target.objectGeneration
      );
      const latest = this.options.instanceSpotApplicationTargetProvider?.(meshName, spotId);
      if (latest === undefined) {
        throw createInternalFrameworkException(
          ZLinkFrameworkInternalErrorKind.SpotMoving,
          `Instance Spot target '${String(spotId)}' no longer has a Ready application target.`
        );
      }
      if (latest.objectGeneration === target.objectGeneration) return;
      target = latest;
    }
  }

  async dispatchMeshInstance(
    meshName: string,
    owner: ReadyRecord,
    record: ReceiveRecord
  ): Promise<void> {
    const spotId = owner.spotId as unknown as RoutingId | null;
    if (spotId === null) {
      throw new ZLinkConfigurationException(
        'MeshNode Instance Spot record is missing the owner Spot RID.'
      );
    }
    const envelope = decodeChannelEnvelope(record.parts);
    const inboundFlow = createInboundFlow(
      envelope.header.flowId,
      envelope.header.flowOrigin,
      this.options.dispatchErrors?.flow.flowCreationEnabled() ?? true
    );
    await runWithFlow(inboundFlow, async () => {
      const request = record.operationKind === OperationKind.InstanceSpotRequest;
      this.traceInstanceMessage(
        ZLinkMessageFlowOutcome.Received,
        meshName,
        spotId,
        record,
        envelope,
        'activating'
      );
      try {
        await this.ensureInstanceApplicationActivation(meshName, spotId);
        const activation = this.activations.resolve(meshName, spotId);
        if (activation === undefined || !this.isInstanceFactory(meshName, activation.spotType)) {
          throw new ZLinkConfigurationException(
            `MeshNode Instance Spot target '${String(spotId)}' is not active.`
          );
        }
        const target = this.options.instanceSpotApplicationTargetProvider?.(meshName, spotId);
        if (target !== undefined && activation.objectGeneration !== target.objectGeneration) {
          throw createInternalFrameworkException(
            ZLinkFrameworkInternalErrorKind.SpotGenerationStale,
            `Instance Spot target '${String(spotId)}' application generation is stale.`
          );
        }
        const codecs = this.options.messageSerializers === undefined
          ? undefined
          : { serializers: this.options.messageSerializers };
        const decodePayload = () => decodeChannelPayload(envelope, codecs);
        const context = {
          channelName: envelope.header.channelName,
          contentType: envelope.header.contentType,
          workOptions: zlinkSerialWorkOptions(
            envelope.payload.byteLength,
            record.applicationMetadata?.byteLength ?? zlinkMetadataByteLength(envelope.header.metadata)
          )
        };
        if (!request) {
          await this.routedSpotPackets.sendEncoded(spotId, envelope.packetName, decodePayload, context);
          this.traceInstanceMessage(
            ZLinkMessageFlowOutcome.Dispatched,
            meshName,
            spotId,
            record,
            envelope,
            'ready',
            target?.stableType
          );
          return;
        }
        const response = await this.routedSpotPackets.requestEncoded(
          spotId,
          envelope.packetName,
          decodePayload,
          context
        );
        requireMeshSpotReply(record.reply(encodeChannelReplyParts(envelope.header, response, codecs)));
        this.traceInstanceMessage(
          ZLinkMessageFlowOutcome.Replied,
          meshName,
          spotId,
          record,
          envelope,
          'ready',
          target?.stableType
        );
      } catch (error) {
        const reason = instanceDispatchErrorReason(error);
        if (!request) {
          this.traceInstanceMessage(
            ZLinkMessageFlowOutcome.Dropped,
            meshName,
            spotId,
            record,
            envelope,
            'closing',
            undefined,
            reason
          );
          return;
        }
        this.options.dispatchErrors?.report({
          surface: ZLinkDispatchErrorSurface.InstanceSpot,
          messageKind: ZLinkDispatchMessageKind.Request,
          packetName: envelope.packetName,
          channelName: envelope.header.channelName,
          meshName,
          correlationId: envelope.header.correlationId ?? undefined,
          sourceRid: record.sourceNodeRid === null ? undefined : String(record.sourceNodeRid),
          spotId: String(spotId),
          instanceSpotType: this.options.instanceSpotApplicationTargetProvider?.(meshName, spotId)?.stableType,
          activationState: 'closing',
          flowId: envelope.header.flowId,
          flowOrigin: envelope.header.flowOrigin,
          reason,
          action: ZLinkDispatchErrorAction.ReplyError,
          error
        });
        requireMeshSpotReply(record.reply(encodeChannelErrorReplyParts(
          envelope.header,
          error instanceof Error ? error.message : String(error)
        )));
      }
    });
  }

  private traceInstanceMessage(
    outcome: ZLinkMessageFlowOutcome,
    meshName: string,
    spotId: RoutingId,
    record: ReceiveRecord,
    envelope: ReturnType<typeof decodeChannelEnvelope>,
    activationState: 'activating' | 'ready' | 'closing',
    instanceSpotType?: string,
    errorReason?: ZLinkDispatchErrorReason
  ): void {
    const flow = flowIfEnabled(this.options.dispatchErrors?.flow, outcome);
    if (flow === undefined) return;
    flow.trace({
      outcome,
      surface: ZLinkDispatchErrorSurface.InstanceSpot,
      messageKind: record.operationKind === OperationKind.InstanceSpotRequest
        ? ZLinkDispatchMessageKind.Request
        : ZLinkDispatchMessageKind.Send,
      packetName: envelope.packetName,
      channelName: envelope.header.channelName,
      meshName,
      correlationId: envelope.header.correlationId ?? undefined,
      sourceRid: record.sourceNodeRid === null ? undefined : String(record.sourceNodeRid),
      spotId: String(spotId),
      instanceSpotType,
      activationState,
      flowId: envelope.header.flowId,
      flowOrigin: envelope.header.flowOrigin,
      errorReason
    });
  }

  async dispatchMeshActor(meshName: string, owner: ReadyRecord, record: ReceiveRecord): Promise<void> {
    const spotId = owner.spotId as unknown as RoutingId | null;
    const entrySpotId = this.options.entryNodeRidProvider?.() ?? this.options.entryNodeRid;
    const targetsEntrySpot = spotId === null
      || (entrySpotId !== undefined && spotId === entrySpotId);
    const actor = owner.actor;
    if (actor === null) {
      throw new ZLinkConfigurationException('MeshNode Actor record is missing its Actor owner.');
    }
    const request = record.kind === ReceiveKind.ActorRequest;
    if (record.sourceBindingGeneration > 0n) {
      this.options.actorBindingGenerationObserver?.(actor.actorId, record.sourceBindingGeneration);
    }
    const resolvedOwner = this.options.actorDispatchOwnerResolver?.(actor.actorId);
    const resolvedActorRef = resolvedOwner?.actorRef ?? {
      actorId: actor.actorId,
      objectGeneration: actor.generation,
      meshName,
      nodeRid: String(actor.nodeRid)
    };
    const responseActorRef = record.sourceBindingGeneration > 0n
      ? {
          ...resolvedActorRef,
          bindingGeneration: record.sourceBindingGeneration
        } as ActorRef
      : resolvedActorRef;
    const ownerActorRef = responseActorRef;
    const requestTerminalState = { prepared: false, submitted: false };
    const requestTerminal = request
      ? Object.assign(
          (response: unknown, preparedReply?: unknown) => {
            const encoded = preparedReply === undefined
              ? this.encodeMeshActorReply(
                  record.parts[0],
                  ZLinkStreamMessageKind.Response,
                  response
                )
              : preparedReply as readonly Buffer[];
            requireMeshSpotReply(record.reply(encoded));
            requestTerminalState.submitted = true;
          },
          {
            prepare: (response: unknown): readonly Buffer[] => {
              const encoded = this.encodeMeshActorReply(
                record.parts[0],
                ZLinkStreamMessageKind.Response,
                response
              );
              requestTerminalState.prepared = true;
              return encoded;
            }
          }
        )
      : undefined;
    try {
      if (this.formalRemoteTransfers.has(actor.actorId)) {
        throw new Error(
          `Actor '${actor.actorId}' target transfer reconciliation is not complete.`
        );
      }
      const response = targetsEntrySpot
        ? await this.options.dispatchEntryActorPacket?.(
          actor.actorId,
          record.parts,
          request,
          undefined,
          ownerActorRef,
          requestTerminal,
          record.messageFollowOrigin
        )
        : await this.dispatchMeshActorPacket(
          meshName,
          spotId,
          actor.actorId,
          record.parts,
          request,
          ownerActorRef,
          requestTerminal,
          record.messageFollowOrigin
        );
      if (targetsEntrySpot && this.options.dispatchEntryActorPacket === undefined) {
        throw new ZLinkConfigurationException('MeshNode Entry Spot Actor dispatch is not configured.');
      }
      if (request && !requestTerminalState.submitted) {
        requireMeshSpotReply(record.reply(this.encodeMeshActorReply(
          record.parts[0],
          ZLinkStreamMessageKind.Response,
          response
        )));
      }
    } catch (error) {
      if (!request) {
        throw error;
      }
      if (requestTerminalState.prepared || requestTerminalState.submitted) {
        throw error;
      }
      requireMeshSpotReply(record.reply(this.encodeMeshActorReply(
        record.parts[0],
        ZLinkStreamMessageKind.Error,
        {
          message: error instanceof Error ? error.message : String(error),
          kind: error instanceof ZLinkFrameworkException
            ? error.kind
            : ZLinkFrameworkErrorKind.InternalFailure
        }
      )));
    }
  }

  async dispatchMeshActorJoin(meshName: string, owner: ReadyRecord, record: ReceiveRecord): Promise<void> {
    const spotId = owner.spotId as unknown as RoutingId | null;
    const control = record.kindData;
    if (control?.kind !== 'actorControl') {
      throw new ZLinkConfigurationException('MeshNode Actor join record is missing lifecycle identity data.');
    }
    const actorId = control.currentActor?.actorId;
    if (spotId === null || actorId === undefined) {
      throw new ZLinkConfigurationException('MeshNode Actor join record is missing its Spot or Actor owner.');
    }
    const entrySpotId = this.options.entryNodeRidProvider?.() ?? this.options.entryNodeRid;
    const targetsEntrySpot = entrySpotId !== undefined
      && String(spotId) === String(entrySpotId);
    const activation = this.activations.resolve(meshName, spotId);
    const transferRequest = record.parts.length === 0
      ? undefined
      : decodeFormalRemoteTransferRequest(record.parts[0]!);
    const remoteJoinPhase = transferRequest?.phase;
    const isRemoteAdmission = remoteJoinPhase === REMOTE_ACTOR_JOIN_ADMISSION;
    const isRemoteCommit = remoteJoinPhase === REMOTE_ACTOR_JOIN_COMMIT;
    const isRemoteAbort = remoteJoinPhase === REMOTE_ACTOR_JOIN_ABORT;
    let admissionRecord = transferRequest !== undefined
      ? this.formalRemoteActorAdmissions.get(transferRequest.transferId)
      : undefined;
    let pendingAdmission = isRemoteCommit ? admissionRecord : undefined;
    let commitAdmissionMissing = isRemoteCommit && admissionRecord === undefined;
    const applicationClaim = transferRequest === undefined
      ? this.options.admission?.claim(meshName, 'Actor join dispatch')
      : undefined;
    const resolvedActor = this.options.actorResolver?.(actorId);
    const lifecycleActor = targetsEntrySpot
      ? this.options.actorLifecycleResolver?.(actorId)
      : undefined;
    let actor = resolvedActor;
    if (actor === undefined && targetsEntrySpot) {
      // An actor returning from a User Spot can still be marked as moving
      // while the source-side relocation fence is being completed. Entry Spot
      // admission must resolve that lifecycle object so the accepted Core
      // reply can commit the return transaction.
      actor = lifecycleActor;
    }
    if (actor === undefined && admissionRecord?.actor !== undefined) {
      actor = admissionRecord.actor;
    }
    let callbackRequest: Message | undefined = record.parts.length === 0
      ? undefined
      : record.parts[0]!;
    let ownedCallbackRequest: Message | undefined;
    let materialized = false;
    let reply: Message | undefined;
    let deferredJoinRoot: ZLinkDeferredJoinAcceptedRoot | undefined;
    let preparedTransferState: Message | undefined;
    let admissionOutcome: ZLinkFormalRemoteActorAdmissionResult | undefined;
    let accepted = false;
    let committedAdmissionReplay = isRemoteCommit
      && admissionRecord?.state === 'committed';
    let targetCommitPublished: boolean | undefined;
    const actorJoinIsCurrent = (): boolean => (
      (record.deadlineUnixMs === undefined || record.deadlineUnixMs > BigInt(Date.now()))
      && (record.isPending?.() ?? true)
    );
    const abandonStaleAdmission = (): void => {
      if (isRemoteAdmission && transferRequest !== undefined) {
        this.formalRemoteActorAdmissions.abort(transferRequest.transferId);
      }
    };
    try {
      if (!isRemoteAbort && !actorJoinIsCurrent()) {
        abandonStaleAdmission();
        return;
      }
      if (isRemoteAbort) {
        if (transferRequest !== undefined) {
          this.formalRemoteActorAdmissions.abort(transferRequest.transferId);
        }
        requireMeshSpotReply(record.replyActorJoin(1, []));
        return;
      }
      if (transferRequest !== undefined) {
        ownedCallbackRequest = RuntimeMessage.from(
          Buffer.from(transferRequest.request, 'base64')
        );
        callbackRequest = ownedCallbackRequest;
      }
      if (
        transferRequest !== undefined
        && !targetsEntrySpot
        && remoteJoinPhase === undefined
      ) {
        // User Spot transfers must carry the private admission/commit phases.
        // A legacy one-phase request would otherwise read relocation state
        // before the target application has accepted the Actor.
        requireMeshSpotReply(record.replyActorJoin(1, []));
        return;
      }
      if (isRemoteAdmission && transferRequest !== undefined) {
        const rawActorRef = transferRequest.actorRef ?? control.currentActor ?? undefined;
        if (rawActorRef === undefined) {
          throw new ZLinkConfigurationException(`Actor '${actorId}' join record has no ActorRef.`);
        }
        const actorRef = toFrameworkRemoteAdmissionActorRef(rawActorRef, meshName);
        const admission = this.formalRemoteActorAdmissions.begin({
          actorId,
          actorType: transferRequest.actorType,
          actorRef,
          spotId,
          targetSpotGeneration: control.currentSpotGeneration,
          expectedMembershipEpoch: transferRequest.expectedMembershipEpoch,
          requestFingerprint: transferRequest.request,
          transferId: transferRequest.transferId
        });
        admissionRecord = admission.record;
        if (admission.created) {
          try {
            if (targetsEntrySpot) {
              admissionOutcome = {
                accepted: this.options.dispatchEntryActorJoin !== undefined,
                actorRef
              };
            } else if (activation === undefined) {
              admissionOutcome = { accepted: false, actorRef };
            } else {
              if (!actorJoinIsCurrent()) {
                abandonStaleAdmission();
                return;
              }
              const response: ZLinkSpotActorJoinResult = await activation.serial.execute(async () =>
                activation.spot.onActorJoin(
                  actorRef.actorId,
                  wrapFrameworkPayloadMessage(ownedCallbackRequest!, this.options.messageSerializers)
                )
              );
              let encodedReply: Message | undefined;
              try {
                encodedReply = response.reply === undefined
                  ? undefined
                  : encodeFrameworkPayloadMessage(response.reply, this.options.messageSerializers);
                if (
                  response.accepted
                  && transferRequest.completionOperationId !== undefined
                  && this.options.actorTransferRuntime !== undefined
                ) {
                  deferredJoinRoot = await this.options.actorTransferRuntime
                    .prepareDeferredJoinAccepted(
                      actorId,
                      transferRequest.completionOperationId,
                      actorRef,
                      encodedReply?.data() ?? Buffer.alloc(0),
                      undefined,
                      {
                        targetMeshName: meshName,
                        targetSpotId: String(spotId),
                        targetSpotGeneration: control.currentSpotGeneration,
                        membershipEpoch: control.currentMembershipEpoch,
                        request: Buffer.from(record.parts[0]!.data())
                      }
                    );
                  this.options.runtimeEventPublisher?.publish({
                    sourceName: 'zlink.framework.actor-handoff',
                    timestamp: new Date(),
                    marker: 'deferred_completion_staged',
                    actorId
                  });
                }
                admissionOutcome = {
                  accepted: response.accepted,
                  actorRef,
                  ...(encodedReply === undefined
                    ? {}
                    : { reply: Buffer.from(encodedReply.data()) })
                };
              } finally {
                encodedReply?.close();
              }
              if (!actorJoinIsCurrent()) {
                abandonStaleAdmission();
                return;
              }
            }
            this.formalRemoteActorAdmissions.complete(
              transferRequest.transferId,
              admissionOutcome
            );
          } catch (error) {
            this.formalRemoteActorAdmissions.fail(transferRequest.transferId, error);
            throw error;
          }
        }
        admissionOutcome = await admissionRecord.resultTask;
        if ('error' in admissionOutcome) {
          throw admissionOutcome.error;
        }
        accepted = admissionOutcome.accepted;
        reply = admissionOutcome.reply === undefined
          ? undefined
          : RuntimeMessage.from(admissionOutcome.reply);
      }
      if (isRemoteCommit && admissionRecord !== undefined) {
        admissionOutcome = await admissionRecord.resultTask;
        if ('error' in admissionOutcome) {
          throw admissionOutcome.error;
        }
      }
      if (
        actor === undefined
        && transferRequest !== undefined
        && this.options.actorTransferRuntime !== undefined
        && !isRemoteAdmission
        && !commitAdmissionMissing
        && admissionRecord?.state !== 'committed'
        && (targetsEntrySpot || activation !== undefined)
      ) {
        // The admission phase has already completed before this point. The
        // source may roll back and delete the reference after its Core request
        // becomes disconnected, so the commit phase owns this read.
        preparedTransferState = RuntimeMessage.from(
          transferRequest.transferStateReference === undefined
            ? Buffer.from(transferRequest.transferState!, 'base64')
            : await this.options.actorTransferRuntime.readPreparedTransferState(
                transferRequest.transferStateReference,
                transferRequest.transferStateChecksumCrc32c!
              )
        );
      }
      if (isRemoteAdmission && targetsEntrySpot) {
        accepted = admissionOutcome !== undefined
          && !('error' in admissionOutcome)
          && admissionOutcome.accepted;
      } else if (isRemoteCommit && pendingAdmission !== undefined) {
        validateRemoteAdmissionCommit(pendingAdmission, transferRequest!, actorId, control);
        accepted = pendingAdmission.state === 'admitted'
          || pendingAdmission.state === 'committed';
      } else if (commitAdmissionMissing) {
        accepted = false;
      } else if (targetsEntrySpot) {
        // Entry Spot return joins have no application admission callback. The
        // actor-manager transaction is committed after the Core reply below.
        // A formal transfer payload materializes the existing actor state at
        // the Entry owner before that transaction is committed.
        accepted = actor !== undefined
          || (transferRequest !== undefined && this.options.actorTransferRuntime !== undefined);
      } else if (
        !isRemoteAdmission
        &&
        activation !== undefined
        && callbackRequest !== undefined
        && (actor !== undefined || transferRequest !== undefined)
      ) {
        if (!actorJoinIsCurrent()) return;
        const request = callbackRequest;
        const rawActorRef = transferRequest?.actorRef ?? control.currentActor ?? undefined;
        if (rawActorRef === undefined) {
          throw new ZLinkConfigurationException(`Actor '${actorId}' join record has no ActorRef.`);
        }
        const actorRef: ActorRef = {
          actorId: rawActorRef.actorId,
          objectGeneration: 'objectGeneration' in rawActorRef
            ? rawActorRef.objectGeneration
            : rawActorRef.generation,
          meshName,
          nodeRid: rawActorRef.nodeRid as unknown as RoutingId
        };
        const response: ZLinkSpotActorJoinResult = await activation.serial.execute(async () =>
          activation.spot.onActorJoin(
            actorRef.actorId,
            wrapFrameworkPayloadMessage(request, this.options.messageSerializers)
          )
        );
        if (!actorJoinIsCurrent()) return;
        accepted = response.accepted;
        reply = response.reply === undefined
          ? undefined
          : encodeFrameworkPayloadMessage(response.reply, this.options.messageSerializers);
        if (
          accepted
          && transferRequest?.completionOperationId !== undefined
          && this.options.actorTransferRuntime !== undefined
        ) {
          deferredJoinRoot = await this.options.actorTransferRuntime
            .prepareDeferredJoinAccepted(
              actorId,
              transferRequest.completionOperationId,
              actorRef,
              reply?.data() ?? Buffer.alloc(0),
              undefined,
              {
                targetMeshName: meshName,
                targetSpotId: String(spotId),
                targetSpotGeneration: control.currentSpotGeneration,
                membershipEpoch: control.currentMembershipEpoch,
                request: Buffer.from(record.parts[0]!.data())
              }
            );
          this.options.runtimeEventPublisher?.publish({
            sourceName: 'zlink.framework.actor-handoff',
            timestamp: new Date(),
            marker: 'deferred_completion_staged',
            actorId
          });
        }
      }
      if (
        accepted
        && !isRemoteAdmission
        && actor === undefined
        && transferRequest !== undefined
        && this.options.actorTransferRuntime !== undefined
      ) {
        if (!actorJoinIsCurrent()) return;
        const transferState = preparedTransferState;
        preparedTransferState = undefined;
        if (transferState === undefined) {
          throw new Error('Remote actor transfer state was not prepared before admission.');
        }
        try {
          const result = await this.options.actorTransferRuntime.materializeRoutedActor(
            actorId,
            transferRequest.actorType,
            transferRequest.transferAdapterKey,
            transferState,
            transferRequest.actorEntryNodeRid,
            transferRequest.remoteBoundSessionTarget
          );
          actor = result.actor;
          materialized = true;
        } finally {
          transferState.close();
        }
        if (!actorJoinIsCurrent()) {
          await this.options.actorTransferRuntime.rollbackRoutedActor(actor);
          materialized = false;
          return;
        }
      }
      if (
        accepted
        && transferRequest !== undefined
        && this.options.actorTransferRuntime !== undefined
        && !isRemoteAdmission
      ) {
        // A lightweight Core actor-join notification can create the Actor
        // before the formal transfer request reaches this branch. Preserve
        // the full relocation fence in that race so a later Session bind
        // refresh cannot erase the seal or accepted journal.
        this.options.actorTransferRuntime.rememberRoutedActorTransferTarget(
          actorId,
          transferRequest.remoteBoundSessionTarget
        );
      }
      if (
        accepted
        && actor !== undefined
        && transferRequest !== undefined
        && !isRemoteAdmission
        && !committedAdmissionReplay
      ) {
        this.formalRemoteTransfers.begin({
          actor,
          spotId,
          transferId: transferRequest.transferId,
          handoffBacklog: transferRequest.handoffBacklog,
          deferredJoinRoot
        });
        if (isRemoteCommit && admissionRecord !== undefined) {
          this.formalRemoteActorAdmissions.markCommitted(
            transferRequest.transferId,
            actor
          );
        }
      }
      const replyActorJoin = (): boolean => {
        if (!actorJoinIsCurrent()) return false;
        const joinReplyResult = record.replyActorJoin(
          accepted ? 0 : 1,
          reply === undefined ? [] : [reply.data()]
        );
        if (accepted && !isRemoteAdmission && joinReplyResult === SubmitResult.Ok) {
          // Core commits the target membership while it accepts this reply.
          // From this call onward a later callback/transport failure is
          // post-commit and must not roll the materialized Actor back.
          targetCommitPublished = true;
        }
        requireMeshSpotReply(joinReplyResult);
        return true;
      };
      if (accepted && targetsEntrySpot && actor !== undefined) {
        const entryActor = actor;
        const pendingTransfer = transferRequest === undefined
          ? undefined
          : this.formalRemoteTransfers.get(actorId);
        if (committedAdmissionReplay) {
          if (!replyActorJoin()) return;
        } else if (pendingTransfer === undefined) {
          if (!actorJoinIsCurrent()) return;
          await this.options.dispatchEntryActorJoin?.(meshName, entryActor);
          // The Entry path performs its lifecycle commit directly because it
          // must wait for OnJoinedActor before the native join completion is
          // released. Do not leave a second Entry Joined control behind the
          // same mailbox turn.
          if (!replyActorJoin()) return;
        } else {
          // A formal transfer must acknowledge the target admission before
          // waiting for the source terminal; that terminal is what permits
          // the detached target commit below to proceed.
          if (!replyActorJoin()) return;
          const commitEntryTransfer = async (): Promise<void> => {
            const sourceLeaveSucceeded = await pendingTransfer.sourceLeaveTerminal;
            if (!sourceLeaveSucceeded) {
              throw new Error(
                `Actor '${entryActor.context.actorId}' source leave callback failed before Entry Spot commit.`
              );
            }
            await this.options.dispatchEntryActorJoin?.(
              meshName,
              entryActor,
              pendingTransfer.handoffBacklog
            );
            this.formalRemoteTransfers.delete(entryActor.context.actorId);
          };
          this.options.detachedTaskRunner?.runDetached(
            `actor Entry Spot transfer ${entryActor.context.actorId}`,
            commitEntryTransfer
          );
          if (this.options.detachedTaskRunner === undefined) {
            void commitEntryTransfer().catch(() => undefined);
          }
        }
      } else {
        if (!replyActorJoin()) return;
      }
    } catch (error) {
      if (deferredJoinRoot !== undefined && targetCommitPublished !== true) {
        await this.options.actorTransferRuntime?.discardDeferredJoinAccepted(
          deferredJoinRoot
        );
      }
      if (materialized && targetCommitPublished !== true) {
        await this.options.actorTransferRuntime?.rollbackRoutedActor(actor!);
      }
      throw error;
    } finally {
      preparedTransferState?.close();
      ownedCallbackRequest?.close();
      reply?.close();
      applicationClaim?.close();
    }
  }

  async dispatchMeshSpotControl(meshName: string, owner: ReadyRecord, record: ReceiveRecord): Promise<void> {
    const spotId = owner.spotId as unknown as RoutingId | null;
    const control = record.kindData;
    if (control?.kind !== 'actorControl') {
      throw new ZLinkConfigurationException('MeshNode Spot control record is missing actor lifecycle data.');
    }
    const actorRef = control.lifecycleKind === ActorLifecycleKind.Left
      ? control.previousActor
      : control.currentActor;
    const actorId = actorRef?.actorId;
    const pendingTransfer = actorId === undefined
      ? undefined
      : this.formalRemoteTransfers.get(actorId);
    const sourceActivation = actorId === undefined || spotId === null
      ? undefined
      : this.activations.resolve(meshName, spotId);
    const sourceJoinedActor = actorId === undefined
      ? undefined
      : sourceActivation?.resolveJoinedActor(actorId);
    const actor = actorId === undefined
      ? undefined
      : pendingTransfer?.actor
        ?? sourceJoinedActor
        ?? this.options.actorLifecycleResolver?.(actorId)
        ?? this.options.actorResolver?.(actorId);
    const entrySpotId = this.options.entryNodeRidProvider?.() ?? this.options.entryNodeRid;
    if (actor === undefined) {
      return;
    }
    if (control.lifecycleKind === ActorLifecycleKind.Left
      && (spotId === null || (entrySpotId !== undefined && String(spotId) === String(entrySpotId)))) {
      const callback = async () => {
        await this.options.entrySpotCallbacks?.onLeaveActor(
          actor,
          undefined,
          actorRef as unknown as ActorRef,
          control.previousMembershipEpoch
        );
      };
      if (this.options.actorTransferRuntime === undefined) {
        await callback();
      } else {
        await this.options.actorTransferRuntime.notifyCoreSourceLeave(actor, callback);
      }
      return;
    }
    if (spotId === null) {
      return;
    }
    const activation = this.activations.resolve(meshName, spotId);
    if (activation === undefined) {
      return;
    }
    await activation.serial.execute(async () => {
      if (control.lifecycleKind === ActorLifecycleKind.Joined) {
        if (control.currentActor !== null) {
          this.options.actorTransferRuntime?.bindRoutedActorRef(
            actor,
            control.currentActor as unknown as ActorRef
          );
        }
        this.options.actorTransferRuntime?.commitRoutedActor(actor, spotId, activation.spot);
        const completeTargetCommit = async (sourceLeaveSucceeded: boolean): Promise<void> => {
          if (sourceLeaveSucceeded && pendingTransfer !== undefined) {
            await replayActorHandoffBacklog(
              pendingTransfer.handoffBacklog,
              (parts, returnResponse, remoteBoundSessionTarget, _fallbackActorRef) =>
                this.dispatchActorPacket(
                  activation,
                  actor.context.actorId,
                  parts,
                  returnResponse,
                  remoteBoundSessionTarget,
                  undefined
                ),
              (index) => this.options.runtimeEventPublisher?.publish({
                sourceName: 'zlink.framework.actor-handoff',
                timestamp: new Date(),
                marker: 'backlog_enqueued',
                actorId: actor.context.actorId,
                index
              })
            );
          }
          await this.options.actorTransferRuntime?.claimRoutedActorLocation(
            actor,
            spotId,
            meshName,
            {
              spotGeneration: control.currentSpotGeneration,
              membershipEpoch: control.currentMembershipEpoch
            }
          );
          if (!sourceLeaveSucceeded) {
            throw new Error(`Actor '${actor.context.actorId}' source leave callback failed after target commit.`);
          }
          activation.commitActorJoin(actor);
          const updateBoundSessionRoute = async (): Promise<void> => {
            await this.options.actorTransferRuntime?.publishRoutedActorOwnership(actor);
            await this.options.actorTransferRuntime?.openRoutedActorSession(actor);
          };
          // Session route publication is post-commit work. The public Join
          // completion and lifecycle callback must not wait for a Session
          // owner ACK; the route update retries in its detached runtime task.
          if (pendingTransfer !== undefined && this.options.detachedTaskRunner !== undefined) {
            this.options.detachedTaskRunner.runDetached(
              `actor transfer Session route ${actor.context.actorId}`,
              updateBoundSessionRoute
            );
          } else {
            await updateBoundSessionRoute();
          }
          const deferredJoinRoot = pendingTransfer?.deferredJoinRoot
            ?? await this.options.actorTransferRuntime?.recoverDeferredJoinAccepted(actor.context.actorId);
          if (deferredJoinRoot !== undefined) {
            const currentRef = this.options.actorTransferRuntime === undefined
              ? null
              : control.currentActor as unknown as ActorRef | null;
            if (currentRef === null) {
              throw new Error(`Actor '${actor.context.actorId}' has no target ref for deferred Join completion.`);
            }
            await this.options.actorTransferRuntime?.commitAndDeliverDeferredJoinAccepted(
              deferredJoinRoot,
              actor,
              currentRef,
              operation => activation.executeActor(
                actor.context.actorId,
                async () => await operation()
              )
            );
          }
          this.formalRemoteTransfers.delete(actor.context.actorId);
          // A transferred actor must receive its first lifecycle callback only
          // after ownership and the bound-session route are publishable. The
          // callback may send to the actor; invoking it before this point
          // races the formal transfer reconciliation guard.
          await activation.spot.onJoinedActor(actor);
        };
        if (pendingTransfer !== undefined) {
          const resume = async (): Promise<void> => {
            const sourceLeaveSucceeded = await pendingTransfer.sourceLeaveTerminal;
            try {
              await activation.serial.execute(() =>
                completeTargetCommit(sourceLeaveSucceeded)
              );
            } catch (error) {
              throw error;
            }
          };
          this.options.detachedTaskRunner?.runDetached(
            `actor transfer target commit ${actor.context.actorId}`,
            resume
          );
          if (this.options.detachedTaskRunner === undefined) {
            void resume().catch(() => undefined);
          }
          return;
        }
        await completeTargetCommit(true);
        return;
      }
      if (control.lifecycleKind === ActorLifecycleKind.Left) {
        if (this.options.actorTransferRuntime === undefined) {
          await activation.spot.onLeaveActor(actor);
        } else {
          // Spot.Context.leaveActor can run the source callback before the
          // native Entry join is submitted. In that path the membership is
          // already committed away, so a later Core LEFT control must not
          // execute the same callback twice.
          if (sourceJoinedActor === undefined && pendingTransfer === undefined) {
            return;
          }
          await this.options.actorTransferRuntime.notifyCoreSourceLeave(
            actor,
            () => activation.spot.onLeaveActor(actor)
          );
        }
        activation.commitActorDeparture(actor.context.actorId);
        return;
      }
      if (control.lifecycleKind === ActorLifecycleKind.Disconnected) {
        await activation.spot.onDisconnectActor?.(actor);
      }
    });
  }

  async recoverPublishedActorTransfer(
    root: ZLinkDeferredJoinAcceptedRoot,
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
      readonly activation?: ZLinkSpotActivation;
      readonly implementation: Type<ZLinkSpot>;
    },
    signal?: AbortSignal
  ): Promise<void> {
    const recovery = root.recovery;
    if (recovery === undefined) {
      throw new Error(
        `Actor '${root.actor.actorId}' published Join root has no recovery manifest.`
      );
    }
    if (
      target.meshName !== recovery.targetMeshName
      || target.spotId !== recovery.targetSpotId
      || target.spotGeneration !== recovery.targetSpotGeneration
    ) {
      throw new Error(
        `Actor '${root.actor.actorId}' recovery root does not match its target authority.`
      );
    }
    const recoveryPayload = await this.options.actorTransferRuntime
      ?.readDeferredJoinRecoveryPayload(root, signal);
    if (recoveryPayload === undefined) {
      throw new Error(
        `Actor '${root.actor.actorId}' recovery payload reader is unavailable.`
      );
    }
    let transferState: Message | undefined;
    let materialized = false;
    let authorityPublished = false;
    let activation = target.activation;
    let activationPrepared = false;
    let activationPublished = activation !== undefined;
    let currentRoot = root;
    try {
      const transfer = decodeFormalRemoteTransferRequestBytes(recoveryPayload);
      if (
        transfer === undefined
        || transfer.actorRef?.actorId !== root.actor.actorId
        || transfer.actorRef.objectGeneration !== root.actor.objectGeneration
      ) {
        const recoveredIdentity = transfer?.actorRef === undefined
          ? 'missing'
          : `${transfer.actorRef.actorId}/${transfer.actorRef.objectGeneration}`;
        throw new Error(
          `Actor '${root.actor.actorId}' published Join recovery request is invalid `
          + `(expected ${root.actor.actorId}/${root.actor.objectGeneration}, `
          + `recovered ${recoveredIdentity}, bytes ${recoveryPayload.byteLength}).`
        );
      }
      let actor = this.options.actorLifecycleResolver?.(root.actor.actorId)
        ?? this.options.actorResolver?.(root.actor.actorId);
      if (this.options.actorTransferRuntime === undefined) {
        throw new Error(
          `Actor '${root.actor.actorId}' recovery runtime is unavailable.`
        );
      }
      let targetActorRef = actor === undefined
        ? {
            actorId: root.actor.actorId,
            objectGeneration: root.actor.objectGeneration,
            meshName: recovery.targetMeshName,
            nodeRid: target.nodeRid
          }
        : this.options.actorTransferRuntime.currentRoutedActorRef(actor);
      if (
        targetActorRef === undefined
        || targetActorRef.actorId !== root.actor.actorId
        || targetActorRef.objectGeneration !== root.actor.objectGeneration
        || targetActorRef.meshName !== recovery.targetMeshName
        || String(targetActorRef.nodeRid) !== String(target.nodeRid)
      ) {
        throw new Error(
          `Actor '${root.actor.actorId}' recovery materialized a different target identity.`
        );
      }
      const publication = await this.options.actorTransferRuntime
        .takeOverDeferredJoinRecoveryAuthority(
          currentRoot,
          targetActorRef,
          target,
          signal
        );
      if (publication === undefined) {
        return;
      }
      currentRoot = publication.root;
      authorityPublished = true;
      if (activation === undefined) {
        activation = await this.prepareRelocationSpot(
          target.meshName,
          'user_spot',
          publication.spotAuthority.allocation.stableType,
          target.implementation,
          recovery.targetSpotId as RoutingId,
          publication.spotAuthority.objectGeneration,
          publication.spotAuthority.authorityOwnerGeneration,
          signal
        );
        activationPrepared = true;
      }
      if (actor === undefined) {
        transferState = transfer.transferStateReference === undefined
          ? RuntimeMessage.from(Buffer.from(transfer.transferState!, 'base64'))
          : RuntimeMessage.from(
              await this.options.actorTransferRuntime.readPreparedTransferState(
                transfer.transferStateReference,
                transfer.transferStateChecksumCrc32c!,
                signal
              )
            );
        actor = await this.options.actorTransferRuntime.prepareRecoveryRoutedActor(
          root.actor.actorId,
          transfer.actorType,
          targetActorRef,
          publication.actorAuthority.authorityOwnerGeneration,
          recovery.targetSpotId as RoutingId,
          recovery.targetSpotGeneration,
          recovery.membershipEpoch,
          transfer.transferAdapterKey,
          transferState,
          transfer.actorEntryNodeRid,
          transfer.remoteBoundSessionTarget,
          signal
        );
        materialized = true;
        targetActorRef = this.options.actorTransferRuntime.currentRoutedActorRef(actor);
        if (
          targetActorRef === undefined
          || targetActorRef.objectGeneration !== root.actor.objectGeneration
          || String(targetActorRef.nodeRid) !== String(target.nodeRid)
        ) {
          throw new Error(
            `Actor '${root.actor.actorId}' recovery did not preserve its published identity.`
          );
        }
      }
      this.options.actorTransferRuntime.bindRoutedActorRef(actor, targetActorRef);
      const recoveredActivation = activation;
      if (activationPrepared) {
        await this.publishRelocationSpot(recoveredActivation);
        activationPublished = true;
      }
      await recoveredActivation.serial.execute(async () => {
        this.options.actorTransferRuntime!.commitRoutedActor(
          actor!,
          recovery.targetSpotId as RoutingId,
          recoveredActivation.spot
        );
        this.options.actorTransferRuntime!.adoptRoutedActorAuthority(
          actor!,
          publication.actorAuthority,
          recovery.targetSpotId as RoutingId,
          recoveredActivation.spot,
          recovery.membershipEpoch
        );
        await this.options.actorTransferRuntime!.publishRoutedActorOwnership(actor!);
        if (
          recoveredActivation.resolveJoinedActor(actor!.context.actorId) === undefined
        ) {
          recoveredActivation.commitActorJoin(actor!);
        }
        currentRoot = await this.options.actorTransferRuntime!
          .markDeferredJoinAcceptedCommitted(currentRoot, targetActorRef!, signal);
        for (
          let index = currentRoot.replayCursor;
          index < transfer.handoffBacklog.length;
          index++
        ) {
          await replayActorHandoffBacklog(
            [transfer.handoffBacklog[index]!],
            (parts, returnResponse, remoteBoundSessionTarget, _fallbackActorRef) =>
              this.dispatchActorPacket(
                recoveredActivation,
                actor!.context.actorId,
                parts,
                returnResponse,
                remoteBoundSessionTarget,
                targetActorRef!
              )
          );
          currentRoot = await this.options.actorTransferRuntime!
            .markDeferredJoinRecoveryMessageReplayed(
              currentRoot,
              index + 1,
              signal
            );
        }
        currentRoot = await this.options.actorTransferRuntime!
          .commitAndDeliverDeferredJoinAccepted(
            currentRoot,
            actor!,
            targetActorRef!,
            operation => recoveredActivation.executeActor(
              actor!.context.actorId,
              async () => await operation()
            ),
            signal,
            true
          );
        this.options.actorTransferRuntime!.publishRecoveryRoutedActor(actor!);
        await this.options.actorTransferRuntime!.openRoutedActorSession(actor!);
        await this.options.actorTransferRuntime!
          .releaseDeferredJoinRecovery(currentRoot, signal);
      });
    } catch (error) {
      if (materialized && !authorityPublished) {
        const actor = this.options.actorLifecycleResolver?.(root.actor.actorId);
        if (actor !== undefined) {
          await this.options.actorTransferRuntime?.rollbackRoutedActor(actor, signal);
        }
      }
      if (activationPrepared && !activationPublished && activation !== undefined) {
        await this.abortRelocationSpot(activation).catch(() => undefined);
      }
      throw error;
    } finally {
      transferState?.close();
    }
  }

  completeFormalSourceLeaveTerminal(
    actorId: string,
    transferId: string,
    succeeded: boolean
  ): boolean {
    return this.formalRemoteTransfers.completeSourceLeaveTerminal(
      actorId,
      transferId,
      succeeded
    );
  }

  private encodeMeshActorReply(
    requestHeaderPart: Message,
    kind: ZLinkStreamMessageKind.Response | ZLinkStreamMessageKind.Error,
    payload: unknown
  ): readonly Buffer[] {
    const requestHeader = decodeStreamHeader(requestHeaderPart.data());
    const payloadPart = encodeFrameworkPayloadMessage(payload, this.options.messageSerializers);
    try {
      return [
        Buffer.from(encodeStreamHeader({
          kind,
          codec: ZLinkStreamCodec.Json,
          flags: ZLinkStreamHeaderFlags.None,
          requestSeq: requestHeader.requestSeq,
          name: '',
          metadata: new Map(),
          correlationId: requestHeader.correlationId
        })),
        Buffer.from(payloadPart.data())
      ];
    } finally {
      payloadPart.close();
    }
  }

  private async createActivation<TSpot extends ZLinkSpot>(
    meshName: string,
    spotType: Type<TSpot>,
    spotId: RoutingId,
    request: Message,
    signal?: AbortSignal,
    authority?: ZLinkNativeSpotAuthority
  ): Promise<ZLinkLocalSpotCreateResult> {
    this.requireRegisteredFactory(spotType);
    return await this.activationLifecycle.create(
      meshName,
      spotType,
      spotId,
      request,
      signal,
      authority
    );
  }

  private requireRegisteredFactory(spotType: Type<ZLinkSpot>): void {
    if (!this.factories.has(spotType)) {
      throw new ZLinkConfigurationException('Spot type is not registered as a spot factory.');
    }
  }

  private async dispatchActorPacket(
    activation: ZLinkSpotActivation,
    actorId: string,
    parts: readonly Message[],
    returnResponse = false,
    remoteBoundSessionTarget?: ZLinkRemoteBoundSessionTarget,
    fallbackActorRef?: ActorRef,
    requestTerminal?: (response: unknown) => Promise<void> | void,
    messageFollowOrigin?: ZLinkMessageFollowOrigin
  ): Promise<unknown> {
    return await this.activationLifecycle.dispatchActorPacket(
      activation,
      actorId,
      parts,
      returnResponse,
      remoteBoundSessionTarget,
      fallbackActorRef,
      requestTerminal,
      messageFollowOrigin
    );
  }

  private async dispatchMeshActorPacket(
    meshName: string,
    spotId: RoutingId,
    actorId: string,
    parts: readonly Message[],
    returnResponse: boolean,
    fallbackActorRef?: ActorRef,
    requestTerminal?: (response: unknown) => Promise<void> | void,
    messageFollowOrigin?: ZLinkMessageFollowOrigin
  ): Promise<unknown> {
    const activation = this.activations.resolve(meshName, spotId);
    if (activation === undefined) {
      throw new ZLinkConfigurationException(
        `Spot '${String(spotId)}' is not active in mesh '${meshName}'.`
      );
    }
    return await this.dispatchActorPacket(
      activation,
      actorId,
      parts,
      returnResponse,
      undefined,
      fallbackActorRef,
      requestTerminal,
      messageFollowOrigin
    );
  }

}

function instanceDispatchErrorReason(error: unknown): ZLinkDispatchErrorReason {
  if (error instanceof ZLinkFrameworkException) {
    const kind = internalFrameworkErrorKind(error);
    if (
      kind === ZLinkFrameworkInternalErrorKind.SpotGenerationStale
      || kind === ZLinkFrameworkInternalErrorKind.SpotMoving
      || kind === ZLinkFrameworkInternalErrorKind.SpotRouteNotFound
      || kind === ZLinkFrameworkInternalErrorKind.RequestTargetNotFound
    ) {
      return ZLinkDispatchErrorReason.StaleTarget;
    }
  }
  return ZLinkDispatchErrorReason.HandlerException;
}

interface ZLinkFormalRemoteTransferRequest {
  readonly actorType: string;
  readonly actorId?: string;
  readonly phase?: 'admission' | 'commit' | 'abort';
  readonly transferId: string;
  readonly transferAdapterKey?: string;
  readonly transferState?: string;
  readonly transferStateReference?: string;
  readonly transferStateChecksumCrc32c?: number;
  readonly request: string;
  readonly actorEntryNodeRid?: RoutingId;
  readonly actorRef?: ActorRef;
  readonly remoteBoundSessionTarget?: ZLinkRemoteBoundSessionTarget;
  readonly expectedMembershipEpoch: bigint;
  readonly handoffBacklog: readonly ZLinkActorHandoffPacket[];
  readonly completionOperationId?: {
    readonly high: bigint;
    readonly low: bigint;
  };
}

function decodeFormalRemoteTransferRequest(
  message: Message
): ZLinkFormalRemoteTransferRequest | undefined {
  return decodeFormalRemoteTransferRequestBytes(message.data());
}

function decodeFormalRemoteTransferRequestBytes(
  bytes: Uint8Array
): ZLinkFormalRemoteTransferRequest | undefined {
  let payload: ZLinkRemoteActorJoinWirePayload;
  try {
    payload = JSON.parse(Buffer.from(bytes).toString()) as ZLinkRemoteActorJoinWirePayload;
  } catch {
    return undefined;
  }
  if (
    payload.packetName !== REMOTE_ACTOR_JOIN_PACKET
    || typeof payload.actorType !== 'string'
    || typeof payload.transferId !== 'string'
    || !(
      payload.phase === REMOTE_ACTOR_JOIN_ADMISSION
      || payload.phase === REMOTE_ACTOR_JOIN_ABORT
      || payload.phase === REMOTE_ACTOR_JOIN_COMMIT
      || payload.phase === undefined
    )
    || !(
      payload.phase === REMOTE_ACTOR_JOIN_ADMISSION
      || payload.phase === REMOTE_ACTOR_JOIN_ABORT
      || (
      typeof payload.transferState === 'string'
      || (
        typeof payload.transferStateReference === 'string'
        && typeof payload.transferStateChecksumCrc32c === 'number'
      )
      )
    )
    || typeof payload.request !== 'string'
  ) {
    return undefined;
  }
  return {
    actorType: payload.actorType,
    actorId: typeof payload.actorId === 'string' ? payload.actorId : undefined,
    phase: payload.phase === REMOTE_ACTOR_JOIN_ADMISSION
      || payload.phase === REMOTE_ACTOR_JOIN_COMMIT
      || payload.phase === REMOTE_ACTOR_JOIN_ABORT
      ? payload.phase
      : undefined,
    transferId: payload.transferId,
    transferAdapterKey: typeof payload.transferAdapterKey === 'string'
      ? payload.transferAdapterKey
      : undefined,
    transferState: typeof payload.transferState === 'string'
      ? payload.transferState
      : undefined,
    transferStateReference: typeof payload.transferStateReference === 'string'
      ? payload.transferStateReference
      : undefined,
    transferStateChecksumCrc32c: typeof payload.transferStateChecksumCrc32c === 'number'
      ? payload.transferStateChecksumCrc32c
      : undefined,
    request: payload.request,
    actorEntryNodeRid: typeof payload.actorEntryNodeRid === 'string'
      ? decodeRoutingId(payload.actorEntryNodeRid, payload.actorEntryNodeRidHex)
      : undefined,
    actorRef: decodeFormalRemoteActorRef(payload),
    remoteBoundSessionTarget: decodeRemoteBoundSessionTarget(
      payload.boundSessionRouterChannelId,
      payload.boundSessionTargetNodeRid,
      payload.boundSessionTargetNodeRidHex,
      payload.boundSessionSpotId,
      payload.boundSessionNodeRid,
      payload.boundSessionNodeRidHex,
      payload.boundSessionRid,
      payload.boundSessionRidHex,
      payload.boundSessionBindingGeneration,
      payload.boundSessionPreviousAuthorityOwnerGeneration,
      payload.boundSessionPreviousOwnerLeaseGeneration,
      payload.boundSessionAcceptedHighWater,
      payload.boundSessionRelocationSealId,
      payload.boundSessionAcceptedJournalReference,
      payload.boundSessionAcceptedJournalChecksumCrc32c
    ),
    expectedMembershipEpoch: typeof payload.expectedMembershipEpoch === 'string'
      ? BigInt(payload.expectedMembershipEpoch)
      : 0n,
    handoffBacklog: decodeHandoffBacklog(payload.handoffBacklog),
    completionOperationId:
      typeof payload.completionOperationHigh === 'string'
      && typeof payload.completionOperationLow === 'string'
        ? {
            high: BigInt(payload.completionOperationHigh),
            low: BigInt(payload.completionOperationLow)
          }
        : undefined
  };
}

function decodeFormalRemoteActorRef(
  payload: ZLinkRemoteActorJoinWirePayload
): ActorRef | undefined {
  const backend = decodeRemoteActorRef(
    payload.actorNodeRid,
    payload.actorNodeRidHex,
    typeof payload.actorId === 'string' ? payload.actorId : '',
    payload.actorGeneration
  );
  if (backend === undefined || typeof payload.routerChannelId !== 'string') {
    return undefined;
  }
  return {
    actorId: backend.actorId,
    objectGeneration: backend.generation,
    meshName: payload.routerChannelId,
    nodeRid: backend.nodeRid as unknown as RoutingId
  };
}


function requireMeshSpotReply(result: number): void {
  if (result !== SubmitResult.Ok) {
    throw new ZLinkConfigurationException(
      `MeshNode reply was not accepted (submit result ${result}).`
    );
  }
}

function toFrameworkRemoteAdmissionActorRef(
  rawActorRef: {
    readonly actorId: string;
    readonly nodeRid: unknown;
    readonly objectGeneration?: bigint;
    readonly generation?: bigint;
  },
  meshName: string
): ActorRef {
  const objectGeneration = rawActorRef.objectGeneration ?? rawActorRef.generation;
  if (objectGeneration === undefined) {
    throw new ZLinkConfigurationException(
      `Actor '${rawActorRef.actorId}' join record has no ObjectGeneration.`
    );
  }
  return {
    actorId: rawActorRef.actorId,
    objectGeneration,
    meshName,
    nodeRid: rawActorRef.nodeRid as RoutingId
  };
}

function validateRemoteAdmissionCommit(
  admission: ZLinkFormalRemoteActorAdmissionRecord,
  request: ZLinkFormalRemoteTransferRequest,
  actorId: string,
  control: { readonly currentSpotGeneration?: bigint }
): void {
  const expected = admission.admission;
  const actorRef = request.actorRef;
  if (
    expected.actorId !== actorId
    || expected.actorType !== request.actorType
    || expected.expectedMembershipEpoch !== request.expectedMembershipEpoch
    || expected.requestFingerprint !== request.request
    || expected.targetSpotGeneration !== (control.currentSpotGeneration ?? 0n)
    || actorRef === undefined
    || actorRef.actorId !== expected.actorRef.actorId
    || String(actorRef.nodeRid) !== String(expected.actorRef.nodeRid)
    || actorRef.objectGeneration !== expected.actorRef.objectGeneration
  ) {
    throw new ZLinkConfigurationException(
      `Remote Actor '${actorId}' commit does not match its target admission.`
    );
  }
}

function normalizeSpotCreateArgs<TRequest>(
  requestOrSignal: ZLinkMessage | TRequest | AbortSignal | undefined,
  signal: AbortSignal | undefined
): { readonly request: ZLinkMessage | TRequest | undefined; readonly signal: AbortSignal | undefined } {
  if (isAbortSignal(requestOrSignal)) {
    return { request: undefined, signal: requestOrSignal };
  }
  return { request: requestOrSignal, signal };
}

function requireMeshName(meshName: string): void {
  if (meshName.length === 0) {
    throw new ZLinkConfigurationException('Spot operations require a mesh name.');
  }
}

function instanceMaterializationPrefix(meshName: string, spotId: RoutingId): string {
  return `${meshName}\0${String(spotId)}`;
}

function instanceMaterializationKey(
  meshName: string,
  spotId: RoutingId,
  objectGeneration: bigint
): string {
  return `${instanceMaterializationPrefix(meshName, spotId)}\0${objectGeneration}`;
}

function isAbortSignal(value: unknown): value is AbortSignal {
  return typeof value === 'object'
    && value !== null
    && typeof (value as { aborted?: unknown }).aborted === 'boolean'
    && typeof (value as { addEventListener?: unknown }).addEventListener === 'function';
}

function instanceActivationOutcome(error: unknown): string {
  if (isAbortError(error)) return 'shutdown';
  if (error instanceof ZLinkFrameworkException) {
    if (error.kind === ZLinkFrameworkErrorKind.DeadlineExceeded) return 'timed_out';
    if (error.kind === ZLinkFrameworkErrorKind.AlreadyExists) return 'conflict';
    if (
      error.kind === ZLinkFrameworkErrorKind.InvalidOperation
      || error.kind === ZLinkFrameworkErrorKind.Unavailable
    ) return 'fenced';
  }
  return 'failed';
}
