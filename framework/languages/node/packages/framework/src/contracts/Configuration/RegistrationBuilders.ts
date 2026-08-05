import type {
  RoutingId,
  Type,
  ZLinkActor,
  ZLinkActorFactory,
  ZLinkEntrySpot,
  ZLinkFanoutChannelBuilder,
  ZLinkFrameworkOptions,
  ZLinkMeshChannelBuilder,
  ZLinkMeshChannelClientBuilder,
  ZLinkMeshChannelServerBuilder,
  ZLinkMeshNodeBuilder,
  ZLinkMeshObjectClientBuilder,
  ZLinkMeshObjectRoleBuilder,
  ZLinkMeshObjectServerBuilder,
  ZLinkMeshNodeSocketConfig,
  ZLinkNetworkOptions,
  ZLinkMeshPeerConnection,
  ZLinkMeshPeerConnections,
  ZLinkLocationOptionValues,
  ZLinkRuntimeErrorSink,
  ZLinkSpotPublisherConfig,
  ZLinkHandlerFilter,
  ZLinkInstanceSpot,
  ZLinkSpot,
  ZLinkClientServerChannelClientBuilder,
  ZLinkClientServerChannelRoleBuilder,
  ZLinkClientServerChannelServerBuilder,
  ZLinkStreamCompressionBuilder,
  ZLinkStreamCompressionCodec,
  ZLinkStreamSocketConfig,
  ZLinkStreamNodeBuilder,
  ZLinkSession,
  ZLinkSessionFactory
} from '../../contracts';
import type {
  ZLinkActorFactoryBuilder,
  ZLinkActorRelocationAdapter,
  ZLinkInstanceSpotFactoryBuilder,
  ZLinkSpotRelocationAdapter,
  ZLinkUserSpotFactoryBuilder
} from './ObjectRoles';
import {
  ZLinkSpotRelocationReadinessMode,
  ZLinkUserSpotExecutionMode
} from './ObjectRoles';
import type { ZLinkSpotNodeBuilder } from '../Spots/Builders';
import { readZLinkDecoratorMetadata } from '../Handlers/Attributes';
import type { ZLinkCodecRegistryBuilder } from '../Codecs';
import type {
  ZLinkDispatchOptions,
  ZLinkDispatchOptionsBuilder,
  ZLinkMessageFlowObserver
} from '../Dispatch';
import { ZLinkMessageFlowLogMode, ZLinkUnhandledDispatchAction } from '../Dispatch';
import {
  setDispatchObserverType,
  setRuntimeErrorSinkType
} from './DispatchObserverRegistration';
import { endpointConnections } from './RuntimeEndpointConnections';
import type { ZLinkEndpointConnections } from './Connections';
import type {
  ZLinkLocationOptions,
  ZLinkLocationStore,
  ZLinkRelocationStore
} from '../Locations';
import { ZLinkConfigurationException } from './ConfigurationException';
import {
  RegistrationCodecRegistryBuilder,
  type MutableCodecRegistryOptions
} from './RegistrationCodecRegistry';
import {
  isStreamCompressionCodec,
  normalizeOptionalPositiveInteger,
  typeMapToRecord
} from './RegistrationNormalizers';
import { DEFAULT_STREAM_NODE_MAX_MESSAGE_SIZE } from './InternalDefaults';
import type {
  ZLinkChannelPublishHandlerRegistration,
  ZLinkActorFactoryConfiguration,
  ZLinkFrameworkRegistrationOptions,
  ZLinkInstanceSpotFactoryConfiguration,
  ZLinkRelocationConfiguration,
  ZLinkSpotRouterPeerConnectionOptions,
  ZLinkStreamTlsServerOptions,
  ZLinkUserSpotFactoryConfiguration,
  ZLinkWorkerOptions
} from './RegistrationTypes';
import {
  ZLinkApplicationHwmProfile,
  type ZLinkInboundDispatchOptions,
  type ZLinkInboundDispatchOptionValues
} from './InboundDispatch';
import {
  validateRoutingIdPrefix,
  registerActorFactory,
  registerEntrySpot,
  registerSpotFactory,
  validateActorTransferTimeout,
  validateMessageFollowDuration
} from './RegistrationBuilderPolicy';

export function createFrameworkOptions(
  configure: (options: ZLinkFrameworkOptions) => void
): ZLinkFrameworkRegistrationOptions {
  const builder = new ZLinkFrameworkOptionsBuilder();
  configure(builder);
  return builder.build();
}

class ZLinkFrameworkOptionsBuilder implements ZLinkFrameworkOptions {
  private readonly spotMeshes = new Set<string>();
  private readonly options: MutableFrameworkRegistrationOptions = {
    network: { bindHost: '127.0.0.1' },
    channels: {},
    streamNodes: {},
    spotNodes: {},
    spotFactories: []
  };

  codecs(): ZLinkCodecRegistryBuilder {
    this.options.codecs ??= { serializers: [], streamCodecs: [] };
    return new RegistrationCodecRegistryBuilder(this.options.codecs);
  }

  configureNetwork(): ZLinkNetworkOptions {
    return this.options.network;
  }

  configureWorker(options: ZLinkWorkerOptions): this {
    this.options.worker = { ...this.options.worker, ...options };
    return this;
  }

  configureInboundDispatch(): ZLinkInboundDispatchOptions {
    this.options.inboundDispatch ??= {
      applicationHwmProfile: ZLinkApplicationHwmProfile.Balanced
    };
    return new DefaultInboundDispatchOptionsBuilder(
      this.options.inboundDispatch
    );
  }

  configureDispatch(): ZLinkDispatchOptionsBuilder {
    this.options.dispatch ??= defaultDispatchOptions();
    return new DefaultDispatchOptionsBuilder(this.options.dispatch);
  }

  addLocationStore(store: ZLinkLocationStore): this {
    this.options.locations ??= { options: {} };
    this.options.locations.storeInstance = store;
    return this;
  }

  addRelocationStore(store: ZLinkRelocationStore): this {
    this.options.locations ??= { options: {} };
    this.options.locations.relocationStoreInstance = store;
    return this;
  }

  setApplicationVersion(version: bigint): this {
    this.options.applicationVersion = version;
    return this;
  }

  setMaintenanceWave(waveId: string): this {
    this.options.maintenanceWave = waveId;
    return this;
  }

  setActorTransferTimeout(timeoutMs: number): this {
    this.options.actorTransferTimeoutMs = validateActorTransferTimeout(timeoutMs);
    return this;
  }

  setMessageFollowDuration(timeoutMs: number): this {
    this.options.messageFollowDurationMs = validateMessageFollowDuration(timeoutMs);
    return this;
  }

  configureLocations(): ZLinkLocationOptions {
    this.options.locations ??= { options: {} };
    this.options.locations.options ??= {};
    return new DefaultLocationOptionsBuilder(this.options.locations.options);
  }

  configureStreamCompression(): ZLinkStreamCompressionBuilder {
    this.options.streamCompression ??= {};
    return new DefaultStreamCompressionBuilder(this.options.streamCompression);
  }

