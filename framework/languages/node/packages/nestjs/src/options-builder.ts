import type {
  Type,
  ZLinkActor,
  ZLinkActorFactory,
  ZLinkCodecExtension,
  ZLinkCodecRegistrar,
  ZLinkDispatchOptionsBuilder,
  ZLinkInboundDispatchOptions,
  ZLinkEntrySpot,
  ZLinkLocationStore,
  ZLinkLocationOptionValues,
  ZLinkLocationOptions,
  ZLinkRelocationStore,
  ZLinkActorFactoryBuilder,
  ZLinkActorRelocationAdapter,
  ZLinkInstanceSpotFactoryBuilder,
  ZLinkSpotRelocationAdapter,
  ZLinkUserSpotFactoryBuilder,
  ZLinkMeshNodeSocketConfig,
  ZLinkMeshPeerConnection,
  ZLinkMeshPeerConnections,
  ZLinkMessageSerializer,
  ZLinkNetworkOptions,
  ZLinkSession,
  ZLinkSessionFactory,
  ZLinkSpot,
  ZLinkInstanceSpot,
  ZLinkSpotPublisherConfig,
  ZLinkStreamCompressionBuilder,
} from '@zlink-systems/framework';
import type {
  ZLinkFrameworkRegistrationOptions,
  ZLinkActorFactoryConfiguration,
  ZLinkInstanceSpotFactoryConfiguration,
  ZLinkRelocationConfiguration,
  ZLinkPublisherCapabilityOptions,
  ZLinkSpotNodeOptions,
  ZLinkStreamNodeOptions,
  ZLinkUserSpotFactoryConfiguration
} from './framework-integration-contracts';
import {
  ZLinkMessageFlowLogMode,
  ZLinkSpotRelocationReadinessMode,
  ZLinkUserSpotExecutionMode,
  ZLinkUnhandledDispatchAction
} from '@zlink-systems/framework';
import {
  ZLINK_MODULE_OPTIONS_BRAND,
  type InternalZLinkNestClientServerChannelOptions,
  type Mutable,
  type MutableCodecRegistryOptions,
  type ZLinkModuleOptions,
  type ZLinkNestClientServerChannelClientBuilder,
  type ZLinkNestClientServerChannelRoleBuilder,
  type ZLinkNestClientServerChannelServerBuilder,
  type ZLinkNestCodecRegistryBuilder,
  type ZLinkNestFanoutChannelBuilder,
  type InternalZLinkNestFanoutChannelOptions,
  type ZLinkNestFrameworkAdditionalOptions,
  type ZLinkNestFrameworkOptionsBuilder,
  type ZLinkNestModuleRegistrationOptions,
  type ZLinkNestMeshChannelBuilder,
  type ZLinkNestMeshChannelClientBuilder,
  type ZLinkNestMeshChannelServerBuilder,
  type ZLinkNestMeshObjectClientBuilder,
  type ZLinkNestMeshObjectRoleBuilder,
  type ZLinkNestMeshObjectServerBuilder,
  type ZLinkNestMeshNodeBuilder,
  type ZLinkNestStreamNodeBuilder
} from './contracts';
import { framework } from './framework-loader';


type ZLinkNestBuilderAdditionalOptions = Omit<
  ZLinkFrameworkRegistrationOptions,
  'channels' | 'routeChannels' | 'streamNodes' | 'spotNodes' | 'codecs'
>;

interface ZLinkNestBuilderState {
  additionalOptions: ZLinkNestBuilderAdditionalOptions;
  readonly clientServerChannels: Record<string, InternalZLinkNestClientServerChannelOptions>;
  readonly fanoutChannels: Record<string, InternalZLinkNestFanoutChannelOptions>;
  readonly streams: Record<string, ZLinkStreamNodeOptions>;
  readonly spotNodes: Record<string, ZLinkSpotNodeOptions>;
  readonly codecOptions: MutableCodecRegistryOptions;
  readonly codecRegistry: ReturnType<typeof framework.createIntegrationCodecRegistryBuilder>;
}

function createBuilderState(): ZLinkNestBuilderState {
  const codecOptions: MutableCodecRegistryOptions = { serializers: [], streamCodecs: [] };
  return {
    additionalOptions: {},
    clientServerChannels: {},
    fanoutChannels: {},
    streams: {},
    spotNodes: {},
    codecOptions,
    codecRegistry: framework.createIntegrationCodecRegistryBuilder(codecOptions)
  };
}

function registerSerializer(
  state: ZLinkNestBuilderState,
  contentType: string,
  serializer: ZLinkMessageSerializer
): void {
  state.codecRegistry.addSerializer(contentType, serializer);
}

function registerStreamCodec(state: ZLinkNestBuilderState, contentType: string, codec: unknown): void {
  state.codecRegistry.addStreamCodec(contentType, codec);
}

function ensureChannelAvailable(state: ZLinkNestBuilderState, name: string): void {
  if (name.trim().length === 0 || name.trim() !== name) {
    throw new framework.ZLinkConfigurationException('Channel name must not be empty or padded.');
  }
  if (
    Object.prototype.hasOwnProperty.call(state.fanoutChannels, name)
    || Object.prototype.hasOwnProperty.call(state.clientServerChannels, name)
  ) {
    throw new framework.ZLinkConfigurationException(`Duplicate channel '${name}'.`);
  }
}

