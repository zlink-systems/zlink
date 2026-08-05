import type { ZLinkActor, ZLinkActorFactory } from '../Actors';
import type {
  ZLinkEntrySpot,
  ZLinkInstanceSpot,
  ZLinkSpot
} from '../Spots';
import type { ZLinkSession, ZLinkSessionFactory, ZLinkStreamCompressionCodec } from '../Streams';
import type { ZLinkEndpointConnections } from './Connections';
import type { ZLinkCodecRegistryBuilder } from '../Codecs';
import type { ZLinkDispatchOptionsBuilder } from '../Dispatch';
import type {
  ZLinkLocationOptions,
  ZLinkLocationStore,
  ZLinkRelocationStore
} from '../Locations';
import type { RoutingId, Type } from '../Common';
import type { ZLinkWorkerOptions } from './RegistrationTypes';
import type { ZLinkSpotPublisherConfig } from './Configs';
import type { ZLinkInboundDispatchOptions } from './InboundDispatch';
import type {
  ZLinkActorFactoryBuilder,
  ZLinkInstanceSpotFactoryBuilder,
  ZLinkUserSpotFactoryBuilder
} from './ObjectRoles';
import type {
  ZLinkRequestHandler,
  ZLinkRouteRequestHandler,
  ZLinkRouteSendHandler,
  ZLinkSendHandler
} from '../Handlers';

export interface ZLinkFrameworkOptions {
  codecs(): ZLinkCodecRegistryBuilder;
  configureNetwork(): ZLinkNetworkOptions;
  /**
   * Configures the bounded worker-thread pool used by `runCpuWorker(...)`.
   * I/O workers do not consume these threads.
   */
  configureWorker(options: ZLinkWorkerOptions): this;
  configureInboundDispatch(): ZLinkInboundDispatchOptions;
  configureDispatch(): ZLinkDispatchOptionsBuilder;
  addLocationStore(store: ZLinkLocationStore): this;
  addRelocationStore(store: ZLinkRelocationStore): this;
  setApplicationVersion(version: bigint): this;
  setMaintenanceWave(waveId: string): this;
  setActorTransferTimeout(timeoutMs: number): this;
  /** Sets how long Message Follow relays traffic from the previous owner. */
  setMessageFollowDuration(timeoutMs: number): this;
  configureLocations(): ZLinkLocationOptions;
  configureStreamCompression(): ZLinkStreamCompressionBuilder;
  addRouteMesh(meshName: string): ZLinkMeshNodeBuilder;
  addClientServerChannel(name: string): ZLinkClientServerChannelRoleBuilder;
  addFanoutChannel(name: string): ZLinkFanoutChannelBuilder;
  addStreamNode(name: string): ZLinkStreamNodeBuilder;
}

export interface ZLinkClientServerChannelRoleBuilder {
  client(): ZLinkClientServerChannelClientBuilder;
  server(): ZLinkClientServerChannelServerBuilder;
}

export interface ZLinkClientServerChannelClientBuilder {
  connect(endpoint: string): this;
}

export interface ZLinkClientServerChannelServerBuilder {
  listen(port?: number): this;
  setBindHost(bindHost: string): this;
  setAdvertiseHost(advertiseHost: string): this;
  setWeight(weight: number): this;
  addHandlerGroup(groupName: string): this;
  addSendHandler<TMessage>(handlerType: Type<ZLinkSendHandler<TMessage>>): this;
  addRequestHandler<TRequest, TReply>(handlerType: Type<ZLinkRequestHandler<TRequest, TReply>>): this;
}

export interface ZLinkMeshPeerConnection {
  readonly endpoint: string;
  readonly expectedRoutingId?: RoutingId;
}

export interface ZLinkMeshPeerConnections {
  connect(endpoint: string): void;
  connect(expectedRoutingId: RoutingId, endpoint: string): void;
  disconnect(endpoint: string): void;
  listConnections(): readonly ZLinkMeshPeerConnection[];
}

export interface ZLinkMeshChannelBuilder {
  client(): ZLinkMeshChannelClientBuilder;
  server(): ZLinkMeshChannelServerBuilder;
}

export interface ZLinkMeshChannelClientBuilder {}

export interface ZLinkMeshChannelServerBuilder {
  setWeight(weight: number): this;
  addHandlerGroup(groupName: string): this;
  addSendHandler<TMessage>(handlerType: Type<ZLinkSendHandler<TMessage>>): this;
  addRequestHandler<TRequest, TReply>(handlerType: Type<ZLinkRequestHandler<TRequest, TReply>>): this;
}