  addRouteMesh(meshName: string): ZLinkMeshNodeBuilder {
    requireRegistrationName(meshName, 'RouteMesh');
    if (this.spotMeshes.has(meshName)) {
      throw new ZLinkConfigurationException(`Duplicate RouteMesh '${meshName}'.`);
    }
    this.spotMeshes.add(meshName);
    const node = this.spotNodeOptions(meshName);
    node.router ??= { port: 0 };
    return new DefaultMeshNodeBuilder(
      meshName,
      node
    );
  }

  addFanoutChannel(name: string): ZLinkFanoutChannelBuilder {
    return new DefaultFanoutChannelBuilder(name, this.channel(name));
  }

  addClientServerChannel(name: string): ZLinkClientServerChannelRoleBuilder {
    return new DefaultClientServerChannelRoleBuilder(name, this.clientServerChannel(name));
  }

  addStreamNode(name: string): ZLinkStreamNodeBuilder {
    if (name.trim().length === 0 || name.trim() !== name) {
      throw new ZLinkConfigurationException('STREAM node name must not be empty or padded.');
    }
    if (Object.prototype.hasOwnProperty.call(this.options.streamNodes, name)) {
      throw new ZLinkConfigurationException(`Duplicate STREAM node '${name}'.`);
    }
    const streamNode: MutableStreamNodeOptions = {
      maxMessageSize: DEFAULT_STREAM_NODE_MAX_MESSAGE_SIZE
    };
    this.options.streamNodes[name] = streamNode;
    return new DefaultStreamNodeBuilder(streamNode);
  }

  build(): ZLinkFrameworkRegistrationOptions {
    return {
      network: { ...this.options.network },
      codecs: this.options.codecs,
      applicationVersion: this.options.applicationVersion,
      maintenanceWave: this.options.maintenanceWave,
      channels: this.options.channels,
      requestTimeoutMs: this.options.requestTimeoutMs,
      streamNodes: this.options.streamNodes,
      streamCompression: this.options.streamCompression,
      spotNodes: this.options.spotNodes,
      spotFactories: this.options.spotFactories,
      actorTransferTimeoutMs: this.options.actorTransferTimeoutMs,
      messageFollowDurationMs: this.options.messageFollowDurationMs,
      dispatch: this.options.dispatch,
      inboundDispatch: this.options.inboundDispatch,
      worker: this.options.worker,
      locations: this.options.locations
    };
  }

  private channel(name: string): MutableChannelOptions {
    if (name.trim().length === 0 || name.trim() !== name) {
      throw new ZLinkConfigurationException('Channel name must not be empty or padded.');
    }
    if (Object.prototype.hasOwnProperty.call(this.options.channels, name)) {
      throw new ZLinkConfigurationException(`Duplicate channel '${name}'.`);
    }
    const channel: MutableChannelOptions = {};
    this.options.channels[name] = channel;
    return channel;
  }

  private clientServerChannel(name: string): MutableChannelOptions {
    if (name.trim().length === 0 || name.trim() !== name) {
      throw new ZLinkConfigurationException('Channel name must not be empty or padded.');
    }
    if (!Object.prototype.hasOwnProperty.call(this.options.channels, name)) {
      const channel: MutableChannelOptions = {};
      this.options.channels[name] = channel;
      return channel;
    }
    const current = this.options.channels[name];
    if (current.publisher !== undefined || current.subscriber !== undefined) {
      throw new ZLinkConfigurationException(`Duplicate channel '${name}'.`);
    }
    return current;
  }

  private spotNodeOptions(name: string): MutableSpotNodeOptions {
    this.options.spotNodes[name] ??= {};
    return this.options.spotNodes[name];
  }
}

export class DefaultInboundDispatchOptionsBuilder
implements ZLinkInboundDispatchOptions {
  constructor(
    private readonly values: Partial<ZLinkInboundDispatchOptionValues>
  ) {}

  applicationHwmBytes(value: bigint | undefined): this {
    this.values.applicationHwmBytes = value;
    return this;
  }

  applicationHwmProfile(value: ZLinkApplicationHwmProfile): this {
    this.values.applicationHwmProfile = value;
    return this;
  }

  processMemoryLimitBytes(value: bigint | undefined): this {
    this.values.processMemoryLimitBytes = value;
    return this;
  }
}

export class DefaultDispatchOptionsBuilder implements ZLinkDispatchOptionsBuilder {
  constructor(private readonly dispatch: ZLinkDispatchOptions) {}

  setMessageFlowObserver(observerType: Type<ZLinkMessageFlowObserver>): this {
    setDispatchObserverType(this.dispatch, observerType);
    return this;
  }

  setRuntimeErrorSink(sinkType: Type<ZLinkRuntimeErrorSink>): this {
    setRuntimeErrorSinkType(this.dispatch, sinkType);
    return this;
  }

  messageFlow(mode: ZLinkMessageFlowLogMode): this {
    this.dispatch.diagnostics.messageFlow = mode;
    return this;
  }

  traceSampleRate(rate: number): this {
    this.dispatch.diagnostics.sampleRate = rate;
    return this;
  }

  includeMessageSizes(include: boolean): this {
    this.dispatch.diagnostics.includeMessageSizes = include;
    return this;
  }

  traceLogFile(path: string): this {
    this.dispatch.diagnostics.logFile = path;
    return this;
  }

  traceLabel(id: string): this {
    this.dispatch.diagnostics.label = id;
    return this;
  }
}

export class DefaultLocationOptionsBuilder implements ZLinkLocationOptions {
  constructor(private readonly options: MutableLocationOptionValues) {}

  ownerLeaseRenewIntervalMs(value: number): this {
    this.options.ownerLeaseRenewIntervalMs = value;
    return this;
  }

  ownerLeaseTtlMs(value: number): this {
    this.options.ownerLeaseTtlMs = value;
    return this;
  }

  pollingIntervalMs(value: number): this {
    this.options.pollingIntervalMs = value;
    return this;
  }

  storeFailureGraceMs(value: number): this {
    this.options.storeFailureGraceMs = value;
    return this;
  }

  ownerLeaseFencingMarginMs(value: number): this {
    this.options.ownerLeaseFencingMarginMs = value;
    return this;
  }

  ownerLeaseRenewTimeoutMs(value: number): this {
    this.options.ownerLeaseRenewTimeoutMs = value;
    return this;
  }

  routeCacheMaxAgeMs(value: number): this {
    this.options.routeCacheMaxAgeMs = value;
    return this;
  }

  messageFollowDurationMs(value: number): this {
    this.options.messageFollowDurationMs = value;
    return this;
  }

  maxActiveOutboundRelocations(value: number): this {
    this.options.maxActiveOutboundRelocations = value;
    return this;
  }

  maxActiveInboundRelocations(value: number): this {
    this.options.maxActiveInboundRelocations = value;
    return this;
  }

  maxConcurrentRelocationCaptures(value: number): this {
    this.options.maxConcurrentRelocationCaptures = value;
    return this;
  }

  maxConcurrentRelocationRestores(value: number): this {
    this.options.maxConcurrentRelocationRestores = value;
    return this;
  }