function ensureClientServerChannelAvailable(state: ZLinkNestBuilderState, name: string): void {
  if (name.trim().length === 0 || name.trim() !== name) {
    throw new framework.ZLinkConfigurationException('Channel name must not be empty or padded.');
  }
  if (Object.prototype.hasOwnProperty.call(state.fanoutChannels, name)) {
    throw new framework.ZLinkConfigurationException(`Duplicate channel '${name}'.`);
  }
}

abstract class ZLinkNestOptionsBuilder implements ZLinkNestFrameworkOptionsBuilder {
  protected constructor(protected readonly state: ZLinkNestBuilderState) {}

  options(options: ZLinkNestFrameworkAdditionalOptions): this {
    this.state.additionalOptions = { ...this.state.additionalOptions, ...options };
    return this;
  }

  codecs(): ZLinkNestCodecRegistryBuilder {
    return new DefaultZLinkNestCodecRegistryBuilder(this.state);
  }

  configureDispatch(): ZLinkDispatchOptionsBuilder {
    this.state.additionalOptions = {
      ...this.state.additionalOptions,
      dispatch: this.state.additionalOptions.dispatch ?? {
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
      }
    };
    return framework.createIntegrationDispatchOptionsBuilder(
      this.state.additionalOptions.dispatch as NonNullable<ZLinkFrameworkRegistrationOptions['dispatch']>
    );
  }

  configureInboundDispatch(): ZLinkInboundDispatchOptions {
    const inboundDispatch = {
      ...(this.state.additionalOptions.inboundDispatch ?? {})
    };
    this.state.additionalOptions = {
      ...this.state.additionalOptions,
      inboundDispatch
    };
    return framework.createIntegrationInboundDispatchOptionsBuilder(inboundDispatch);
  }

  addLocationStore(store: ZLinkLocationStore): this {
    this.state.additionalOptions = {
      ...this.state.additionalOptions,
      locations: {
        ...(this.state.additionalOptions.locations ?? {}),
        storeInstance: store
      }
    };
    return this;
  }

  addRelocationStore(store: ZLinkRelocationStore): this {
    this.state.additionalOptions = {
      ...this.state.additionalOptions,
      locations: {
        ...(this.state.additionalOptions.locations ?? {}),
        relocationStoreInstance: store
      }
    };
    return this;
  }

  setApplicationVersion(version: bigint): this {
    this.state.additionalOptions = {
      ...this.state.additionalOptions,
      applicationVersion: version
    };
    return this;
  }

  setMaintenanceWave(waveId: string): this {
    this.state.additionalOptions = {
      ...this.state.additionalOptions,
      maintenanceWave: waveId
    };
    return this;
  }

  setActorTransferTimeout(timeoutMs: number): this {
    this.state.additionalOptions = {
      ...this.state.additionalOptions,
      actorTransferTimeoutMs: framework.validateActorTransferTimeout(timeoutMs)
    };
    return this;
  }

  setMessageFollowDuration(timeoutMs: number): this {
    this.state.additionalOptions = {
      ...this.state.additionalOptions,
      messageFollowDurationMs: framework.validateMessageFollowDuration(timeoutMs)
    };
    return this;
  }

  configureStreamCompression(): ZLinkStreamCompressionBuilder {
    const compression = { ...(this.state.additionalOptions.streamCompression ?? {}) };
    this.state.additionalOptions = { ...this.state.additionalOptions, streamCompression: compression };
    return framework.createIntegrationStreamCompressionBuilder(compression);
  }

  configureLocations(): ZLinkLocationOptions {
    this.state.additionalOptions = {
      ...this.state.additionalOptions,
      locations: this.state.additionalOptions.locations ?? { options: {} }
    };
    const locations = this.state.additionalOptions.locations as NonNullable<ZLinkFrameworkRegistrationOptions['locations']>;
    (locations as { options?: Partial<ZLinkLocationOptionValues> }).options ??= {};
    return framework.createIntegrationLocationOptionsBuilder(
      locations.options as Partial<ZLinkLocationOptionValues>
    );
  }

  configureNetwork(): ZLinkNetworkOptions {
    const network = {
      bindHost: '127.0.0.1',
      ...(this.state.additionalOptions.network ?? {})
    };
    this.state.additionalOptions = {
      ...this.state.additionalOptions,
      network
    };
    return network;
  }

  addFanoutChannel(name: string): ZLinkNestFanoutChannelBuilder {
    ensureChannelAvailable(this.state, name);
    this.state.fanoutChannels[name] = {};
    return new DefaultZLinkNestFanoutChannelBuilder(this.state, name, this.state.fanoutChannels[name]);
  }

  addClientServerChannel(name: string): ZLinkNestClientServerChannelRoleBuilder {
    ensureClientServerChannelAvailable(this.state, name);
    this.state.clientServerChannels[name] ??= {};
    return new DefaultZLinkNestClientServerChannelRoleBuilder(
      this.state,
      name,
      this.state.clientServerChannels[name]
    );
  }

  addRouteMesh(name: string): ZLinkNestMeshNodeBuilder {
    if (name.trim().length === 0 || name.trim() !== name) {
      throw new framework.ZLinkConfigurationException('RouteMesh name must not be empty or padded.');
    }
    if (Object.prototype.hasOwnProperty.call(this.state.spotNodes, name)) {
      throw new framework.ZLinkConfigurationException(`Duplicate RouteMesh '${name}'.`);
    }
    this.state.spotNodes[name] = { router: { port: 0 } };
    return new DefaultZLinkNestMeshNodeBuilder(this.state, name, this.state.spotNodes[name]);
  }

