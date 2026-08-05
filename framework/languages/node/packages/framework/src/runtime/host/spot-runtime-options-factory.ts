import type { ZLinkRuntimeEventPublisher } from '../diagnostics';
import {
  ZLinkSpotRelocationReadinessMode,
  ZLinkUserSpotExecutionMode
} from '../../contracts';
import {
  toFrameworkActorRef,
  type DefaultZLinkActorManager,
  type ZLinkActorHandoffCoordinator
} from '../actors';
import {
  DefaultZLinkChannelClient,
  DefaultZLinkFanoutClient,
  DefaultZLinkSpotPublisherClient,
  type ZLinkChannelClientTransport,
  type ZLinkRouteClientTransport,
  type ZLinkDispatchErrorSink,
  type ZLinkSpotPublisherClientTransport
} from '../channels';
import type { ZLinkDispatchErrorReporter } from '../channels';
import {
  ZLinkConfigurationException,
  type ZLinkFrameworkRegistration
} from '../configuration';
import type { ZLinkLocationLifecycle } from '../locations';
import type { ZLinkBackendMeshNode } from '../backend';
import type { ZLinkSpotRouteResolver } from '../spots/spot-routing-internal';
import type {
  ZLinkDetachedTaskRunner,
  ZLinkSpotManagerOptions,
  ZLinkSpotNodeRuntimeManager,
  ZLinkSpotAddressTransport,
  ZLinkSpotRoutedTransport
} from '../spots';
import type { ZLinkActorTransferRuntime } from './actor-transfer-runtime';
import type { ZLinkBoundSessionRelay } from './bound-session-relay';
import type { MeshRouterResolver } from './mesh-router-resolver';
import type { ZLinkRuntimeAdmissionGate } from '../admission';

export interface ZLinkSpotRuntimeOptionsFactoryOptions {
  readonly registration: ZLinkFrameworkRegistration;
  readonly channelTransport: ZLinkChannelClientTransport;
  readonly routeTransport: ZLinkRouteClientTransport & ZLinkSpotRoutedTransport;
  readonly addressTransport: ZLinkSpotAddressTransport;
  readonly spotPublisherTransport: ZLinkSpotPublisherClientTransport;
  readonly meshRouters: MeshRouterResolver;
  readonly runtimeEventPublisher: ZLinkRuntimeEventPublisher;
  readonly spotNodeRuntime: () => ZLinkSpotNodeRuntimeManager | undefined;
  readonly actorManager: () => DefaultZLinkActorManager | undefined;
  readonly locationLifecycle: () => ZLinkLocationLifecycle | undefined;
  readonly releaseInstanceAuthority: (
    meshName: string,
    spotId: string,
    objectGeneration: bigint
  ) => Promise<void>;
  readonly beginInstanceIdleClosingAuthority: (
    meshName: string,
    spotId: string
  ) => Promise<boolean>;
  readonly beginInstanceClosingAuthority: (
    meshName: string,
    spotId: string
  ) => Promise<boolean>;
  readonly createLocationSpotRouteResolver: () => ZLinkSpotRouteResolver | undefined;
  readonly boundSessionRelay: ZLinkBoundSessionRelay;
  readonly actorHandoff: ZLinkActorHandoffCoordinator;
  readonly dispatchErrorReporter: (errorSink: ZLinkDispatchErrorSink) => ZLinkDispatchErrorReporter;
  readonly runtimeOrPreStartErrorSink: ZLinkDispatchErrorSink;
  readonly detachedTaskRunner: ZLinkDetachedTaskRunner;
  readonly metrics: import('../diagnostics').ZLinkRuntimeMetrics;
  readonly admission: ZLinkRuntimeAdmissionGate;
}

export class ZLinkSpotRuntimeOptionsFactory {
  constructor(private readonly options: ZLinkSpotRuntimeOptionsFactoryOptions) {}

