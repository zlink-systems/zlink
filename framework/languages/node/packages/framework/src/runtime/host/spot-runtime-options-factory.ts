import type { ZLinkRuntimeEventPublisher } from '../diagnostics';
import { ZLinkSpotRelocationCoordinationMode, ZLinkUserSpotExecutionMode } from '../../contracts';
import {
  toFrameworkActorRef,
  type DefaultZLinkActorManager,
  type ZLinkActorHandoffCoordinator
} from '../actors';
import {
  DefaultZLinkChannelClient,
  DefaultZLinkFanoutClient,
  DefaultZLinkSpotPublisherClient,
  type ZLinkChannelClientTransportSource,
  type ZLinkRouteClientTransport,
  type ZLinkDispatchErrorSink,
  type ZLinkSpotPublisherClientTransport
} from '../channels';
import type { ZLinkDispatchErrorReporter } from '../channels';
import { ZLinkConfigurationException, type ZLinkFrameworkRegistration } from '../configuration';
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
  readonly channelTransport: ZLinkChannelClientTransportSource;
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
    spotId: string,
    onCommitted: () => void
  ) => Promise<
    import('../locations/spot-location-claims').ZLinkInstanceClosingAuthority | undefined
  >;
  readonly beginInstanceClosingAuthority: (
    meshName: string,
    spotId: string,
    onCommitted: () => void
  ) => Promise<
    import('../locations/spot-location-claims').ZLinkInstanceClosingAuthority | undefined
  >;
  readonly beginUserClosingAuthority: (
    meshName: string,
    spotId: string,
    objectGeneration: bigint,
    onCommitted: () => void
  ) => Promise<
    import('../locations/spot-location-claims').ZLinkInstanceClosingAuthority | undefined
  >;
  readonly createLocationSpotRouteResolver: () => ZLinkSpotRouteResolver | undefined;
  readonly boundSessionRelay: ZLinkBoundSessionRelay;
  readonly actorHandoff: ZLinkActorHandoffCoordinator;
  readonly dispatchErrorReporter: (errorSink: ZLinkDispatchErrorSink) => ZLinkDispatchErrorReporter;
  readonly runtimeOrPreStartErrorSink: ZLinkDispatchErrorSink;
  readonly detachedTaskRunner: ZLinkDetachedTaskRunner;
  readonly metrics: import('../diagnostics').ZLinkRuntimeMetrics;
  readonly admission: ZLinkRuntimeAdmissionGate;
  readonly statefulExecution: NonNullable<ZLinkSpotManagerOptions['statefulExecution']>;
}

export class ZLinkSpotRuntimeOptionsFactory {
  constructor(private readonly options: ZLinkSpotRuntimeOptionsFactoryOptions) {}