export interface ZLinkMeshNodeSocketConfig {
  maxMessageSize: number;
  sendHighWaterMark: number;
  receiveHighWaterMark: number;
  mailboxMessageBudget: number;
  mailboxByteBudget: number;
  receiveTimeoutMs?: number;
  sendTimeoutMs?: number;
}

export interface ZLinkStreamSocketConfig {
  maxMessageSize: number;
}

export interface ZLinkMeshNodeBuilder {
  channel(channelName: string): ZLinkMeshChannelBuilder;
  listen(endpoint: string): this;
  listen(port?: number): this;
  setBindHost(bindHost: string): this;
  setAdvertiseHost(advertiseHost: string): this;
  routingId(routingId: RoutingId): this;
  setRoutingIdPrefix(prefix: string): this;
  setPlacementWeight(weight: number): this;
  setActorLimit(limit: number): this;
  setSpotLimit(limit: number): this;
  setActivationConcurrency(limit: number): this;
  setInstanceSpotIdleTimeout(timeoutMs: number): this;
  configureRouterSocket(): ZLinkMeshNodeSocketConfig;
  configureSpotPublisher(): ZLinkSpotPublisherConfig;
  peerConnections(): ZLinkMeshPeerConnections;
  setDefaultRequestTimeout(timeoutMs: number): this;
  objects(): ZLinkMeshObjectRoleBuilder;
  addRouteSendHandler<TMessage>(handlerType: Type<ZLinkRouteSendHandler<TMessage>>): this;
  addRouteRequestHandler<TRequest, TReply>(handlerType: Type<ZLinkRouteRequestHandler<TRequest, TReply>>): this;
}

export interface ZLinkNetworkOptions {
  bindHost: string;
  advertiseHost?: string;
}

export interface ZLinkMeshObjectRoleBuilder {
  client(): ZLinkMeshObjectClientBuilder;
  server(): ZLinkMeshObjectServerBuilder;
}

export interface ZLinkMeshObjectClientBuilder {}

export interface ZLinkMeshObjectServerBuilder {
  addEntrySpot<TEntrySpot extends ZLinkEntrySpot>(entrySpotType: Type<TEntrySpot>): this;
  addSpotFactory<TSpot extends ZLinkSpot>(
    spotType: string,
    implementation: Type<TSpot>,
    configure: (builder: ZLinkUserSpotFactoryBuilder<TSpot>) => void
  ): this;
  addInstanceSpotFactory<TSpot extends ZLinkInstanceSpot>(
    instanceSpotType: string,
    implementation: Type<TSpot>,
    configure: (builder: ZLinkInstanceSpotFactoryBuilder<TSpot>) => void
  ): this;
  addActorFactory<TActor extends ZLinkActor>(
    actorType: string,
    implementation: Type<ZLinkActorFactory<TActor>>,
    configure: (builder: ZLinkActorFactoryBuilder<TActor>) => void
  ): this;
}

export interface ZLinkMetadataPolicyBuilder {
  allowSessionToActor(key: string): this;
  allowActorToSession(key: string): this;
}

export interface ZLinkStreamCompressionBuilder {
  useDefault(): this;
  useLz4(): this;
  use(codec: ZLinkStreamCompressionCodec): this;
  disable(): this;
}

export interface ZLinkFanoutChannelBuilder {
  enablePublisher(endpointOrPort?: string | number): this;
  setBindHost(bindHost: string): this;
  setAdvertiseHost(advertiseHost: string): this;
  routingId(routingId: string): this;
  setRoutingIdPrefix(prefix: string): this;
  enableSubscriber(): this;
  connect(endpoint: string): this;
  subscriberConnections(): ZLinkEndpointConnections;
}

export interface ZLinkStreamNodeBuilder {
  bind(endpointOrPort?: string | number): this;
  setBindHost(bindHost: string): this;
  setAdvertiseHost(advertiseHost: string): this;
  configureSocket(): ZLinkStreamSocketConfig;
  enableActorDispatch(): this;
  setTlsServer(certificatePath: string, keyPath: string, requireClientCertificate?: boolean): this;
  registerSession<TSession extends ZLinkSession>(sessionType: Type<TSession> | Type<ZLinkSessionFactory<TSession>>): this;
}