  maxRelocationPayloadInFlightBytes(value: number): this {
    this.options.maxRelocationPayloadInFlightBytes = value;
    return this;
  }
}

function defaultDispatchOptions(): ZLinkDispatchOptions {
  return {
    unhandled: {
      request: ZLinkUnhandledDispatchAction.ReplyError,
      send: ZLinkUnhandledDispatchAction.LogAndDrop,
      publish: ZLinkUnhandledDispatchAction.LogAndDrop
    },
    diagnostics: {
      messageFlow: ZLinkMessageFlowLogMode.ErrorsOnly,
      sampleRate: 1,
      includeMessageSizes: false
    }
  };
}

class DefaultFanoutChannelBuilder implements ZLinkFanoutChannelBuilder {
  private subscriberMode?: 'automatic' | 'manual';

  constructor(
    private readonly name: string,
    private readonly channel: MutableChannelOptions
  ) {}

  enablePublisher(endpointOrPort: string | number = 0): this {
    this.channel.publisher ??= {};
    if (typeof endpointOrPort === 'string') {
      requireRegistrationName(endpointOrPort, `Fanout channel '${this.name}' publisher endpoint`);
      this.channel.publisher.bind = endpointOrPort;
      this.channel.publisher.port = undefined;
      return this;
    }
    this.channel.publisher.port = requireListenerPort(
      endpointOrPort,
      `Fanout channel '${this.name}' publisher port`
    );
    this.updateBind();
    return this;
  }

  setBindHost(bindHost: string): this {
    requireRegistrationName(bindHost, `Fanout channel '${this.name}' bind host`);
    this.channel.publisher ??= {};
    this.channel.publisher.bindHost = bindHost;
    if (this.channel.publisher.port !== undefined) this.updateBind();
    return this;
  }

  setAdvertiseHost(advertiseHost: string): this {
    requireRegistrationName(advertiseHost, `Fanout channel '${this.name}' advertise host`);
    this.channel.publisher ??= {};
    this.channel.publisher.advertiseHost = advertiseHost;
    return this;
  }

  routingId(routingId: string): this {
    rejectGeneratedRoutingId(this.channel.routingIdPrefix, this.name);
    this.channel.routingId = routingId;
    return this;
  }

  setRoutingIdPrefix(prefix: string): this {
    rejectFixedRoutingId(this.channel.routingId, this.name);
    this.channel.routingIdPrefix = validateRoutingIdPrefix(prefix);
    return this;
  }

  enableSubscriber(endpoint?: string): this {
    this.selectSubscriberMode(endpoint === undefined ? 'automatic' : 'manual');
    this.channel.subscriber ??= { manualConnections: [] };
    if (endpoint !== undefined) {
      this.channel.subscriber.manualConnections ??= [];
      if (!this.channel.subscriber.manualConnections.includes(endpoint)) {
        this.channel.subscriber.manualConnections.push(endpoint);
      }
    }
    return this;
  }

  connect(endpoint: string): this {
    requireRegistrationName(endpoint, `Fanout channel '${this.name}' subscriber endpoint`);
    this.selectSubscriberMode('manual');
    this.channel.subscriber ??= { manualConnections: [] };
    this.channel.subscriber.manualConnections ??= [];
    if (!this.channel.subscriber.manualConnections.includes(endpoint)) {
      this.channel.subscriber.manualConnections.push(endpoint);
    }
    return this;
  }

  subscriberConnections(): ZLinkEndpointConnections {
    this.selectSubscriberMode('manual');
    this.channel.subscriber ??= { manualConnections: [] };
    this.channel.subscriber.manualConnections ??= [];
    return endpointConnections(this.channel.subscriber, this.channel.subscriber.manualConnections);
  }

  private selectSubscriberMode(mode: 'automatic' | 'manual'): void {
    if (this.subscriberMode !== undefined && this.subscriberMode !== mode) {
      throw new ZLinkConfigurationException(
        `Fanout channel '${this.name}' cannot combine automatic and manual subscriber sources.`
      );
    }
    this.subscriberMode = mode;
  }

  private updateBind(): void {
    const publisher = this.channel.publisher;
    if (publisher?.port === undefined) return;
    publisher.bind = tcpEndpoint(publisher.bindHost ?? '127.0.0.1', publisher.port);
  }
}

class DefaultClientServerChannelRoleBuilder implements ZLinkClientServerChannelRoleBuilder {
  constructor(
    private readonly name: string,
    private readonly channel: MutableChannelOptions
  ) {}

  client(): ZLinkClientServerChannelClientBuilder {
    if (this.channel.client !== undefined) {
      throw this.duplicate('Client');
    }
    this.channel.client = { manualConnections: [] };
    return new DefaultClientServerChannelClientBuilder(this.name, this.channel.client);
  }

  server(): ZLinkClientServerChannelServerBuilder {
    if (this.channel.server !== undefined) {
      throw this.duplicate('Server');
    }
    this.channel.server = {};
    return new DefaultClientServerChannelServerBuilder(this.name, this.channel);
  }

  private duplicate(role: 'Client' | 'Server'): ZLinkConfigurationException {
    return new ZLinkConfigurationException(
      `ClientServer channel '${this.name}' ${role} role is already registered.`
    );
  }
}

class DefaultClientServerChannelClientBuilder implements ZLinkClientServerChannelClientBuilder {
  constructor(
    private readonly name: string,
    private readonly client: MutableClientCapabilityOptions
  ) {}

  connect(endpoint: string): this {
    requireRegistrationName(endpoint, `ClientServer channel '${this.name}' endpoint`);
    this.client.manualConnections ??= [];
    if (!this.client.manualConnections.includes(endpoint)) {
      this.client.manualConnections.push(endpoint);
    }
    return this;
  }
}

class DefaultClientServerChannelServerBuilder implements ZLinkClientServerChannelServerBuilder {
  constructor(
    private readonly name: string,
    private readonly channel: MutableChannelOptions
  ) {}

  listen(port = 0): this {
    if (!Number.isInteger(port) || port < 0 || port > 65_535) {
      throw new ZLinkConfigurationException(
        `ClientServer channel '${this.name}' port must be between 0 and 65535.`
      );
    }
    this.server.port = port;
    this.updateBind();
    return this;
  }

  setBindHost(bindHost: string): this {
    requireRegistrationName(bindHost, `ClientServer channel '${this.name}' bind host`);
    this.server.bindHost = bindHost;
    if (this.server.port !== undefined) this.updateBind();
    return this;
  }

  setAdvertiseHost(advertiseHost: string): this {
    requireRegistrationName(advertiseHost, `ClientServer channel '${this.name}' advertise host`);
    this.server.advertiseHost = advertiseHost;
    return this;
  }

  setWeight(weight: number): this {
    this.server.weight = requirePublicWeight(
      weight,
      `ClientServer channel '${this.name}' weight`
    );
    return this;
  }

  addHandlerGroup(groupName: string): this {
    requireRegistrationName(groupName, `ClientServer channel '${this.name}' handler group`);
    this.channel.handlerGroups ??= [];
    if (!this.channel.handlerGroups.includes(groupName)) this.channel.handlerGroups.push(groupName);
    return this;
  }

