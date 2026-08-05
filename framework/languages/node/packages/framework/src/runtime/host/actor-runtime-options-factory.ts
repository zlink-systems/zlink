import type {
  ActorRef,
  RoutingId,
  ZLinkActor,
  ZLinkMessage
} from '../../contracts';
import type { ZLinkProviderResolver } from '../../contracts/Common/ZLinkProviderResolver';
import type { Type } from '../../contracts/Common/CoreTypes';
import {
  meshActorSessionNodeAdapter,
  type ZLinkBackendMeshNode,
  type ZLinkMeshCompletionTable
} from '../backend';
import type { ZLinkFrameworkRegistration } from '../configuration';
import type { DefaultZLinkActorManager } from '../actors';
import type { DefaultZLinkSpotManager } from '../spots';
import type { ZLinkSpotRouteResolver } from '../spots/spot-routing-internal';
import {
  DefaultZLinkActorClient,
  ZLinkActorNativeJoinCoordinator,
  type ZLinkActorTransferRegistry,
  type ZLinkActorManagerOptions
} from '../actors';
import type { ZLinkActorHandoffCoordinator } from '../actors';
import { type ZLinkActorRoutedJoinTransport } from '../actors';
import {
  ZLINK_INTERNAL_ACTOR_TRANSPORT_DELIVERY_GATE,
  type ZLinkInternalActorTransportDeliveryGate
} from '../actors/actor-transport-delivery-gate';
import type { ZLinkLocationLifecycle, ZLinkStoreLocationResolvers } from '../locations';
import type {
  ZLinkNativeFallbackBoundSessionPort,
  ZLinkStreamActorLifecyclePort
} from '../streams/stream-binding-runtime-ports';
import { ZLinkNativeFallbackBoundSession } from '../streams/native-fallback-bound-session';
import type { ZLinkActorTransferRuntime } from './actor-transfer-runtime';
import type { ZLinkRuntimeAdmissionGate } from '../admission';
import type { ZLinkRemoteActorPacketTarget } from '../actors';
import type { ZLinkMeshSubmitterRegistry } from '../messaging';

export interface ZLinkActorRuntimeOptionsFactoryOptions {
  readonly registration: ZLinkFrameworkRegistration;
  readonly routeTransport: ZLinkActorRoutedJoinTransport;
  readonly streamBindingRuntime: ZLinkStreamActorLifecyclePort & ZLinkNativeFallbackBoundSessionPort;
  readonly providerResolver?: ZLinkProviderResolver;
  readonly spotManager: () => DefaultZLinkSpotManager | undefined;
  readonly actorManager: () => DefaultZLinkActorManager | undefined;
  readonly primaryMeshNode: () => ZLinkBackendMeshNode;
  readonly primaryMeshNodeOrUndefined: () => ZLinkBackendMeshNode | undefined;
  readonly primaryMeshCompletions: () => ZLinkMeshCompletionTable | undefined;
  readonly meshNode: (meshName: string) => ZLinkBackendMeshNode | undefined;
  readonly meshCompletions: (meshName: string) => ZLinkMeshCompletionTable | undefined;
  readonly notifyEntrySpotActorCreated: (
    nodeRid: RoutingId,
    actor: ZLinkActor,
    createRequest: ZLinkMessage,
    signal?: AbortSignal
  ) => Promise<import('../../contracts').ZLinkActorCreateResponse | undefined>;
  readonly locationLifecycle: () => ZLinkLocationLifecycle | undefined;
  readonly primaryMeshName: () => string | undefined;
  readonly actorMeshName: (actorType: string) => string | undefined;
  readonly createLocationSpotRouteResolver: () => ZLinkSpotRouteResolver | undefined;
  readonly createActorLocationResolver: () => ZLinkStoreLocationResolvers | undefined;
  readonly forgetDestroyedActorRef: (actorId: string) => void;
  readonly rememberDestroyedActorRef: (actorId: string, actorRef: ActorRef) => void;
  /**
   * Drops a cached direct route when the local Actor incarnation changes.
   * Authority deletion and recreation are separate store mutations, so a
   * positive route cache must not bridge the two incarnations.
   */
  readonly invalidateActorRoute: (actorId: string) => void;
  readonly publishActorAuthority: NonNullable<
    ZLinkActorManagerOptions['publishActorAuthority']
  >;
  readonly reportPostCommitError: (error: unknown) => void;
  readonly reportBoundSessionSendError: (error: unknown) => void;
  readonly actorHandoff: ZLinkActorHandoffCoordinator;
  readonly actorTransferRuntime: ZLinkActorTransferRuntime;
  readonly actorTransferRegistry: ZLinkActorTransferRegistry;
  readonly shutdownSignal: () => AbortSignal | undefined;
  readonly metrics: import('../diagnostics').ZLinkRuntimeMetrics;
  readonly traceBoundSessionSend?: (actorId: string, packetName: string) => void;
  readonly flowCreationEnabled?: () => boolean;
  readonly admission: ZLinkRuntimeAdmissionGate;
  readonly actorPacketTargetForState: (actorId: string) => ZLinkRemoteActorPacketTarget | undefined;
  readonly meshSubmitters: ZLinkMeshSubmitterRegistry;
}