  create(actorTransferRuntime: ZLinkActorTransferRuntime): Partial<ZLinkSpotManagerOptions> {
    return {
      nodeRid: undefined,
      nodeRidProvider: (meshName) => this.meshNodeRoutingId(meshName),
      nodeGenerationProvider: (meshName) =>
        this.options.spotNodeRuntime()?.meshNode(meshName)?.status().lifecycleGeneration,
      entryNodeRid: undefined,
      entryNodeRidProvider: () => this.primaryNodeRoutingId(),
      entrySpotIdProvider: meshName =>
        this.options.spotNodeRuntime()?.entrySpotIdForMesh(meshName),
      entrySpotCallbacks: {
        onLeaveActor: (actor, signal, actorRef, membershipEpoch) =>
          this.options.spotNodeRuntime()?.notifyPrimaryEntrySpotActorLeft(
            actor,
            signal,
            actorRef,
            membershipEpoch
          ) ?? Promise.resolve()
      },
      dispatchEntryActorPacket: (
        actorId,
        parts,
        returnResponse,
        remoteBoundSessionTarget,
        fallbackActorRef,
        requestTerminal,
        messageFollowOrigin
      ) => {
        const runtime = this.options.spotNodeRuntime();
        if (runtime === undefined) {
          throw new ZLinkConfigurationException('Entry Spot actor packet dispatch requires the MeshNode runtime.');
        }
        return runtime.dispatchEntryActorPacket(
          actorId,
          parts,
          returnResponse,
          remoteBoundSessionTarget,
          fallbackActorRef,
          requestTerminal,
          messageFollowOrigin
        );
      },
      dispatchEntryActorJoin: async (meshName, actor, handoffBacklog) => {
        const runtime = this.options.spotNodeRuntime();
        if (runtime === undefined) {
          throw new ZLinkConfigurationException('Entry Spot actor join requires the MeshNode runtime.');
        }
        await runtime.dispatchEntryActorJoin(meshName, actor, handoffBacklog);
      },
      channelClient: new DefaultZLinkChannelClient(this.options.registration, this.options.channelTransport),
      fanoutClient: new DefaultZLinkFanoutClient(this.options.registration, this.options.channelTransport),
      spotPublisherClient: new DefaultZLinkSpotPublisherClient(
        this.options.registration,
        this.options.spotPublisherTransport
      ),
      routedTransport: this.options.routeTransport,
      addressTransport: this.options.addressTransport,
      spotRouterChannelIdForMesh: this.options.meshRouters.spotRouterChannelIdByMesh(),
      channelMeshNameForChannel: (channelName) => {
        const matches = [...this.options.registration.spotNodes.entries()]
          .filter(([, node]) => Object.prototype.hasOwnProperty.call(node.meshChannels ?? {}, channelName))
          .map(([meshName]) => meshName);
        return matches.length === 1 ? matches[0] : undefined;
      },
      messageSerializers: this.options.registration.messageSerializers,
      runtimeEventPublisher: this.options.runtimeEventPublisher,
      detachedTaskRunner: this.options.detachedTaskRunner,
      locationLifecycle: this.options.locationLifecycle(),
      releaseInstanceAuthority: (meshName, spotId, objectGeneration) =>
        this.options.releaseInstanceAuthority(meshName, String(spotId), objectGeneration),
      beginInstanceIdleClosingAuthority: (meshName, spotId) =>
        this.options.beginInstanceIdleClosingAuthority(meshName, String(spotId)),
      beginInstanceClosingAuthority: (meshName, spotId) =>
        this.options.beginInstanceClosingAuthority(meshName, String(spotId)),
      instanceSpotApplicationTargetProvider: (meshName, spotId) =>
        this.options.spotNodeRuntime()
          ?.meshNode(meshName)
          ?.instanceSpotApplicationTarget?.(String(spotId)),
      instanceSpotApplicationQuiescenceProvider: (meshName, spotId, signal) =>
        this.options.spotNodeRuntime()
          ?.meshNode(meshName)
          ?.waitForInstanceApplicationQuiescence?.(String(spotId), signal)
        ?? Promise.resolve(),
      createNativeSpot: (meshName, spotId, authority) => {
        const node = this.options.spotNodeRuntime()?.meshNode(meshName);
        if (node === undefined) {
          return undefined;
        }
        const restored = authority === undefined
          ? undefined
          : node.restoreSpotAuthority?.(
              String(spotId),
              authority.objectKind ?? 'user_spot',
              authority.stableType,
              authority.objectGeneration,
              authority.authorityOwnerGeneration
            ) ?? node.restoreUserSpotAuthority?.(
                String(spotId),
                authority.stableType,
                authority.objectGeneration,
                authority.authorityOwnerGeneration
              );
        const result = restored === undefined
          ? node.getOrCreateSpot(spotId)
          : { spot: restored, created: true };
        return {
          routingId: spotId,
          lifecycleGeneration: result.spot.status().lifecycleGeneration,
          setSubscription: (channelName: string, topic: string) =>
            result.spot.setSubscription(channelName, topic),
          dispose: () => {
            if (result.created) result.spot.close();
          }
        } as never;
      },
      createReceived: () => {
        const runtime = this.options.spotNodeRuntime();
        if (runtime === undefined) throw new Error('Spot backend runtime is not initialized.');
        return runtime.createReceived();
      },
      createTopicMessage: () => {
        const runtime = this.options.spotNodeRuntime();
        if (runtime === undefined) throw new Error('Spot backend runtime is not initialized.');
        return runtime.createTopicMessage();
      },
      nativeSpotNodeProvider: (meshName) =>
        this.options.spotNodeRuntime()?.meshNode(meshName) as never,
      spotRouteResolver: this.options.createLocationSpotRouteResolver(),
      actorResolver: (actorId) => {
        const state = this.options.actorManager()?.getState(actorId);
        return state?.isMoving === true ? undefined : state?.actor;
      },
      actorLifecycleResolver: (actorId) =>
        this.options.actorManager()?.getState(actorId)?.actor,
      actorDispatchOwnerResolver: (actorId) => {
        const state = this.options.actorManager()?.getState(actorId);
        const actorRef = state?.nativeActorRef === undefined
          ? undefined
          : toFrameworkActorRef(state.nativeActorRef, state.meshName ?? '');
        const localNodeRid = this.primaryNodeRoutingId();
        if (
          state?.actor === undefined
          || state.spot === undefined
          || state.remoteActorPacketTarget !== undefined
          || actorRef === undefined
          || localNodeRid === undefined
          || String(actorRef.nodeRid) !== localNodeRid
        ) {
          return {};
        }
        return {
          actorRef,
          spotId: state.spotId
        };
      },
      userSpotExecutionMode: (meshName, spotType) => {
        const registrations =
          this.options.registration.spotNodes.get(meshName)
            ?.spotFactoryRegistrations ?? {};
        const registration = Object.values(registrations)
          .find(candidate => candidate.implementation === spotType);
        return registration?.options?.executionMode
          ?? ZLinkUserSpotExecutionMode.SpotWide;
      },
      userSpotRelocationReadiness: (meshName, spotType) => {
        const registrations =
          this.options.registration.spotNodes.get(meshName)
            ?.spotFactoryRegistrations ?? {};
        const registration = Object.values(registrations)
          .find(candidate => candidate.implementation === spotType);
        return registration?.options?.relocationReadiness
          ?? ZLinkSpotRelocationReadinessMode.AnyTurnBoundary;
      },
      actorBindingGenerationObserver: (actorId, generation) =>
        this.options.actorManager()?.getState(actorId)?.setBoundSessionBindingGeneration(generation),
      actorTransferRuntime,
      boundSessionRuntime: this.options.boundSessionRelay.boundSessions,
      actorHandoffRuntime: this.options.actorHandoff,
      metrics: this.options.metrics,
      admission: this.options.admission,
      dispatchErrors: this.options.dispatchErrorReporter(this.options.runtimeOrPreStartErrorSink)
    };
  }

  private primaryNode(): ZLinkBackendMeshNode | undefined {
    return this.options.spotNodeRuntime()?.primaryMeshNode;
  }

  private primaryNodeRoutingId(): string | undefined {
    const node = this.primaryNode();
    return node === undefined ? undefined : String(node.status().routingId);
  }

  private meshNodeRoutingId(meshName: string): string | undefined {
    const node = this.options.spotNodeRuntime()?.meshNode(meshName);
    return node === undefined ? undefined : String(node.status().routingId);
  }

}