  addStreamNode(name: string): ZLinkNestStreamNodeBuilder {
    this.state.streams[name] ??= {};
    return new DefaultZLinkNestStreamNodeBuilder(this.state, this.state.streams[name]);
  }

  build(): ZLinkModuleOptions {
    const options: ZLinkNestModuleRegistrationOptions = {
      [ZLINK_MODULE_OPTIONS_BRAND]: true,
      ...this.state.additionalOptions,
      clientServerChannels: { ...this.state.clientServerChannels },
      fanoutChannels: { ...this.state.fanoutChannels },
      streams: { ...this.state.streams },
      spotNodes: { ...this.state.spotNodes },
      codecs: this.state.codecOptions.serializers.length === 0 &&
          this.state.codecOptions.streamCodecs.length === 0
        ? undefined
        : {
            serializers: [...this.state.codecOptions.serializers],
            streamCodecs: [...this.state.codecOptions.streamCodecs]
          }
    };
    return options;
  }
}

class DefaultZLinkNestFrameworkOptionsBuilder extends ZLinkNestOptionsBuilder {
  constructor() {
    super(createBuilderState());
  }
}

class DefaultZLinkNestCodecRegistryBuilder extends ZLinkNestOptionsBuilder implements ZLinkNestCodecRegistryBuilder, ZLinkCodecRegistrar {
  constructor(state: ZLinkNestBuilderState) {
    super(state);
  }

  addSerializer(contentType: string, serializer: ZLinkMessageSerializer): this {
    registerSerializer(this.state, contentType, serializer);
    return this;
  }

  addStreamCodec(contentType: string, codec: unknown): this {
    if (contentType.trim().length === 0) {
      throw new Error('Codec content type must not be empty.');
    }
    registerStreamCodec(this.state, contentType, codec);
    return this;
  }

  use(extension: ZLinkCodecExtension): this {
    extension.register(this);
    return this;
  }

}

class DefaultZLinkNestFanoutChannelBuilder extends ZLinkNestOptionsBuilder implements ZLinkNestFanoutChannelBuilder {
  private subscriberMode?: 'automatic' | 'manual';

  constructor(state: ZLinkNestBuilderState, private readonly name: string, private readonly channelOptions: Mutable<InternalZLinkNestFanoutChannelOptions>) {
    super(state);
  }

  enablePublisher(endpointOrPort?: string | number): this {
    this.channelOptions.publisher = typeof endpointOrPort === 'string'
      ? { bind: endpointOrPort }
      : { port: requireListenerPort(endpointOrPort, `Fanout channel '${this.name}' publisher`) };
    return this;
  }

  setBindHost(bindHost: string): this {
    requireClientServerText(bindHost, `Fanout channel '${this.name}' publisher bind host`);
    this.updatePublisher({ bindHost });
    return this;
  }

  setAdvertiseHost(advertiseHost: string): this {
    requireClientServerText(advertiseHost, `Fanout channel '${this.name}' publisher advertise host`);
    this.updatePublisher({ advertiseHost });
    return this;
  }

  routingId(routingId: string | undefined): this {
    rejectGeneratedRoutingId(this.channelOptions.routingIdPrefix, this.name);
    this.channelOptions.routingId = routingId;
    return this;
  }

  setRoutingIdPrefix(prefix: string): this {
    rejectFixedRoutingId(this.channelOptions.routingId, this.name);
    this.channelOptions.routingIdPrefix = framework.validateRoutingIdPrefix(prefix);
    return this;
  }

  enableSubscriber(endpoint?: string | readonly string[]): this {
    const mode = endpoint === undefined ? 'automatic' : 'manual';
    if (this.subscriberMode !== undefined && this.subscriberMode !== mode) {
      throw new framework.ZLinkConfigurationException(
        `Fanout channel '${this.name}' cannot combine automatic and manual subscriber sources.`
      );
    }
    this.subscriberMode = mode;
    this.channelOptions.subscriber = endpoint === undefined ? {} : { manualConnections: endpointList(endpoint) };
    return this;
  }

  addHandlerGroup(groupName: string): this {
    this.channelOptions.handlerGroups = [...(this.channelOptions.handlerGroups ?? []), groupName];
    return this;
  }

  addPublishHandler(packetName: string, handlerType: Type): this {
    this.channelOptions.publishHandlerTypes = [...(this.channelOptions.publishHandlerTypes ?? []), { packetName, handlerType }];
    return this;
  }

  private updatePublisher(values: Partial<ZLinkPublisherCapabilityOptions>): void {
    if (this.channelOptions.publisher === undefined) {
      throw new framework.ZLinkConfigurationException(
        `Fanout channel '${this.name}' publisher must be enabled before configuring its listener.`
      );
    }
    this.channelOptions.publisher = { ...this.channelOptions.publisher, ...values };
  }
}

class DefaultZLinkNestClientServerChannelRoleBuilder extends ZLinkNestOptionsBuilder implements ZLinkNestClientServerChannelRoleBuilder {
  constructor(
    state: ZLinkNestBuilderState,
    private readonly name: string,
    private readonly channel: Mutable<InternalZLinkNestClientServerChannelOptions>
  ) {
    super(state);
  }

  client(): ZLinkNestClientServerChannelClientBuilder {
    if (this.channel.client !== undefined) {
      throw this.duplicate('Client');
    }
    this.channel.client = { manualConnections: [] };
    return new DefaultZLinkNestClientServerChannelClientBuilder(this.state, this.name, this.channel);
  }