export class ZLinkActorRuntimeOptionsFactory {
  constructor(private readonly options: ZLinkActorRuntimeOptionsFactoryOptions) {}

  createActorManagerOptions(_spotRouteResolver?: ZLinkSpotRouteResolver): Pick<
    ZLinkActorManagerOptions,
    | 'joinCoordinator'
    | 'actorMeshNameProvider'
    | 'actorLeaveSpot'
    | 'messageSerializers'
    | 'nativeActorNode'
    | 'nativeActorNodeProvider'
    | 'nativeActorCompletionTableProvider'
    | 'actorCreatedNodeRidProvider'
    | 'actorRefResolver'
    | 'actorCreatedNotifier'
    | 'actorDestroyedCleanup'
    | 'publishActorAuthority'
    | 'locationLifecycle'
    | 'boundSessionFactory'
    | 'actorTransferRegistry'
    | 'shutdownSignal'
    | 'metrics'
    | 'admission'
  > {
    const actorTransferRegistry = this.options.actorTransferRegistry;
    return {
      actorMeshNameProvider: this.options.actorMeshName,
      actorLeaveSpot: (meshName, spotId, actor, signal) => {
        const spotManager = this.options.spotManager();
        if (spotManager === undefined) {
          throw new Error('Actor Spot lifecycle runtime is not started.');
        }
        return spotManager.leaveActorInMesh(meshName, spotId, actor, signal);
      },
      joinCoordinator: new ZLinkActorNativeJoinCoordinator({
        node: this.options.primaryMeshNode,
        completionTableProvider: this.options.primaryMeshCompletions,
        spotRouteResolver: this.options.createLocationSpotRouteResolver(),
        locationLifecycle: this.options.locationLifecycle(),
        postCommitErrorReporter: this.options.reportPostCommitError,
        sourceTransfer: this.options.actorTransferRuntime,
        actorLocationResolver: this.options.createActorLocationResolver,
        entrySpotIdProvider: meshName => {
          const resolvedMeshName = meshName ?? this.options.primaryMeshName();
          return resolvedMeshName === undefined
            ? undefined
            : this.options.spotManager()?.entrySpotIdForMesh(resolvedMeshName);
        },
        remoteActorBinder: (actorRef, signal) =>
          this.options.streamBindingRuntime.commitActorRoute(actorRef, signal),
        routedTransport: this.options.routeTransport,
        messageSerializers: this.options.registration.messageSerializers,
        actorTransferTimeoutMs: this.options.registration.actorTransferTimeoutMs,
        shutdownSignal: this.options.shutdownSignal()
      }),
      messageSerializers: this.options.registration.messageSerializers,
      actorTransferRegistry,
      shutdownSignal: this.options.shutdownSignal(),
      metrics: this.options.metrics,
      admission: this.options.admission,
      nativeActorNodeProvider: this.options.primaryMeshNodeOrUndefined,
      nativeActorCompletionTableProvider: this.options.primaryMeshCompletions,
      locationLifecycle: this.options.locationLifecycle(),
      boundSessionFactory: (actorId) => new ZLinkNativeFallbackBoundSession({
        runtime: this.options.streamBindingRuntime,
        routedTransport: this.options.routeTransport,
        actorRefProvider: () => {
          const state = this.options.actorManager()?.getState(actorId);
          const actorRef = state?.nativeActorRef;
          const meshName = state?.meshName
            ?? (state?.actorType === undefined
              ? undefined
              : this.options.actorMeshName(state.actorType));
          const objectGeneration = actorRef?.generation
            ?? (actorRef as unknown as { readonly objectGeneration?: bigint } | undefined)
              ?.objectGeneration;
          return actorRef === undefined || objectGeneration === undefined
            ? undefined
            : {
                actorId: actorRef.actorId,
                objectGeneration,
                meshName: meshName ?? this.options.primaryMeshName() ?? '',
                nodeRid: actorRef.nodeRid,
                ownershipGeneration: state?.locationGeneration,
                bindingGeneration: state?.boundSessionBindingGeneration
              } as ActorRef;
        },
        nativeActorNodeProvider: () => {
          const state = this.options.actorManager()?.getState(actorId);
          const meshName = state?.meshName
            ?? (state?.actorType === undefined
              ? undefined
              : this.options.actorMeshName(state.actorType));
          const node = meshName === undefined
            ? this.options.primaryMeshNodeOrUndefined()
            : this.options.meshNode(meshName);
          const completions = meshName === undefined
            ? this.options.primaryMeshCompletions()
            : this.options.meshCompletions(meshName);
          return node === undefined
            ? undefined
            : meshActorSessionNodeAdapter(node, completions);
        },
        localActorProvider: () => this.options.actorManager()?.getState(actorId)?.actor !== undefined,
        remoteBoundSessionTargetProvider: () => {
          const state = this.options.actorManager()?.getState(actorId);
          return state?.remoteBoundSessionTarget ?? state?.boundSessionTransferTarget;
        },
        remoteActorPacketTargetProvider: () => this.options.actorPacketTargetForState(actorId),
        requestTimeoutMs: this.options.registration.requestTimeoutMs,
        actorId,
        onSend: this.options.traceBoundSessionSend,
        reportError: this.options.reportBoundSessionSendError,
        flowCreationEnabled: this.options.flowCreationEnabled
      }),
      actorCreatedNodeRidProvider: () => {
        const node = this.options.primaryMeshNodeOrUndefined();
        return node === undefined ? undefined : String(node.status().routingId);
      },
      actorRefResolver: {
        resolveActorRef: async (actorId, signal) =>
          await this.options.createActorLocationResolver()?.resolveActorRef(actorId, signal)
      },
      actorCreatedNotifier: (nodeRid, actor, createRequest, signal) => {
        this.options.forgetDestroyedActorRef(actor.context.actorId);
        return this.options.notifyEntrySpotActorCreated(nodeRid, actor, createRequest, signal);
      },
      actorDestroyedCleanup: (actorId) => {
        this.options.invalidateActorRoute(actorId);
        const actorRef = this.options.actorManager()?.getState(actorId)?.nativeActorRef as ActorRef | undefined;
        if (actorRef !== undefined) {
          this.options.rememberDestroyedActorRef(actorId, actorRef);
        }
        this.options.streamBindingRuntime.unbindActor(actorId);
      },
      publishActorAuthority: this.options.publishActorAuthority
    };
  }