  create(actorTransferRuntime: ZLinkActorTransferRuntime): Partial<ZLinkSpotManagerOptions> {
    const spotRouterChannelIdForMesh = this.options.meshRouters.spotRouterChannelIdByMesh();
    return {
      nodeRid: undefined,
      nodeRidProvider: (meshName) => this.meshNodeRoutingId(meshName),
      nodeGenerationProvider: (meshName) =>
        this.options.spotNodeRuntime()?.meshNode(meshName)?.status().lifecycleGeneration,
      entryNodeRid: undefined,
      entryNodeRidProvider: () => this.primaryNodeRoutingId(),
      entrySpotIdProvider: (meshName) =>
        this.options.spotNodeRuntime()?.entrySpotIdForMesh(meshName),
      entrySpotCallbacks: {
        onLeaveActor: (actor, signal, actorRef, membershipEpoch) =>
          this.options
            .spotNodeRuntime()
            ?.notifyPrimaryEntrySpotActorLeft(actor, signal, actorRef, membershipEpoch) ??
          Promise.resolve()
      },
      dispatchEntryActorPacket: (
        origin,
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
          throw new ZLinkConfigurationException(
            'Entry Spot actor packet dispatch requires the MeshNode runtime.'
          );
        }
        return runtime.dispatchEntryActorPacket(
          origin,
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
          throw new ZLinkConfigurationException(
            'Entry Spot actor join requires the MeshNode runtime.'
          );
        }
        await runtime.dispatchEntryActorJoin(meshName, actor, handoffBacklog);
      },
      executeEntryActor: (meshName, actorId, operation) => {
        const runtime = this.options.spotNodeRuntime();
        if (runtime === undefined) {
          throw new ZLinkConfigurationException(
            'Entry Spot Actor completion requires the MeshNode runtime.'
          );
        }
        return runtime.executeEntryActor(meshName, actorId, operation);
      },
      channelClient: new DefaultZLinkChannelClient(
        this.options.registration,
        this.options.channelTransport,
        this.options.routeTransport,
        spotRouterChannelIdForMesh
      ),
      fanoutClient: new DefaultZLinkFanoutClient(
        this.options.registration,
        this.options.channelTransport
      ),
      spotPublisherClient: new DefaultZLinkSpotPublisherClient(
        this.options.registration,
        this.options.spotPublisherTransport
      ),
      routedTransport: this.options.routeTransport,
      addressTransport: this.options.addressTransport,
      spotRouterChannelIdForMesh,
      messageSerializers: this.options.registration.messageSerializers,
      runtimeEventPublisher: this.options.runtimeEventPublisher,
      detachedTaskRunner: this.options.detachedTaskRunner,
      locationLifecycle: this.options.locationLifecycle(),
      releaseInstanceAuthority: (meshName, spotId, objectGeneration) =>
        this.options.releaseInstanceAuthority(meshName, String(spotId), objectGeneration),
      beginInstanceIdleClosingAuthority: (meshName, spotId, onCommitted) =>
        this.options.beginInstanceIdleClosingAuthority(meshName, String(spotId), onCommitted),
      beginInstanceClosingAuthority: async (meshName, spotId, onCommitted, objectGeneration) => {
        const authority = await this.options.beginInstanceClosingAuthority(
          meshName,
          String(spotId),
          onCommitted
        );
        if (authority === undefined) return undefined;
        // A Close without Reincarnate ends this incarnation. The Instance authority
        // release also forgets the node's Ready projection, so a request on a cached
        // Ready route is refused instead of rematerializing the closed generation.
        const release = () =>
          this.options.releaseInstanceAuthority(meshName, String(spotId), objectGeneration);
        if (authority.reincarnate === undefined) return { release };
        return {
          release,
          reincarnate: async (initialize) => {
            const current = await authority.reincarnate!(initialize);
            const node = this.options.spotNodeRuntime()?.meshNode(meshName);
            const route = {
              targetSpotId: String(spotId),
              targetNodeRid: String(current.allocation.descriptor.rid),
              targetNodeGeneration: current.allocation.descriptorLifecycleGeneration,
              objectGeneration: current.objectGeneration,
              authorityOwnerGeneration: current.authorityOwnerGeneration,
              ownerId: current.ownerId,
              leaseGeneration: current.ownerLeaseGeneration,
              storeVersion: current.storeVersion.value
            };
            node?.restoreSpotAuthority?.(
              String(spotId),
              'instance_spot',
              current.stableType,
              current.objectGeneration,
              current.authorityOwnerGeneration
            );
            node?.registerInstanceIntent?.(current.stableType, route);
            node?.rememberSpotRoute?.({
              spot: { spotId: String(spotId), generation: current.objectGeneration },
              targetNodeRid: route.targetNodeRid,
              targetNodeGeneration: route.targetNodeGeneration,
              authorityOwnerGeneration: route.authorityOwnerGeneration,
              ownerLeaseGeneration: route.leaseGeneration,
              storeVersion: route.storeVersion
            });
            return current;
          }
        };
      },
      beginUserClosingAuthority: (meshName, spotId, objectGeneration, onCommitted) =>
        this.options.beginUserClosingAuthority(
          meshName,
          String(spotId),
          objectGeneration,
          onCommitted
        ),
      instanceSpotApplicationTargetProvider: (meshName, spotId) =>
        this.options
          .spotNodeRuntime()
          ?.meshNode(meshName)
          ?.instanceSpotApplicationTarget?.(String(spotId)),
      instanceSpotApplicationQuiescenceProvider: (meshName, spotId, signal) =>
        this.options
          .spotNodeRuntime()
          ?.meshNode(meshName)
          ?.waitForInstanceApplicationQuiescence?.(String(spotId), signal) ?? Promise.resolve(),
      createNativeSpot: (meshName, spotId, authority) => {
        const node = this.options.spotNodeRuntime()?.meshNode(meshName);
        if (node === undefined) {
          return undefined;
        }
        const restored =
          authority === undefined
            ? undefined
            : (node.restoreSpotAuthority?.(
                String(spotId),
                authority.objectKind ?? 'user_spot',
                authority.stableType,
                authority.objectGeneration,
                authority.authorityOwnerGeneration
              ) ??
              node.restoreUserSpotAuthority?.(
                String(spotId),
                authority.stableType,
                authority.objectGeneration,
                authority.authorityOwnerGeneration
              ));
        const result =
          restored === undefined ? node.getOrCreateSpot(spotId) : { spot: restored, created: true };
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
      canonicalActorJoinResolver: (fence) =>
        this.options.boundSessionRelay.actorJoins.prepareCanonicalActorJoin({
          actorId: fence.actorId,
          actorNodeRid: fence.actorNodeRid,
          actorGeneration: fence.actorGeneration,
          actorNodeGeneration: fence.actorNodeGeneration,
          expectedAuthorityOwnerGeneration: fence.authorityOwnerGeneration,
          expectedOwnerLeaseGeneration: fence.ownerLeaseGeneration
        }),
      actorLifecycleResolver: (actorId) => this.options.actorManager()?.getState(actorId)?.actor,
      actorDispatchOwnerResolver: (actorId) => {
        const state = this.options.actorManager()?.getState(actorId);
        const actorRef =
          state?.nativeActorRef === undefined
            ? undefined
            : toFrameworkActorRef(state.nativeActorRef, state.meshName ?? '');
        const localNodeRid = this.primaryNodeRoutingId();
        if (
          state?.actor === undefined ||
          state.spot === undefined ||
          state.remoteActorPacketTarget !== undefined ||
          actorRef === undefined ||
          localNodeRid === undefined ||
          String(actorRef.nodeRid) !== localNodeRid
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
          this.options.registration.spotNodes.get(meshName)?.spotFactoryRegistrations ?? {};
        const registration = Object.values(registrations).find(
          (candidate) => candidate.implementation === spotType
        );
        return registration?.options?.executionMode ?? ZLinkUserSpotExecutionMode.SpotWide;
      },
      userSpotRelocationCoordinationMode: (meshName, spotType) => {
        const registrations =
          this.options.registration.spotNodes.get(meshName)?.spotFactoryRegistrations ?? {};
        const registration = Object.values(registrations).find(
          (candidate) => candidate.implementation === spotType
        );
        return (
          registration?.options?.relocationCoordinationMode ??
          ZLinkSpotRelocationCoordinationMode.FrameworkManaged
        );
      },
      actorTransferRuntime,
      boundSessionRuntime: this.options.boundSessionRelay.boundSessions,
      actorHandoffRuntime: this.options.actorHandoff,
      metrics: this.options.metrics,
      admission: this.options.admission,
      statefulExecution: this.options.statefulExecution,
      closeErrorSink: this.options.runtimeOrPreStartErrorSink,
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