  server(): ZLinkNestClientServerChannelServerBuilder {
    if (this.channel.server !== undefined) {
      throw this.duplicate('Server');
    }
    this.channel.server = {};
    return new DefaultZLinkNestClientServerChannelServerBuilder(this.state, this.name, this.channel);
  }

  private duplicate(role: 'Client' | 'Server') {
    return new framework.ZLinkConfigurationException(
      `ClientServer channel '${this.name}' ${role} role is already registered.`
    );
  }
}

class DefaultZLinkNestClientServerChannelClientBuilder extends ZLinkNestOptionsBuilder implements ZLinkNestClientServerChannelClientBuilder {
  constructor(
    state: ZLinkNestBuilderState,
    private readonly name: string,
    private readonly channel: Mutable<InternalZLinkNestClientServerChannelOptions>
  ) {
    super(state);
  }

  connect(endpoint: string): this {
    requireClientServerText(endpoint, `ClientServer channel '${this.name}' endpoint`);
    const connections = [...(this.channel.client?.manualConnections ?? [])];
    if (!connections.includes(endpoint)) connections.push(endpoint);
    this.channel.client = { ...this.channel.client, manualConnections: connections };
    return this;
  }
}

class DefaultZLinkNestClientServerChannelServerBuilder extends ZLinkNestOptionsBuilder implements ZLinkNestClientServerChannelServerBuilder {
  constructor(
    state: ZLinkNestBuilderState,
    private readonly name: string,
    private readonly channel: Mutable<InternalZLinkNestClientServerChannelOptions>
  ) {
    super(state);
  }

  listen(port = 0): this {
    if (!Number.isInteger(port) || port < 0 || port > 65_535) {
      throw new framework.ZLinkConfigurationException(
        `ClientServer channel '${this.name}' port must be between 0 and 65535.`
      );
    }
    this.updateServer({ port });
    this.updateBind();
    return this;
  }

  setBindHost(bindHost: string): this {
    requireClientServerText(bindHost, `ClientServer channel '${this.name}' bind host`);
    this.updateServer({ bindHost });
    if (this.server.port !== undefined) this.updateBind();
    return this;
  }

  setAdvertiseHost(advertiseHost: string): this {
    requireClientServerText(advertiseHost, `ClientServer channel '${this.name}' advertise host`);
    this.updateServer({ advertiseHost });
    return this;
  }

  setWeight(weight: number): this {
    if (!Number.isInteger(weight) || weight < 0 || weight > 10_000) {
      throw new framework.ZLinkConfigurationException(
        `ClientServer channel '${this.name}' weight must be between 0 and 10000.`
      );
    }
    this.updateServer({ weight });
    return this;
  }

  addSendHandler(packetName: string, handlerType: Type): this {
    requireClientServerText(packetName, `ClientServer channel '${this.name}' send packet name`);
    this.channel.sendHandlerTypes = [
      ...(this.channel.sendHandlerTypes ?? []),
      { packetName, handlerType }
    ];
    return this;
  }

  addRequestHandler(packetName: string, handlerType: Type): this {
    requireClientServerText(packetName, `ClientServer channel '${this.name}' request packet name`);
    this.channel.requestHandlerTypes = [
      ...(this.channel.requestHandlerTypes ?? []),
      { packetName, handlerType }
    ];
    return this;
  }

  addHandlerGroup(groupName: string): this {
    requireClientServerText(groupName, `ClientServer channel '${this.name}' handler group`);
    this.channel.handlerGroups = [...(this.channel.handlerGroups ?? []), groupName];
    return this;
  }

  private get server(): NonNullable<InternalZLinkNestClientServerChannelOptions['server']> {
    return this.channel.server!;
  }

  private updateServer(
    values: Partial<NonNullable<InternalZLinkNestClientServerChannelOptions['server']>>
  ): void {
    this.channel.server = { ...this.channel.server, ...values };
  }

  private updateBind(): void {
    const host = this.server.bindHost ?? '127.0.0.1';
    const endpointHost = host.includes(':') && !host.startsWith('[') ? `[${host}]` : host;
    this.updateServer({ bind: `tcp://${endpointHost}:${this.server.port ?? 0}` });
  }
}

function requireClientServerText(value: string, label: string): void {
  if (value.trim().length === 0 || value.trim() !== value) {
    throw new framework.ZLinkConfigurationException(`${label} must not be empty or padded.`);
  }
}

function requireListenerPort(port: number | undefined, label: string): number {
  const normalized = port ?? 0;
  if (!Number.isInteger(normalized) || normalized < 0 || normalized > 65_535) {
    throw new framework.ZLinkConfigurationException(`${label} port must be between 0 and 65535.`);
  }
  return normalized;
}

function requirePublicWeight(value: number, label: string): number {
  if (!Number.isInteger(value) || value < 0 || value > 10_000) {
    throw new framework.ZLinkConfigurationException(
      `${label} must be an integer in 0..10000.`
    );
  }
  return value;
}

function rejectFixedRoutingId(routingId: string | undefined, memberName: string): void {
  if (routingId !== undefined) {
    throw new framework.ZLinkConfigurationException(
      `Mesh member '${memberName}' cannot combine a fixed routing id with a generated prefix.`
    );
  }
}