  addSendHandler<TMessage>(handlerType: Type<import('../Handlers').ZLinkSendHandler<TMessage>>): this {
    this.channel.sendHandlers ??= [];
    this.channel.sendHandlers.push({ packetName: handlerPacketName(handlerType), handlerType });
    return this;
  }

  addRequestHandler<TRequest, TReply>(
    handlerType: Type<import('../Handlers').ZLinkRequestHandler<TRequest, TReply>>
  ): this {
    this.channel.requestHandlers ??= [];
    this.channel.requestHandlers.push({ packetName: handlerPacketName(handlerType), handlerType });
    return this;
  }

  private get server(): MutableServerCapabilityOptions {
    return this.channel.server!;
  }

  private updateBind(): void {
    const host = this.server.bindHost ?? '127.0.0.1';
    const endpointHost = host.includes(':') && !host.startsWith('[') ? `[${host}]` : host;
    this.server.bind = `tcp://${endpointHost}:${this.server.port ?? 0}`;
  }
}

class DefaultStreamNodeBuilder implements ZLinkStreamNodeBuilder {
  constructor(private readonly streamNode: MutableStreamNodeOptions) {}

  bind(endpointOrPort: string | number = 0): this {
    if (typeof endpointOrPort === 'string') {
      requireRegistrationName(endpointOrPort, 'STREAM bind endpoint');
      this.streamNode.bind = endpointOrPort;
      this.streamNode.port = undefined;
      return this;
    }
    this.streamNode.port = requireListenerPort(endpointOrPort, 'STREAM bind port');
    this.updateBind();
    return this;
  }

  setBindHost(bindHost: string): this {
    requireRegistrationName(bindHost, 'STREAM bind host');
    this.streamNode.bindHost = bindHost;
    if (this.streamNode.port !== undefined) this.updateBind();
    return this;
  }

  setAdvertiseHost(advertiseHost: string): this {
    requireRegistrationName(advertiseHost, 'STREAM advertise host');
    this.streamNode.advertiseHost = advertiseHost;
    return this;
  }

  configureSocket(): ZLinkStreamSocketConfig {
    return this.streamNode;
  }

  enableActorDispatch(): this {
    if (this.streamNode.actorDispatchEnabled === true) {
      throw new ZLinkConfigurationException('STREAM node actor dispatch is already enabled.');
    }
    this.streamNode.actorDispatchEnabled = true;
    return this;
  }

  setTlsServer(certificatePath: string, keyPath: string, requireClientCertificate: boolean = false): this {
    this.streamNode.tlsServer = {
      certificatePath,
      keyPath,
      requireClientCertificate
    };
    return this;
  }

  registerSession<TSession extends ZLinkSession>(sessionType: Type<TSession> | Type<ZLinkSessionFactory<TSession>>): this {
    if (this.streamNode.session !== undefined) {
      throw new ZLinkConfigurationException('STREAM node cannot register more than one header stream session.');
    }
    this.streamNode.session = sessionType;
    return this;
  }

  private updateBind(): void {
    if (this.streamNode.port === undefined) return;
    this.streamNode.bind = tcpEndpoint(
      this.streamNode.bindHost ?? '127.0.0.1',
      this.streamNode.port
    );
  }
}

export class DefaultStreamCompressionBuilder implements ZLinkStreamCompressionBuilder {
  constructor(private readonly options: MutableStreamCompressionOptions) {}

  useDefault(): this {
    this.options.disabled = false;
    this.options.codec = undefined;
    return this;
  }

  useLz4(): this {
    return this.useDefault();
  }

  use(codec: ZLinkStreamCompressionCodec): this {
    if (!isStreamCompressionCodec(codec)) {
      throw new ZLinkConfigurationException('STREAM compression codec must provide compress and decompress functions.');
    }
    this.options.disabled = false;
    this.options.codec = codec;
    return this;
  }

  disable(): this {
    this.options.disabled = true;
    this.options.codec = undefined;
    return this;
  }
}

class DefaultSpotNodeBuilder implements ZLinkSpotNodeBuilder {
  constructor(
    private readonly name: string,
    private readonly spotNode: MutableSpotNodeOptions
  ) {}

  routingId(routingId: RoutingId): this {
    rejectGeneratedRoutingId(this.spotNode.routingIdPrefix, this.name);
    this.spotNode.routingId = routingId;
    if (this.spotNode.router !== undefined) {
      this.spotNode.router.routingId = routingId;
    }
    if (this.spotNode.pubSub !== undefined) {
      this.spotNode.pubSub.routingId = routingId;
    }
    return this;
  }

  setRoutingIdPrefix(prefix: string): this {
    rejectFixedRoutingId(this.spotNode.routingId, this.name);
    this.spotNode.routingIdPrefix = validateRoutingIdPrefix(prefix);
    return this;
  }

  enableRouter(endpoint: string, routingId?: RoutingId, connect?: string | readonly string[]): this {
    this.spotNode.router = {
      ...(this.spotNode.router ?? {}),
      bind: endpoint,
      routingId: routingId ?? this.spotNode.routingId,
      manualConnections: connect === undefined ? this.spotNode.router?.manualConnections : endpointList(connect)
    };
    return this;
  }

  connectRouter(endpoint: string): this;
  connectRouter(peerRid: RoutingId, endpoint: string): this;
  connectRouter(peerRidOrEndpoint: RoutingId | string, endpoint?: string): this {
    this.spotNode.router ??= { manualConnections: [] };
    if (endpoint === undefined) {
      this.spotNode.router.manualConnections ??= [];
      this.spotNode.router.manualConnections.push(peerRidOrEndpoint as string);
      return this;
    }
    this.spotNode.router.manualPeerConnections ??= [];
    this.spotNode.router.manualPeerConnections.push({
      peerRid: peerRidOrEndpoint,
      endpoint
    });
    return this;
  }

  enablePubSub(endpoint: string, routingId?: RoutingId, connect?: string | readonly string[]): this {
    this.spotNode.pubSub = {
      ...(this.spotNode.pubSub ?? {}),
      bind: endpoint,
      routingId: routingId ?? this.spotNode.routingId,
      manualConnections: connect === undefined ? this.spotNode.pubSub?.manualConnections : endpointList(connect)
    };
    return this;
  }

  connectPeerPub(endpoint: string): this {
    this.spotNode.pubSub ??= { manualConnections: [] };
    this.spotNode.pubSub.manualConnections ??= [];
    this.spotNode.pubSub.manualConnections.push(endpoint);
    return this;
  }

}

class DefaultMeshNodeBuilder implements ZLinkMeshNodeBuilder {
  private readonly spot: DefaultSpotNodeBuilder;

  constructor(
    private readonly name: string,
    private readonly node: MutableSpotNodeOptions
  ) {
    this.spot = new DefaultSpotNodeBuilder(name, node);
  }

  channel(channelName: string): ZLinkMeshChannelBuilder {
    requireRegistrationName(channelName, 'Mesh channel');
    this.node.meshChannels ??= {};
    if (Object.prototype.hasOwnProperty.call(this.node.meshChannels, channelName)) {
      throw new ZLinkConfigurationException(
        `Duplicate channel '${channelName}' in RouteMesh '${this.name}'.`
      );
    }
    const channel: MutableMeshChannelOptions = {};
    this.node.meshChannels[channelName] = channel;
    return new DefaultMeshChannelBuilder(channel);
  }