  createActorClientOptions(): ConstructorParameters<typeof DefaultZLinkActorClient>[0] {
    const locationResolver = this.options.createActorLocationResolver();
    return {
      nodeProvider: this.options.meshNode,
      completionTableProvider: this.options.meshCompletions,
      locationResolver: () => locationResolver,
      routeTransport: this.options.routeTransport,
      transportDeliveryGate: () => this.options.providerResolver?.get?.(
        ZLINK_INTERNAL_ACTOR_TRANSPORT_DELIVERY_GATE as unknown as
          Type<ZLinkInternalActorTransportDeliveryGate>
      ),
      messageSerializers: this.options.registration.messageSerializers,
      defaultRequestTimeoutMs: this.options.registration.requestTimeoutMs,
      staleActorRefReporter: (_meshName, actorId) => this.options.actorHandoff.recordStaleFailure(actorId),
      staleActorRefPredicate: (meshName, actor) =>
        this.actorBelongsToMesh(meshName, actor.actorId)
        && this.options.actorHandoff.isKnownStale(actor),
      handoffCapture: (meshName, actorId, parts, returnResponse, actor, deadlineUnixMs) =>
        this.actorBelongsToMesh(meshName, actorId)
          && !this.options.actorHandoff.isProvisional(actorId)
          ? this.options.actorHandoff.capture(
            actorId,
            parts,
            returnResponse,
            undefined,
            actor,
            deadlineUnixMs
          )
          : undefined,
      sendErrorReporter: this.options.reportPostCommitError,
      meshSubmitters: this.options.meshSubmitters
    };
  }

  private actorBelongsToMesh(meshName: string, actorId: string): boolean {
    const actorType = this.options.actorManager()?.getState(actorId)?.actorType;
    if (actorType !== undefined) {
      return this.options.actorMeshName(actorType) === meshName;
    }
    const actorMeshes = [...this.options.registration.spotNodes.entries()]
      .filter(([, node]) => node.actorFactories instanceof Map
        ? node.actorFactories.size > 0
        : Object.keys(node.actorFactories ?? {}).length > 0)
      .map(([name]) => name);
    return actorMeshes.length === 1 && actorMeshes[0] === meshName;
  }

}