function rejectGeneratedRoutingId(prefix: string | undefined, memberName: string): void {
  if (prefix !== undefined) {
    throw new framework.ZLinkConfigurationException(
      `Mesh member '${memberName}' cannot combine a generated routing-id prefix with a fixed id.`
    );
  }
}

class DefaultZLinkNestStreamNodeBuilder extends ZLinkNestOptionsBuilder implements ZLinkNestStreamNodeBuilder {
  constructor(state: ZLinkNestBuilderState, private readonly streamOptions: Mutable<ZLinkStreamNodeOptions>) {
    super(state);
  }

  bind(endpointOrPort?: string | number): this {
    if (typeof endpointOrPort === 'string') {
      this.streamOptions.bind = endpointOrPort;
      delete this.streamOptions.port;
    } else {
      this.streamOptions.port = requireListenerPort(endpointOrPort, 'STREAM listener');
      delete this.streamOptions.bind;
    }
    return this;
  }

  setBindHost(bindHost: string): this {
    requireClientServerText(bindHost, 'STREAM bind host');
    this.streamOptions.bindHost = bindHost;
    return this;
  }

  setAdvertiseHost(advertiseHost: string): this {
    requireClientServerText(advertiseHost, 'STREAM advertise host');
    this.streamOptions.advertiseHost = advertiseHost;
    return this;
  }

  enableActorDispatch(): this {
    if (this.streamOptions.actorDispatchEnabled === true) {
      throw new framework.ZLinkConfigurationException(
        'STREAM node actor dispatch is already enabled.'
      );
    }
    this.streamOptions.actorDispatchEnabled = true;
    return this;
  }

  setTlsServer(certificatePath: string, keyPath: string, requireClientCertificate: boolean = false): this {
    this.streamOptions.tlsServer = {
      certificatePath,
      keyPath,
      requireClientCertificate
    };
    return this;
  }

  registerSession<TSession extends ZLinkSession>(sessionType: Type<TSession> | Type<ZLinkSessionFactory<TSession>>): this {
    if (this.streamOptions.session !== undefined) {
      throw new framework.ZLinkConfigurationException('STREAM node cannot register more than one header stream session.');
    }
    this.streamOptions.session = sessionType as Type;
    return this;
  }

}

class DefaultZLinkNestMeshNodeBuilder extends ZLinkNestOptionsBuilder implements ZLinkNestMeshNodeBuilder {
  constructor(state: ZLinkNestBuilderState, private readonly name: string, private readonly spotOptions: Mutable<ZLinkSpotNodeOptions>) {
    super(state);
  }

  channel(name: string): ZLinkNestMeshChannelBuilder {
    if (name.trim().length === 0 || name.trim() !== name) {
      throw new framework.ZLinkConfigurationException('Mesh channel name must not be empty or padded.');
    }
    const channels = {
      ...(this.spotOptions.meshChannels ?? {})
    } as Record<string, Mutable<NonNullable<ZLinkSpotNodeOptions['meshChannels']>[string]>>;
    if (Object.prototype.hasOwnProperty.call(channels, name)) {
      throw new framework.ZLinkConfigurationException(
        `Duplicate channel '${name}' in RouteMesh '${this.name}'.`
      );
    }
    channels[name] = {};
    this.spotOptions.meshChannels = channels;
    return new DefaultZLinkNestMeshChannelBuilder(this.state, channels[name]);
  }

  listen(endpointOrPort?: string | number): this {
    this.spotOptions.router = typeof endpointOrPort === 'string'
      ? { ...(this.spotOptions.router ?? {}), bind: endpointOrPort, port: undefined }
      : {
          ...(this.spotOptions.router ?? {}),
          bind: undefined,
          port: requireListenerPort(endpointOrPort, `RouteMesh '${this.name}' listener`)
        };
    return this;
  }

  setBindHost(bindHost: string): this {
    requireClientServerText(bindHost, `RouteMesh '${this.name}' bind host`);
    this.spotOptions.router = { ...(this.spotOptions.router ?? {}), bindHost };
    return this;
  }

  setAdvertiseHost(advertiseHost: string): this {
    requireClientServerText(advertiseHost, `RouteMesh '${this.name}' advertise host`);
    this.spotOptions.router = { ...(this.spotOptions.router ?? {}), advertiseHost };
    return this;
  }

  routingId(routingId: string | undefined): this {
    rejectGeneratedRoutingId(this.spotOptions.routingIdPrefix, this.name);
    this.spotOptions.routingId = routingId;
    if (this.spotOptions.router !== undefined) {
      (this.spotOptions.router as Mutable<NonNullable<ZLinkSpotNodeOptions['router']>>).routingId = routingId;
    }
    if (this.spotOptions.pubSub !== undefined) {
      (this.spotOptions.pubSub as Mutable<NonNullable<ZLinkSpotNodeOptions['pubSub']>>).routingId = routingId;
    }
    return this;
  }

  setRoutingIdPrefix(prefix: string): this {
    rejectFixedRoutingId(this.spotOptions.routingId, this.name);
    this.spotOptions.routingIdPrefix = framework.validateRoutingIdPrefix(prefix);
    return this;
  }

  setPlacementWeight(weight: number): this {
    if (!Number.isInteger(weight) || weight < 0 || weight > 10_000) {
      throw new framework.ZLinkConfigurationException(
        'Placement weight must be an integer in 0..10000.'
      );
    }
    this.spotOptions.placementWeight = weight;
    return this;
  }

  setActorLimit(limit: number): this {
    this.spotOptions.actorLimit = requirePositiveCapacity(limit, 'Actor limit');
    return this;
  }