  listen(endpointOrPort: string | number = 0): this {
    this.node.router ??= {};
    if (typeof endpointOrPort === 'string') {
      requireRegistrationName(endpointOrPort, 'RouteMesh listen endpoint');
      this.node.router.bind = endpointOrPort;
      this.node.router.port = undefined;
      return this;
    }
    if (!Number.isInteger(endpointOrPort) || endpointOrPort < 0 || endpointOrPort > 65_535) {
      throw new ZLinkConfigurationException(
        `RouteMesh '${this.name}' port must be between 0 and 65535.`
      );
    }
    this.node.router.port = endpointOrPort;
    this.updateBind();
    return this;
  }

  setBindHost(bindHost: string): this {
    requireRegistrationName(bindHost, `RouteMesh '${this.name}' bind host`);
    this.node.router ??= {};
    this.node.router.bindHost = bindHost;
    if (this.node.router.port !== undefined) this.updateBind();
    return this;
  }

  setAdvertiseHost(advertiseHost: string): this {
    requireRegistrationName(advertiseHost, `RouteMesh '${this.name}' advertise host`);
    this.node.router ??= {};
    this.node.router.advertiseHost = advertiseHost;
    return this;
  }

  routingId(routingId: RoutingId): this {
    this.spot.routingId(routingId);
    return this;
  }

  setRoutingIdPrefix(prefix: string): this {
    this.spot.setRoutingIdPrefix(prefix);
    return this;
  }

  setPlacementWeight(weight: number): this {
    this.node.placementWeight = requirePublicWeight(weight, 'Placement weight');
    return this;
  }

  setActorLimit(limit: number): this {
    this.node.actorLimit = requirePositiveCapacity(limit, 'Actor limit');
    return this;
  }

  setSpotLimit(limit: number): this {
    this.node.spotLimit = requirePositiveCapacity(limit, 'Spot limit');
    return this;
  }

  setActivationConcurrency(limit: number): this {
    this.node.activationConcurrencyLimit = requirePositiveCapacity(
      limit,
      'Activation concurrency limit'
    );
    return this;
  }

  setInstanceSpotIdleTimeout(timeoutMs: number): this {
    this.node.instanceSpotIdleTimeoutMs = requireNonNegativeSafeInteger(
      timeoutMs,
      'Instance Spot idle timeout'
    );
    return this;
  }

  configureRouterSocket(): ZLinkMeshNodeSocketConfig {
    this.node.router ??= {};
    return this.node.router as ZLinkMeshNodeSocketConfig;
  }

  configureSpotPublisher(): ZLinkSpotPublisherConfig {
    this.node.publisherConfig ??= {};
    return this.node.publisherConfig as ZLinkSpotPublisherConfig;
  }

  peerConnections(): ZLinkMeshPeerConnections {
    this.node.router ??= {};
    return new DefaultMeshPeerConnections(this.node.router);
  }

  setDefaultRequestTimeout(timeoutMs: number): this {
    this.node.requestTimeoutMs = normalizeOptionalPositiveInteger(
      timeoutMs,
      `RouteMesh '${this.name}' default request timeout`
    );
    return this;
  }

  objects(): ZLinkMeshObjectRoleBuilder {
    return new DefaultMeshObjectRoleBuilder(this.name, this.node);
  }

  addRouteSendHandler<TMessage>(handlerType: Type<import('../Handlers').ZLinkRouteSendHandler<TMessage>>): this {
    this.node.routeSendHandlers ??= [];
    this.node.routeSendHandlers.push({ packetName: handlerPacketName(handlerType), handlerType });
    return this;
  }

  addRouteRequestHandler<TRequest, TReply>(
    handlerType: Type<import('../Handlers').ZLinkRouteRequestHandler<TRequest, TReply>>
  ): this {
    this.node.routeRequestHandlers ??= [];
    this.node.routeRequestHandlers.push({ packetName: handlerPacketName(handlerType), handlerType });
    return this;
  }

  private updateBind(): void {
    const router = this.node.router;
    if (router?.port === undefined) return;
    const bindHost = router.bindHost ?? '127.0.0.1';
    const host = bindHost.includes(':') && !bindHost.startsWith('[')
      ? `[${bindHost}]`
      : bindHost;
    router.bind = `tcp://${host}:${router.port}`;
  }
}

class DefaultMeshObjectRoleBuilder implements ZLinkMeshObjectRoleBuilder {
  constructor(
    private readonly meshName: string,
    private readonly node: MutableSpotNodeOptions
  ) {}

  client(): ZLinkMeshObjectClientBuilder {
    this.node.objectRole = 'client';
    return {};
  }

  server(): ZLinkMeshObjectServerBuilder {
    this.node.objectRole = 'server';
    return new DefaultMeshObjectServerBuilder(this.meshName, this.node);
  }
}

abstract class DefaultFactoryBuilder<TInstance> {
  private relocation: ZLinkRelocationConfiguration<TInstance> | undefined;
  private sealed = false;

  protected disable(): void {
    this.select({ kind: 'disabled' });
  }

  protected recreate(): void {
    this.select({ kind: 'recreate' });
  }

  protected preserve(adapterType: Type): void {
    if (typeof adapterType !== 'function') {
      throw new ZLinkConfigurationException(
        'PreserveStateWith relocation requires an adapter type.'
      );
    }
    this.select({ kind: 'snapshot', adapterType });
  }

  protected relocationConfiguration(): ZLinkRelocationConfiguration<TInstance> {
    if (this.relocation === undefined) {
      throw new ZLinkConfigurationException(
        'Factory configure callback must select exactly one relocation policy.'
      );
    }
    return this.relocation;
  }

  seal(): void {
    this.sealed = true;
  }

  protected assertMutable(): void {
    if (this.sealed) {
      throw new ZLinkConfigurationException(
        'Factory builder cannot be changed after the configure callback returns.'
      );
    }
  }

  private select(relocation: ZLinkRelocationConfiguration<TInstance>): void {
    this.assertMutable();
    if (this.relocation !== undefined) {
      throw new ZLinkConfigurationException(
        'Factory configure callback must select exactly one relocation policy.'
      );
    }
    this.relocation = relocation;
  }
}

class DefaultActorFactoryBuilder<TActor extends ZLinkActor>
  extends DefaultFactoryBuilder<TActor>
  implements ZLinkActorFactoryBuilder<TActor> {
  disableRelocation(): void {
    this.disable();
  }

  recreateOnRelocation(): void {
    this.recreate();
  }

  preserveStateWith(
    adapterType: Type<ZLinkActorRelocationAdapter<TActor>>
  ): void {
    this.preserve(adapterType);
  }

  build(): {
    readonly options: ZLinkActorFactoryConfiguration;
    readonly relocation: ZLinkRelocationConfiguration<TActor>;
  } {
    return {
      options: {},
      relocation: this.relocationConfiguration()
    };
  }
}

