import type {
} from '../../contracts';
import type { ZLinkProviderResolver } from '../../contracts/Common/ZLinkProviderResolver';
import type { ZLinkRuntimeEventPublisher } from '../diagnostics';
import {
  DefaultZLinkChannelClient,
  DefaultZLinkFanoutClient,
  DefaultZLinkSpotPublisherClient,
  type ZLinkChannelClientTransport,
  type ZLinkRouteClientTransport,
  type ZLinkSpotPublisherClientTransport
} from '../channels';
import type { ZLinkDispatchErrorReporter } from '../channels';
import type { ZLinkFrameworkRegistration } from '../configuration';
import type { ZLinkBackendAdapterFactory, ZLinkBackendContext } from '../backend';
import type {
  ZLinkSpotNodeRuntimeManagerOptions,
  ZLinkDetachedTaskRunner,
  ZLinkSpotRoutedTransport
} from '../spots';
import type { ZLinkEntryActorRuntime, ZLinkSpotActorTransferRuntime } from '../spots/spot-runtime-ports';
import type { MeshRouterResolver } from './mesh-router-resolver';
import type { ZLinkBoundSessionRelay } from './bound-session-relay';
import type { ZLinkActorHandoffCoordinator } from '../actors';
import type { ZLinkRuntimeMetrics } from '../diagnostics';

export interface ZLinkSpotNodeRuntimeOptionsFactoryOptions {
  readonly registration: ZLinkFrameworkRegistration;
  readonly backendAdapterFactory: ZLinkBackendAdapterFactory;
  readonly context: ZLinkBackendContext;
  readonly channelTransport: ZLinkChannelClientTransport;
  readonly routeTransport: ZLinkRouteClientTransport & ZLinkSpotRoutedTransport;
  readonly spotPublisherTransport: ZLinkSpotPublisherClientTransport;
  readonly meshRouters: MeshRouterResolver;
  readonly providerResolver?: ZLinkProviderResolver;
  readonly dispatchErrors: ZLinkDispatchErrorReporter;
  readonly runtimeEventPublisher: ZLinkRuntimeEventPublisher;
  readonly entryActorRuntime: ZLinkEntryActorRuntime;
  readonly actorTransferRuntime: ZLinkSpotActorTransferRuntime;
  readonly boundSessionRelay: ZLinkBoundSessionRelay;
  readonly actorHandoff: ZLinkActorHandoffCoordinator;
  readonly detachedTaskRunner: ZLinkDetachedTaskRunner;
  readonly metrics: ZLinkRuntimeMetrics;
}

export class ZLinkSpotNodeRuntimeOptionsFactory {
  constructor(private readonly options: ZLinkSpotNodeRuntimeOptionsFactoryOptions) {}

  create(): ZLinkSpotNodeRuntimeManagerOptions {
    return {
      registration: this.options.registration,
      primaryMeshName: this.options.meshRouters.primaryMeshName(),
      backendAdapterFactory: this.options.backendAdapterFactory,
      context: this.options.context,
      channelClient: new DefaultZLinkChannelClient(this.options.registration, this.options.channelTransport),
      fanoutClient: new DefaultZLinkFanoutClient(this.options.registration, this.options.channelTransport),
      spotPublisherClient: new DefaultZLinkSpotPublisherClient(
        this.options.registration,
        this.options.spotPublisherTransport
      ),
      routedTransport: this.options.routeTransport,
      spotRouterChannelIdForMesh: this.options.meshRouters.spotRouterChannelIdByMesh(),
      providerResolver: this.options.providerResolver,
      dispatchErrors: this.options.dispatchErrors,
      runtimeEventPublisher: this.options.runtimeEventPublisher,
      metrics: this.options.metrics,
      detachedTaskRunner: this.options.detachedTaskRunner,
      messageSerializers: this.options.registration.messageSerializers,
      entryActorRuntime: this.options.entryActorRuntime,
      actorTransferRuntime: this.options.actorTransferRuntime,
      boundSessionRuntime: this.options.boundSessionRelay.boundSessions,
      actorHandoffRuntime: this.options.actorHandoff
    };
  }

}