  setSpotLimit(limit: number): this {
    this.spotOptions.spotLimit = requirePositiveCapacity(limit, 'Spot limit');
    return this;
  }

  setActivationConcurrency(limit: number): this {
    this.spotOptions.activationConcurrencyLimit = requirePositiveCapacity(
      limit,
      'Activation concurrency limit'
    );
    return this;
  }

  setInstanceSpotIdleTimeout(timeoutMs: number): this {
    this.spotOptions.instanceSpotIdleTimeoutMs = requireNonNegativeSafeInteger(
      timeoutMs,
      'Instance Spot idle timeout'
    );
    return this;
  }

  configureRouterSocket(): ZLinkMeshNodeSocketConfig {
    this.spotOptions.router ??= {};
    return this.spotOptions.router as ZLinkMeshNodeSocketConfig;
  }

  configureSpotPublisher(): ZLinkSpotPublisherConfig {
    this.spotOptions.publisherConfig ??= {};
    return this.spotOptions.publisherConfig as ZLinkSpotPublisherConfig;
  }

  peerConnections(): ZLinkMeshPeerConnections {
    this.spotOptions.router ??= {};
    return new DefaultZLinkNestMeshPeerConnections(this.spotOptions.router);
  }

  objects(): ZLinkNestMeshObjectRoleBuilder {
    return new DefaultZLinkNestMeshObjectRoleBuilder(
      this.state,
      this.name,
      this.spotOptions
    );
  }

  addSendHandler(packetName: string, handlerType: Type): this {
    this.spotOptions.routeSendHandlers = [
      ...(this.spotOptions.routeSendHandlers ?? []),
      { packetName, handlerType }
    ];
    return this;
  }

  addRequestHandler(packetName: string, handlerType: Type): this {
    this.spotOptions.routeRequestHandlers = [
      ...(this.spotOptions.routeRequestHandlers ?? []),
      { packetName, handlerType }
    ];
    return this;
  }

}

class DefaultZLinkNestMeshObjectRoleBuilder
  extends ZLinkNestOptionsBuilder
  implements ZLinkNestMeshObjectRoleBuilder {
  constructor(
    state: ZLinkNestBuilderState,
    private readonly meshName: string,
    private readonly node: Mutable<ZLinkSpotNodeOptions>
  ) {
    super(state);
  }

  client(): ZLinkNestMeshObjectClientBuilder {
    this.node.objectRole = 'client';
    return new DefaultZLinkNestMeshObjectClientBuilder(this.state);
  }

  server(): ZLinkNestMeshObjectServerBuilder {
    this.node.objectRole = 'server';
    return new DefaultZLinkNestMeshObjectServerBuilder(
      this.state,
      this.meshName,
      this.node
    );
  }
}

class DefaultZLinkNestMeshObjectClientBuilder
  extends ZLinkNestOptionsBuilder
  implements ZLinkNestMeshObjectClientBuilder {}

abstract class DefaultZLinkNestFactoryBuilder<TInstance> {
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
      throw new framework.ZLinkConfigurationException(
        'PreserveStateWith relocation requires an adapter type.'
      );
    }
    this.select({ kind: 'snapshot', adapterType });
  }

  protected relocationConfiguration(): ZLinkRelocationConfiguration<TInstance> {
    if (this.relocation === undefined) {
      throw new framework.ZLinkConfigurationException(
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
      throw new framework.ZLinkConfigurationException(
        'Factory builder cannot be changed after the configure callback returns.'
      );
    }
  }

  private select(relocation: ZLinkRelocationConfiguration<TInstance>): void {
    this.assertMutable();
    if (this.relocation !== undefined) {
      throw new framework.ZLinkConfigurationException(
        'Factory configure callback must select exactly one relocation policy.'
      );
    }
    this.relocation = relocation;
  }
}

class DefaultZLinkNestActorFactoryBuilder<TActor extends ZLinkActor>
  extends DefaultZLinkNestFactoryBuilder<TActor>
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
    return { options: {}, relocation: this.relocationConfiguration() };
  }
}

class DefaultZLinkNestUserSpotFactoryBuilder<TSpot extends ZLinkSpot>
  extends DefaultZLinkNestFactoryBuilder<TSpot>
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
    validateUserSpotFactoryConfiguration(options);
    if (
      options.executionMode === ZLinkUserSpotExecutionMode.PerActor
      && relocation.kind !== 'recreate'
    ) {
      throw new framework.ZLinkConfigurationException(
        'PerActor User Spots require RecreateOnRelocation.'
      );
    }
    return { options, relocation };
  }
}

class DefaultZLinkNestInstanceSpotFactoryBuilder<TSpot extends ZLinkInstanceSpot>
  extends DefaultZLinkNestFactoryBuilder<TSpot>
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