class DefaultUserSpotFactoryBuilder<TSpot extends ZLinkSpot>
  extends DefaultFactoryBuilder<TSpot>
  implements ZLinkUserSpotFactoryBuilder<TSpot> {
  private stableTypeLimitValue: number | undefined;
  private executionModeValue = ZLinkUserSpotExecutionMode.SpotWide;
  private relocationReadinessValue = ZLinkSpotRelocationReadinessMode.AnyTurnBoundary;

  stableTypeLimit(limit: number): this {
    this.assertMutable();
    validateStableTypeLimit(limit);
    this.stableTypeLimitValue = limit;
    return this;
  }

  executionMode(mode: ZLinkUserSpotExecutionMode): this {
    this.assertMutable();
    this.executionModeValue = mode;
    return this;
  }

  relocationReadiness(mode: ZLinkSpotRelocationReadinessMode): this {
    this.assertMutable();
    this.relocationReadinessValue = mode;
    return this;
  }

  disableRelocation(): void {
    this.disable();
  }

  recreateOnRelocation(): void {
    this.recreate();
  }

  preserveStateWith(
    adapterType: Type<ZLinkSpotRelocationAdapter<TSpot>>
  ): void {
    this.preserve(adapterType);
  }

  build(): {
    readonly options: ZLinkUserSpotFactoryConfiguration;
    readonly relocation: ZLinkRelocationConfiguration<TSpot>;
  } {
    const relocation = this.relocationConfiguration();
    const options = {
      stableTypeLimit: this.stableTypeLimitValue,
      executionMode: this.executionModeValue,
      relocationReadiness: this.relocationReadinessValue
    };
    validateUserSpotFactoryOptions(options);
    if (
      options.executionMode === ZLinkUserSpotExecutionMode.PerActor
      && relocation.kind !== 'recreate'
    ) {
      throw new ZLinkConfigurationException(
        'PerActor User Spots require RecreateOnRelocation.'
      );
    }
    return { options, relocation };
  }
}

class DefaultInstanceSpotFactoryBuilder<TSpot extends ZLinkInstanceSpot>
  extends DefaultFactoryBuilder<TSpot>
  implements ZLinkInstanceSpotFactoryBuilder<TSpot> {
  private stableTypeLimitValue: number | undefined;

  stableTypeLimit(limit: number): this {
    this.assertMutable();
    validateStableTypeLimit(limit);
    this.stableTypeLimitValue = limit;
    return this;
  }

  disableRelocation(): void {
    this.disable();
  }

  recreateOnRelocation(): void {
    this.recreate();
  }

  preserveStateWith(
    adapterType: Type<ZLinkSpotRelocationAdapter<TSpot>>
  ): void {
    this.preserve(adapterType);
  }

  build(): {
    readonly options: ZLinkInstanceSpotFactoryConfiguration;
    readonly relocation: ZLinkRelocationConfiguration<TSpot>;
  } {
    return {
      options: { stableTypeLimit: this.stableTypeLimitValue },
      relocation: this.relocationConfiguration()
    };
  }
}

class DefaultMeshObjectServerBuilder implements ZLinkMeshObjectServerBuilder {
  constructor(
    private readonly meshName: string,
    private readonly node: MutableSpotNodeOptions
  ) {}

  addEntrySpot<TEntrySpot extends ZLinkEntrySpot>(entrySpotType: Type<TEntrySpot>): this {
    registerEntrySpot(this.node, entrySpotType);
    return this;
  }

  addSpotFactory<TSpot extends ZLinkSpot>(
    spotType: string,
    implementation: Type<TSpot>,
    configure: (builder: ZLinkUserSpotFactoryBuilder<TSpot>) => void
  ): this {
    const stableType = requireStableObjectType(spotType, 'User Spot type');
    requireFactoryConfigure(configure);
    const factory = new DefaultUserSpotFactoryBuilder<TSpot>();
    let built: ReturnType<typeof factory.build>;
    try {
      configure(factory);
      built = factory.build();
    } finally {
      factory.seal();
    }
    const { options, relocation } = built;
    validateUserSpotFactoryOptions(options);
    this.node.spotFactoryRegistrations ??= {};
    rejectDuplicateObjectType(this.node.spotFactoryRegistrations, stableType, this.meshName);
    this.node.spotFactoryRegistrations[stableType] = {
      implementation,
      options: {
        ...options,
        executionMode: options.executionMode
      },
      relocation
    };
    registerSpotFactory(this.node, implementation);
    return this;
  }

  addInstanceSpotFactory<TSpot extends ZLinkInstanceSpot>(
    instanceSpotType: string,
    implementation: Type<TSpot>,
    configure: (builder: ZLinkInstanceSpotFactoryBuilder<TSpot>) => void
  ): this {
    const stableType = requireStableObjectType(instanceSpotType, 'Instance Spot type');
    requireFactoryConfigure(configure);
    const factory = new DefaultInstanceSpotFactoryBuilder<TSpot>();
    let built: ReturnType<typeof factory.build>;
    try {
      configure(factory);
      built = factory.build();
    } finally {
      factory.seal();
    }
    const { options, relocation } = built;
    validateStableTypeLimit(options.stableTypeLimit);
    this.node.instanceSpotFactories ??= {};
    if (Object.hasOwn(this.node.instanceSpotFactories, stableType)) {
      throw new ZLinkConfigurationException(
        `Duplicate Instance Spot factory '${stableType}' on RouteMesh '${this.meshName}'.`
      );
    }
    this.node.instanceSpotFactories[stableType] = implementation;
    this.node.instanceSpotFactoryRegistrations ??= {};
    this.node.instanceSpotFactoryRegistrations[stableType] = {
      implementation,
      options,
      relocation
    };
    return this;
  }

  addActorFactory<TActor extends ZLinkActor>(
    actorType: string,
    implementation: Type<ZLinkActorFactory<TActor>>,
    configure: (builder: ZLinkActorFactoryBuilder<TActor>) => void
  ): this {
    const stableType = requireStableObjectType(actorType, 'Actor type');
    requireFactoryConfigure(configure);
    const factory = new DefaultActorFactoryBuilder<TActor>();
    let built: ReturnType<typeof factory.build>;
    try {
      configure(factory);
      built = factory.build();
    } finally {
      factory.seal();
    }
    const { options, relocation } = built;
    this.node.actorFactories = typeMapToRecord(this.node.actorFactories);
    registerActorFactory(this.node, stableType, implementation);
    this.node.actorFactoryRegistrations ??= {};
    this.node.actorFactoryRegistrations[stableType] = {
      implementation,
      options,
      relocation
    };
    return this;
  }
}

class DefaultMeshChannelBuilder implements ZLinkMeshChannelBuilder {
  constructor(private readonly channel: MutableMeshChannelOptions) {}

  client(): ZLinkMeshChannelClientBuilder {
    if (this.channel.client === true || this.channel.server === true) {
      throw new ZLinkConfigurationException(
        'RouteMesh channel must register exactly one role; client() and server() cannot both be used or called twice.'
      );
    }
    this.channel.client = true;
    return new DefaultMeshChannelClientBuilder();
  }