class DefaultZLinkNestMeshObjectServerBuilder
  extends ZLinkNestOptionsBuilder
  implements ZLinkNestMeshObjectServerBuilder {
  constructor(
    state: ZLinkNestBuilderState,
    private readonly meshName: string,
    private readonly node: Mutable<ZLinkSpotNodeOptions>
  ) {
    super(state);
  }

  addEntrySpot<TEntrySpot extends ZLinkEntrySpot>(
    entrySpotType: Type<TEntrySpot>
  ): this {
    framework.registerEntrySpot(this.node, entrySpotType);
    return this;
  }

  addSpotFactory<TSpot extends ZLinkSpot>(
    spotType: string,
    implementation: Type<TSpot>,
    configure: (builder: ZLinkUserSpotFactoryBuilder<TSpot>) => void
  ): this {
    const stableType = validateObjectFactory(spotType, 'User Spot type');
    requireFactoryConfigure(configure);
    const factory = new DefaultZLinkNestUserSpotFactoryBuilder<TSpot>();
    let built: ReturnType<typeof factory.build>;
    try {
      configure(factory);
      built = factory.build();
    } finally {
      factory.seal();
    }
    const { options, relocation } = built;
    const registrations = {
      ...(this.node.spotFactoryRegistrations ?? {})
    };
    rejectDuplicateObjectType(registrations, stableType, this.meshName);
    registrations[stableType] = {
      implementation,
      options: {
        ...options,
        executionMode: options.executionMode
      },
      relocation
    };
    this.node.spotFactoryRegistrations = registrations;
    const factories = [...(this.node.spotFactories ?? [])];
    framework.registerSpotFactory({ spotFactories: factories }, implementation);
    this.node.spotFactories = factories;
    return this;
  }

  addInstanceSpotFactory<TSpot extends ZLinkInstanceSpot>(
    instanceSpotType: string,
    implementation: Type<TSpot>,
    configure: (builder: ZLinkInstanceSpotFactoryBuilder<TSpot>) => void
  ): this {
    const stableType = validateObjectFactory(instanceSpotType, 'Instance Spot type');
    requireFactoryConfigure(configure);
    const factory = new DefaultZLinkNestInstanceSpotFactoryBuilder<TSpot>();
    let built: ReturnType<typeof factory.build>;
    try {
      configure(factory);
      built = factory.build();
    } finally {
      factory.seal();
    }
    const { options, relocation } = built;
    validateStableTypeLimit(options.stableTypeLimit);
    const factories = {
      ...(this.node.instanceSpotFactories ?? {})
    };
    if (Object.hasOwn(factories, stableType)) {
      throw new framework.ZLinkConfigurationException(
        `Duplicate Instance Spot factory '${stableType}' on RouteMesh '${this.meshName}'.`
      );
    }
    factories[stableType] = implementation;
    this.node.instanceSpotFactories = factories;
    this.node.instanceSpotFactoryRegistrations = {
      ...(this.node.instanceSpotFactoryRegistrations ?? {}),
      [stableType]: { implementation, options, relocation }
    };
    return this;
  }

  addActorFactory<TActor extends ZLinkActor>(
    actorType: string,
    implementation: Type<ZLinkActorFactory<TActor>>,
    configure: (builder: ZLinkActorFactoryBuilder<TActor>) => void
  ): this {
    const stableType = validateObjectFactory(actorType, 'Actor type');
    requireFactoryConfigure(configure);
    const factory = new DefaultZLinkNestActorFactoryBuilder<TActor>();
    let built: ReturnType<typeof factory.build>;
    try {
      configure(factory);
      built = factory.build();
    } finally {
      factory.seal();
    }
    const { options, relocation } = built;
    const registrations = {
      ...(this.node.actorFactoryRegistrations ?? {})
    };
    rejectDuplicateObjectType(registrations, stableType, this.meshName);
    const actorFactories = {
      ...(this.node.actorFactories as Readonly<Record<string, Type>> | undefined)
    };
    framework.registerActorFactory({ actorFactories }, stableType, implementation);
    this.node.actorFactories = actorFactories;
    registrations[stableType] = { implementation, options, relocation };
    this.node.actorFactoryRegistrations = registrations;
    return this;
  }
}

function validateObjectFactory(
  stableType: string,
  label: string
): string {
  if (
    typeof stableType !== 'string'
    || Buffer.byteLength(stableType) < 1
    || Buffer.byteLength(stableType) > 255
    || stableType.includes('\0')
  ) {
    throw new framework.ZLinkConfigurationException(
      `${label} must contain 1..255 UTF-8 bytes and no NUL.`
    );
  }
  return stableType;
}

function requireFactoryConfigure(
  configure: unknown
): asserts configure is (builder: unknown) => void {
  if (typeof configure !== 'function') {
    throw new framework.ZLinkConfigurationException(
      'Object factory requires a configure callback.'
    );
  }
}

function validateUserSpotFactoryConfiguration(
  options: ZLinkUserSpotFactoryConfiguration
): void {
  validateStableTypeLimit(options.stableTypeLimit);
  const executionMode: unknown = options.executionMode;
  const relocationReadiness: unknown = options.relocationReadiness;
  if (
    executionMode !== ZLinkUserSpotExecutionMode.SpotWide
    && executionMode !== ZLinkUserSpotExecutionMode.PerActor
  ) {
    throw new framework.ZLinkConfigurationException(
      'User Spot executionMode is invalid.'
    );
  }
  if (
    relocationReadiness !== ZLinkSpotRelocationReadinessMode.AnyTurnBoundary
    && relocationReadiness !== ZLinkSpotRelocationReadinessMode.ApplicationSignaled
  ) {
    throw new framework.ZLinkConfigurationException(
      'User Spot relocationReadiness is invalid.'
    );
  }
  if (
    options.executionMode === ZLinkUserSpotExecutionMode.PerActor
    && options.relocationReadiness === ZLinkSpotRelocationReadinessMode.ApplicationSignaled
  ) {
    throw new framework.ZLinkConfigurationException(
      'ApplicationSignaled relocation readiness is valid only for SpotWide User Spots.'
    );
  }
}

function validateStableTypeLimit(value: number | undefined): void {
  if (
    value !== undefined
    && (!Number.isInteger(value) || value < 1 || value > 2_147_483_647)
  ) {
    throw new framework.ZLinkConfigurationException(
      'stableTypeLimit must be an integer from 1 through 2147483647.'
    );
  }
}

function requirePositiveCapacity(value: number, label: string): number {
  if (!Number.isSafeInteger(value) || value <= 0 || value > 0x7fff_ffff) {
    throw new framework.ZLinkConfigurationException(
      `${label} must be an integer in 1..2147483647.`
    );
  }
  return value;
}

function requireNonNegativeSafeInteger(value: number, label: string): number {
  if (!Number.isSafeInteger(value) || value < 0) {
    throw new framework.ZLinkConfigurationException(
      `${label} must be a non-negative safe integer.`
    );
  }
  return value;
}

function rejectDuplicateObjectType(
  registrations: Readonly<Record<string, unknown>>,
  stableType: string,
  meshName: string
): void {
  if (Object.hasOwn(registrations, stableType)) {
    throw new framework.ZLinkConfigurationException(
      `Duplicate object factory '${stableType}' on RouteMesh '${meshName}'.`
    );
  }
}

class DefaultZLinkNestMeshChannelBuilder extends ZLinkNestOptionsBuilder implements ZLinkNestMeshChannelBuilder {
  constructor(
    state: ZLinkNestBuilderState,
    private readonly channel: Mutable<NonNullable<ZLinkSpotNodeOptions['meshChannels']>[string]>
  ) {
    super(state);
  }

  client(): ZLinkNestMeshChannelClientBuilder {
    if (this.channel.client === true || this.channel.server === true) {
      throw new framework.ZLinkConfigurationException(
        'RouteMesh channel must register exactly one role; client() and server() cannot both be used or called twice.'
      );
    }
    this.channel.client = true;
    return new DefaultZLinkNestMeshChannelClientBuilder(this.state);
  }

  server(): ZLinkNestMeshChannelServerBuilder {
    if (this.channel.client === true || this.channel.server === true) {
      throw new framework.ZLinkConfigurationException(
        'RouteMesh channel must register exactly one role; client() and server() cannot both be used or called twice.'
      );
    }
    this.channel.server = true;
    return new DefaultZLinkNestMeshChannelServerBuilder(this.state, this.channel);
  }
}

class DefaultZLinkNestMeshChannelClientBuilder extends ZLinkNestOptionsBuilder
  implements ZLinkNestMeshChannelClientBuilder {}

class DefaultZLinkNestMeshChannelServerBuilder extends ZLinkNestOptionsBuilder
  implements ZLinkNestMeshChannelServerBuilder {
  constructor(
    state: ZLinkNestBuilderState,
    private readonly channel: Mutable<NonNullable<ZLinkSpotNodeOptions['meshChannels']>[string]>
  ) {
    super(state);
  }

  setWeight(weight: number): this {
    this.channel.weight = requirePublicWeight(weight, 'Mesh channel weight');
    return this;
  }

  addSendHandler(packetName: string, handlerType: Type): this {
    this.channel.sendHandlers = [...(this.channel.sendHandlers ?? []), { packetName, handlerType }];
    return this;
  }

  addRequestHandler(packetName: string, handlerType: Type): this {
    this.channel.requestHandlers = [...(this.channel.requestHandlers ?? []), { packetName, handlerType }];
    return this;
  }

  addHandlerGroup(groupName: string): this {
    this.channel.handlerGroups = [...(this.channel.handlerGroups ?? []), groupName];
    return this;
  }
}

class DefaultZLinkNestMeshPeerConnections implements ZLinkMeshPeerConnections {
  constructor(private readonly router: Mutable<NonNullable<ZLinkSpotNodeOptions['router']>>) {}

  connect(endpoint: string): void;
  connect(expectedRoutingId: string, endpoint: string): void;
  connect(expectedRoutingIdOrEndpoint: string, endpoint?: string): void {
    if (endpoint === undefined) {
      this.router.manualConnections = [...(this.router.manualConnections ?? []), expectedRoutingIdOrEndpoint];
      return;
    }
    this.router.manualPeerConnections = [
      ...(this.router.manualPeerConnections ?? []),
      { peerRid: expectedRoutingIdOrEndpoint, endpoint }
    ];
  }

  disconnect(endpoint: string): void {
    this.router.manualConnections = (this.router.manualConnections ?? [])
      .filter((value) => value !== endpoint);
    this.router.manualPeerConnections = (this.router.manualPeerConnections ?? [])
      .filter((value) => value.endpoint !== endpoint);
  }

  listConnections(): readonly ZLinkMeshPeerConnection[] {
    return [
      ...(this.router.manualConnections ?? []).map((endpoint) => ({ endpoint })),
      ...(this.router.manualPeerConnections ?? []).map((connection) => ({
        endpoint: connection.endpoint,
        expectedRoutingId: connection.peerRid
      }))
    ];
  }
}

function endpointList(endpoint: string | readonly string[]): string[] {
  return typeof endpoint === 'string' ? [endpoint] : [...endpoint];
}

export function createZLinkNestFrameworkOptionsBuilder(): ZLinkNestFrameworkOptionsBuilder {
  return new DefaultZLinkNestFrameworkOptionsBuilder();
}