  server(): ZLinkMeshChannelServerBuilder {
    if (this.channel.client === true || this.channel.server === true) {
      throw new ZLinkConfigurationException(
        'RouteMesh channel must register exactly one role; client() and server() cannot both be used or called twice.'
      );
    }
    this.channel.server = true;
    return new DefaultMeshChannelServerBuilder(this.channel);
  }
}

class DefaultMeshChannelClientBuilder implements ZLinkMeshChannelClientBuilder {}

class DefaultMeshChannelServerBuilder implements ZLinkMeshChannelServerBuilder {
  constructor(private readonly channel: MutableMeshChannelOptions) {}

  setWeight(weight: number): this {
    this.channel.weight = requirePublicWeight(weight, 'Mesh channel weight');
    return this;
  }

  addHandlerGroup(groupName: string): this {
    requireRegistrationName(groupName, 'Mesh channel handler group');
    this.channel.handlerGroups ??= [];
    if (!this.channel.handlerGroups.includes(groupName)) this.channel.handlerGroups.push(groupName);
    return this;
  }

  addSendHandler<TMessage>(handlerType: Type<import('../Handlers').ZLinkSendHandler<TMessage>>): this {
    this.channel.sendHandlers ??= [];
    this.channel.sendHandlers.push({ packetName: handlerPacketName(handlerType), handlerType });
    return this;
  }

  addRequestHandler<TRequest, TReply>(
    handlerType: Type<import('../Handlers').ZLinkRequestHandler<TRequest, TReply>>
  ): this {
    this.channel.requestHandlers ??= [];
    this.channel.requestHandlers.push({ packetName: handlerPacketName(handlerType), handlerType });
    return this;
  }
}

class DefaultMeshPeerConnections implements ZLinkMeshPeerConnections {
  constructor(private readonly router: MutableSpotRouterCapabilityOptions) {}

  connect(endpoint: string): void;
  connect(expectedRoutingId: RoutingId, endpoint: string): void;
  connect(expectedRoutingIdOrEndpoint: RoutingId | string, endpoint?: string): void {
    if (endpoint === undefined) {
      requireRegistrationName(expectedRoutingIdOrEndpoint, 'Mesh peer endpoint');
      this.router.manualConnections ??= [];
      if (!this.router.manualConnections.includes(expectedRoutingIdOrEndpoint)) {
        this.router.manualConnections.push(expectedRoutingIdOrEndpoint);
      }
      return;
    }
    requireRegistrationName(endpoint, 'Mesh peer endpoint');
    this.router.manualPeerConnections ??= [];
    if (!this.router.manualPeerConnections.some(
      (peer) => peer.endpoint === endpoint && peer.peerRid === expectedRoutingIdOrEndpoint
    )) {
      this.router.manualPeerConnections.push({ peerRid: expectedRoutingIdOrEndpoint, endpoint });
    }
  }

  disconnect(endpoint: string): void {
    this.router.manualConnections = this.router.manualConnections?.filter((value) => value !== endpoint);
    this.router.manualPeerConnections = this.router.manualPeerConnections?.filter(
      (value) => value.endpoint !== endpoint
    );
  }

  listConnections(): readonly ZLinkMeshPeerConnection[] {
    return [
      ...(this.router.manualConnections ?? []).map((endpoint) => ({ endpoint })),
      ...(this.router.manualPeerConnections ?? []).map((peer) => ({
        endpoint: peer.endpoint,
        expectedRoutingId: peer.peerRid
      }))
    ];
  }
}

function endpointList(endpoint: string | readonly string[]): string[] {
  return typeof endpoint === 'string' ? [endpoint] : [...endpoint];
}

function requireStableObjectType(value: string, label: string): string {
  const byteLength = Buffer.byteLength(value, 'utf8');
  if (byteLength < 1 || byteLength > 255 || value.includes('\0')) {
    throw new ZLinkConfigurationException(
      `${label} must contain 1..255 UTF-8 bytes and no NUL.`
    );
  }
  return value;
}

function requirePositiveCapacity(value: number, label: string): number {
  if (!Number.isSafeInteger(value) || value <= 0 || value > 0x7fff_ffff) {
    throw new ZLinkConfigurationException(`${label} must be an integer in 1..2147483647.`);
  }
  return value;
}

function requireNonNegativeSafeInteger(value: number, label: string): number {
  if (!Number.isSafeInteger(value) || value < 0) {
    throw new ZLinkConfigurationException(
      `${label} must be a non-negative safe integer.`
    );
  }
  return value;
}

function requirePublicWeight(value: number, label: string): number {
  if (!Number.isInteger(value) || value < 0 || value > 10_000) {
    throw new ZLinkConfigurationException(`${label} must be an integer in 0..10000.`);
  }
  return value;
}

function requireFactoryConfigure(
  configure: unknown
): asserts configure is (builder: unknown) => void {
  if (typeof configure !== 'function') {
    throw new ZLinkConfigurationException(
      'Object factory requires a configure callback.'
    );
  }
}

function validateUserSpotFactoryOptions(
  options: ZLinkUserSpotFactoryConfiguration | undefined
): void {
  validateStableTypeLimit(options?.stableTypeLimit);
  const executionMode: unknown = options?.executionMode;
  if (
    executionMode !== undefined
    && executionMode !== ZLinkUserSpotExecutionMode.SpotWide
    && executionMode !== ZLinkUserSpotExecutionMode.PerActor
  ) {
    throw new ZLinkConfigurationException('User Spot executionMode is invalid.');
  }
  if (
    options?.relocationReadiness !== ZLinkSpotRelocationReadinessMode.AnyTurnBoundary
    && options?.relocationReadiness !== ZLinkSpotRelocationReadinessMode.ApplicationSignaled
  ) {
    throw new ZLinkConfigurationException('User Spot relocationReadiness is invalid.');
  }
  if (
    options.executionMode === ZLinkUserSpotExecutionMode.PerActor
    && options.relocationReadiness === ZLinkSpotRelocationReadinessMode.ApplicationSignaled
  ) {
    throw new ZLinkConfigurationException(
      'ApplicationSignaled relocation readiness is valid only for SpotWide User Spots.'
    );
  }
}

function validateStableTypeLimit(value: number | undefined): void {
  if (
    value !== undefined
    && (!Number.isInteger(value) || value < 1 || value > 2_147_483_647)
  ) {
    throw new ZLinkConfigurationException(
      'stableTypeLimit must be an integer from 1 through 2147483647.'
    );
  }
}

function rejectDuplicateObjectType(
  registrations: Readonly<Record<string, unknown>>,
  stableType: string,
  meshName: string
): void {
  if (Object.hasOwn(registrations, stableType)) {
    throw new ZLinkConfigurationException(
      `Duplicate object factory '${stableType}' on RouteMesh '${meshName}'.`
    );
  }
}

interface MutableFrameworkRegistrationOptions {
  network: ZLinkNetworkOptions;
  applicationVersion?: bigint;
  maintenanceWave?: string;
  actorTransferTimeoutMs?: number;
  messageFollowDurationMs?: number;
  codecs?: MutableCodecRegistryOptions;
  channels: Record<string, MutableChannelOptions>;
  streamNodes: Record<string, MutableStreamNodeOptions>;
  streamCompression?: MutableStreamCompressionOptions;
  spotNodes: Record<string, MutableSpotNodeOptions>;
  spotFactories: Type<ZLinkSpot>[];
  filters?: Type<ZLinkHandlerFilter>[];
  worker?: ZLinkWorkerOptions;
  inboundDispatch?: Partial<ZLinkInboundDispatchOptionValues>;
  dispatch?: ZLinkDispatchOptions;
  requestTimeoutMs?: number;
  locations?: MutableLocationRegistrationOptions;
}

interface MutableLocationRegistrationOptions {
  useInMemoryStores?: boolean;
  storeInstance?: ZLinkLocationStore;
  relocationStoreInstance?: ZLinkRelocationStore;
  options?: MutableLocationOptionValues;
}

type MutableLocationOptionValues = {
  -readonly [Key in keyof ZLinkLocationOptionValues]?: ZLinkLocationOptionValues[Key];
};

interface MutableChannelOptions {
  routingId?: string;
  routingIdPrefix?: string;
  publisher?: MutablePublisherCapabilityOptions;
  publishHandlers?: ZLinkChannelPublishHandlerRegistration[];
  subscriber?: MutableClientCapabilityOptions;
  client?: MutableClientCapabilityOptions;
  server?: MutableServerCapabilityOptions;
  handlerGroups?: string[];
  requestHandlers?: Array<{ packetName: string; handlerType: Type }>;
  sendHandlers?: Array<{ packetName: string; handlerType: Type }>;
}

interface MutableServerCapabilityOptions {
  bind?: string;
  bindHost?: string;
  advertiseHost?: string;
  port?: number;
  routingId?: string;
  weight?: number;
  sendHighWaterMark?: number;
  receiveHighWaterMark?: number;
  sendTimeoutMs?: number;
  maxMessageSize?: number;
}

interface MutableClientCapabilityOptions {
  manualConnections?: string[];
  sendHighWaterMark?: number;
  receiveHighWaterMark?: number;
  sendTimeoutMs?: number;
  maxMessageSize?: number;
}

interface MutablePublisherCapabilityOptions {
  bind?: string;
  bindHost?: string;
  advertiseHost?: string;
  port?: number;
}

interface MutableStreamNodeOptions {
  bind?: string;
  bindHost?: string;
  advertiseHost?: string;
  port?: number;
  maxMessageSize: number;
  actorDispatchEnabled?: boolean;
  tlsServer?: ZLinkStreamTlsServerOptions;
  session?: Type;
}

export interface MutableStreamCompressionOptions {
  disabled?: boolean;
  codec?: ZLinkStreamCompressionCodec;
}

interface MutableSpotNodeOptions {
  objectRole?: 'client' | 'server';
  placementWeight?: number;
  actorLimit?: number;
  spotLimit?: number;
  activationConcurrencyLimit?: number;
  instanceSpotIdleTimeoutMs?: number;
  routingId?: string;
  routingIdPrefix?: string;
  router?: MutableSpotRouterCapabilityOptions;
  pubSub?: MutableSpotPubSubCapabilityOptions;
  entrySpotType?: Type<ZLinkEntrySpot>;
  spotFactories?: Type<ZLinkSpot>[];
  spotFactoryRegistrations?: Record<string, MutableObjectFactoryRegistration<
    ZLinkSpot,
    ZLinkSpot,
    ZLinkUserSpotFactoryConfiguration
  >>;
  instanceSpotFactories?: Record<string, Type<ZLinkInstanceSpot>>;
  instanceSpotFactoryRegistrations?: Record<
    string,
    MutableObjectFactoryRegistration<
      ZLinkInstanceSpot,
      ZLinkInstanceSpot,
      ZLinkInstanceSpotFactoryConfiguration
    >
  >;
  actorFactories?: Record<string, Type>;
  actorFactoryRegistrations?: Record<
    string,
    MutableObjectFactoryRegistration<
      ZLinkActor,
      ZLinkActorFactory,
      ZLinkActorFactoryConfiguration
    >
  >;
  meshChannels?: Record<string, MutableMeshChannelOptions>;
  routeSendHandlers?: Array<{ packetName: string; handlerType: Type }>;
  routeRequestHandlers?: Array<{ packetName: string; handlerType: Type }>;
  requestTimeoutMs?: number;
  publisherConfig?: {
    sendHighWaterMark?: number;
    sendTimeoutMs?: number;
    lingerMs?: number;
  };
}

interface MutableObjectFactoryRegistration<T, TImplementation = T, TOptions = unknown> {
  readonly implementation: Type<TImplementation>;
  readonly options?: TOptions;
  readonly relocation: ZLinkRelocationConfiguration<T>;
}

interface MutableMeshChannelOptions {
  client?: boolean;
  server?: boolean;
  weight?: number;
  handlerGroups?: string[];
  sendHandlers?: Array<{ packetName: string; handlerType: Type }>;
  requestHandlers?: Array<{ packetName: string; handlerType: Type }>;
}

interface MutableSpotRouterCapabilityOptions {
  bind?: string;
  bindHost?: string;
  advertiseHost?: string;
  port?: number;
  manualConnections?: string[];
  manualPeerConnections?: ZLinkSpotRouterPeerConnectionOptions[];
  routingId?: string;
  maxMessageSize?: number;
  sendHighWaterMark?: number;
  receiveHighWaterMark?: number;
  receiveTimeoutMs?: number;
  sendTimeoutMs?: number;
}

interface MutableSpotPubSubCapabilityOptions {
  bind?: string;
  manualConnections?: string[];
  routingId?: string;
}

function handlerPacketName(handlerType: Type): string {
  return readZLinkDecoratorMetadata(handlerType)
    .find((metadata) => metadata.kind === 'packet' && metadata.packetName !== undefined)
    ?.packetName ?? handlerType.name;
}

function requireRegistrationName(value: string, label: string): void {
  if (value.trim().length === 0 || value.trim() !== value) {
    throw new ZLinkConfigurationException(`${label} must not be empty or padded.`);
  }
}

function requireListenerPort(value: number, label: string): number {
  if (!Number.isInteger(value) || value < 0 || value > 65_535) {
    throw new ZLinkConfigurationException(`${label} must be between 0 and 65535.`);
  }
  return value;
}

function tcpEndpoint(host: string, port: number): string {
  const endpointHost = host.includes(':') && !host.startsWith('[') ? `[${host}]` : host;
  return `tcp://${endpointHost}:${port}`;
}

function rejectFixedRoutingId(routingId: string | undefined, memberName: string): void {
  if (routingId !== undefined) {
    throw new ZLinkConfigurationException(
      `Mesh member '${memberName}' cannot combine a fixed routing id with a generated prefix.`
    );
  }
}

function rejectGeneratedRoutingId(prefix: string | undefined, memberName: string): void {
  if (prefix !== undefined) {
    throw new ZLinkConfigurationException(
      `Mesh member '${memberName}' cannot combine a generated routing-id prefix with a fixed id.`
    );
  }
}
