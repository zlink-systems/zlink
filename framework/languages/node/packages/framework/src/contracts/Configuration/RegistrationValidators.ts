import type { Type, ZLinkSpot } from '../../contracts';
import { ZLinkConfigurationException } from './ConfigurationException';
import { requirePositiveInteger } from './RegistrationNormalizers';
import type {
  ZLinkFrameworkRegistration,
  ZLinkFrameworkRegistrationOptions,
  ZLinkRouteChannelOptions,
  ZLinkRouteMeshChannelOptions,
  ZLinkMeshChannelOptions,
  ZLinkSpotNodeOptions,
  ZLinkSpotPubSubCapabilityOptions,
  ZLinkSpotRouterCapabilityOptions,
  ZLinkSpotRouterPeerConnectionOptions,
  ZLinkWorkerOptions
} from './RegistrationTypes';
import {
  isRouteClientEnabled,
  isRouteTransportDeclared
} from './RouteChannelInternalState';
import { validateTimerRegistration } from './TimerRegistrationValidator';
import { zlinkDefaultLocationOptions } from '../Locations';
import { requireValidSendTimeoutMs } from './SendTimeoutValidation';

export function validateFrameworkRegistration(
  registration: ZLinkFrameworkRegistration,
  _options: ZLinkFrameworkRegistrationOptions = {}
): void {
  validateListenerNetworkIdentity(
    'process network',
    undefined,
    registration.network.bindHost,
    registration.network.advertiseHost
  );
  const actorCapableSpotNodes = [...registration.spotNodes.values()]
    .filter((spotNode) => toActorFactoryCount(spotNode.actorFactories) > 0);
  if (actorCapableSpotNodes.length > 1) {
    throw new ZLinkConfigurationException(
      'Actor factory registration is ambiguous because more than one SpotNode owns actor factories.'
    );
  }

  const peerLocationConfigured = hasLocationStores(registration);
  validateChannelCapabilities(
    Object.fromEntries(registration.channels),
    peerLocationConfigured
  );
  validateChannelTopologyNames(registration);
  validateSpotNodes(registration);
  validateRouteChannels(registration, peerLocationConfigured);
  validateStreamNodes(registration);
  validateWorkerOptions(registration.worker);
  validateInboundDispatchListenerBounds(registration);
  validateLocationRegistration(registration);
}

function validateInboundDispatchListenerBounds(
  registration: ZLinkFrameworkRegistration
): void {
  if (registration.inboundDispatch.applicationHwmBytes === 0n) {
    return;
  }

  const listeners: Array<readonly [string, number | undefined]> = [];
  for (const [channelName, channel] of registration.channels) {
    listeners.push(
      [`channel '${channelName}' server`, channel.server?.maxMessageSize],
      [`channel '${channelName}' subscriber`, channel.subscriber?.maxMessageSize],
      [`channel '${channelName}' route mesh`, channel.routeMesh?.maxMessageSize]
    );
  }
  for (const [channelName, routeChannel] of registration.routeChannelOptions) {
    if (hasBind(routeChannel.bind)) {
      listeners.push([`route channel '${channelName}'`, routeChannel.maxMessageSize]);
    }
  }
  for (const [spotNodeName, spotNode] of registration.spotNodes) {
    listeners.push([`SpotNode '${spotNodeName}' router`, spotNode.router?.maxMessageSize]);
  }
  for (const [streamNodeName, streamNode] of registration.streamNodes) {
    listeners.push([
      `STREAM node '${streamNodeName}'`,
      streamNode.maxMessageSize
    ]);
  }

  const unbounded = listeners.find(([, maxMessageSize]) => maxMessageSize === 0);
  if (unbounded !== undefined) {
    throw new ZLinkConfigurationException(
      `${unbounded[0]} maxMessageSize must be bounded when Application HWM is enabled.`
    );
  }
}

function validateChannelTopologyNames(registration: ZLinkFrameworkRegistration): void {
  const declaredChannelNames = new Set(
    [...registration.channels]
      .filter(([, channel]) => channel.client !== undefined
        || channel.server !== undefined
        || channel.publisher !== undefined
        || channel.subscriber !== undefined)
      .map(([channelName]) => channelName)
  );
  if (declaredChannelNames.size === 0) return;

  const routeMeshChannels = new Set(registration.routeChannels);
  for (const spotNode of registration.spotNodes.values()) {
    for (const channelName of Object.keys(spotNode.meshChannels ?? {})) {
      routeMeshChannels.add(channelName);
    }
  }
  for (const channelName of declaredChannelNames) {
    if (routeMeshChannels.has(channelName)) {
      throw new ZLinkConfigurationException(
        `ChannelName '${channelName}' is registered on both RouteMesh and ClientServer physical paths.`
      );
    }
  }
}

function toActorFactoryCount(value: ZLinkSpotNodeOptions['actorFactories']): number {
  if (value === undefined) {
    return 0;
  }
  return value instanceof Map ? value.size : Object.keys(value).length;
}

function hasLocationStores(registration: ZLinkFrameworkRegistration): boolean {
  return registration.locations.useInMemoryStores
    || registration.locations.storeInstance !== undefined;
}

function validateWorkerOptions(worker: ZLinkWorkerOptions | undefined): void {
  if (worker === undefined) {
    return;
  }
  requireNonNegativeInteger('Worker minThreads', worker.minThreads);
  requirePositiveInteger('Worker maxThreads', worker.maxThreads);
  requireNonNegativeInteger('Worker idleTimeoutMs', worker.idleTimeoutMs);
  requirePositiveInteger('Worker maxQueueLength', worker.maxQueueLength);
  if (worker.maxThreads < worker.minThreads) {
    throw new ZLinkConfigurationException(
      'Worker maxThreads must be greater than or equal to minThreads.'
    );
  }
}

function validateLocationRegistration(registration: ZLinkFrameworkRegistration): void {
  const locations = registration.locations;
  if (locations.useInMemoryStores && locations.storeInstance !== undefined) {
    throw new ZLinkConfigurationException(
      'In-memory location stores cannot be combined with explicit location store registrations.'
    );
  }
  const store = locations.storeInstance;
  if (store !== undefined
    && (typeof store.read !== 'function'
      || typeof store.write !== 'function'
      || typeof store.scan !== 'function')) {
    throw new ZLinkConfigurationException(
      'Location Store must implement read, write, and scan.'
    );
  }
  const options = { ...zlinkDefaultLocationOptions, ...locations.options };
  for (const [name, value] of Object.entries({
    ownerLeaseRenewIntervalMs: options.ownerLeaseRenewIntervalMs,
    ownerLeaseTtlMs: options.ownerLeaseTtlMs,
    ownerLeaseFencingMarginMs: options.ownerLeaseFencingMarginMs,
    ownerLeaseRenewTimeoutMs: options.ownerLeaseRenewTimeoutMs
  })) {
    if (!Number.isFinite(value) || value <= 0) {
      throw new ZLinkConfigurationException(`${name} must be a finite positive number.`);
    }
  }
  if (
    options.ownerLeaseRenewIntervalMs + options.ownerLeaseRenewTimeoutMs
    >= options.ownerLeaseTtlMs - options.ownerLeaseFencingMarginMs
  ) {
    throw new ZLinkConfigurationException(
      'ownerLeaseRenewIntervalMs + ownerLeaseRenewTimeoutMs must be less than ownerLeaseTtlMs - ownerLeaseFencingMarginMs.'
    );
  }
  for (const [name, value] of Object.entries({
    routeCacheMaxAgeMs: options.routeCacheMaxAgeMs,
    messageFollowDurationMs: options.messageFollowDurationMs
  })) {
    if (!Number.isFinite(value) || value < 0) {
      throw new ZLinkConfigurationException(`${name} must be a finite non-negative number.`);
    }
  }
  if (
    options.routeCacheMaxAgeMs > 0
    && options.messageFollowDurationMs > 0
    && options.routeCacheMaxAgeMs > options.messageFollowDurationMs - 5000
  ) {
    throw new ZLinkConfigurationException(
      'routeCacheMaxAgeMs must be at least 5000 ms shorter than messageFollowDurationMs.'
    );
  }
  for (const [name, value] of Object.entries({
    maxActiveOutboundRelocations: options.maxActiveOutboundRelocations,
    maxActiveInboundRelocations: options.maxActiveInboundRelocations,
    maxConcurrentRelocationCaptures: options.maxConcurrentRelocationCaptures,
    maxConcurrentRelocationRestores: options.maxConcurrentRelocationRestores,
    maxRelocationPayloadInFlightBytes: options.maxRelocationPayloadInFlightBytes
  })) {
    if (!Number.isSafeInteger(value) || value <= 0) {
      throw new ZLinkConfigurationException(`${name} must be a positive safe integer.`);
    }
  }
}

function requireNonNegativeInteger(label: string, value: number | undefined): void {
  if (value === undefined) {
    return;
  }
  if (!Number.isInteger(value) || value < 0) {
    throw new ZLinkConfigurationException(`${label} must be a non-negative integer.`);
  }
}

function requireNonNegativeSafeInteger(label: string, value: number | undefined): void {
  if (value === undefined) return;
  if (!Number.isSafeInteger(value) || value < 0) {
    throw new ZLinkConfigurationException(`${label} must be a non-negative safe integer.`);
  }
}

function validateChannelCapabilities(
  channels: ZLinkFrameworkRegistrationOptions['channels'],
  peerLocationConfigured: boolean
): void {
  for (const [channelName, channel] of Object.entries(channels ?? {})) {
    if (channel.server !== undefined) {
      requireEndpoint(`channel '${channelName}' server`, channel.server.bind);
      validateListenerNetworkIdentity(
        `channel '${channelName}' server`,
        channel.server.bind,
        channel.server.bindHost,
        channel.server.advertiseHost
      );
      if (channel.server.routingId !== undefined) {
        requireName(`channel '${channelName}' server routingId`, channel.server.routingId);
      }
      requirePeerWeight(`channel '${channelName}' server weight`, channel.server.weight);
      requireSocketOptions(`channel '${channelName}' server`, channel.server);
    }
    if (channel.publisher !== undefined) {
      requireEndpoint(`channel '${channelName}' publisher`, channel.publisher.bind);
      validateListenerNetworkIdentity(
        `channel '${channelName}' publisher`,
        channel.publisher.bind,
        channel.publisher.bindHost,
        channel.publisher.advertiseHost
      );
      validateFanoutPublisherIdentity(
        channelName,
        channel.routingId,
        channel.routingIdPrefix,
        peerLocationConfigured
      );
    }
    if (channel.client !== undefined) {
      requirePeerSource(
        `channel '${channelName}' client`,
        channel.client.manualConnections,
        peerLocationConfigured || channel.server !== undefined
      );
      requireSocketOptions(`channel '${channelName}' client`, channel.client);
    }
    if (channel.subscriber !== undefined) {
      requirePeerSource(`channel '${channelName}' subscriber`, channel.subscriber.manualConnections, peerLocationConfigured);
    }
    if ((channel.publishHandlers ?? []).length > 0 && channel.subscriber === undefined) {
      throw new ZLinkConfigurationException(
        `Channel '${channelName}' publish handlers require a subscriber capability.`
      );
    }
    if (channel.subscriber !== undefined && (channel.publishHandlers ?? []).length === 0) {
      throw new ZLinkConfigurationException(
        `Channel '${channelName}' subscriber must register at least one publish handler.`
      );
    }
    if (((channel.requestHandlers ?? []).length > 0 || (channel.sendHandlers ?? []).length > 0) && channel.server === undefined) {
      throw new ZLinkConfigurationException(
        `Channel '${channelName}' request/send handlers require a server capability.`
      );
    }
    if (
      channel.server !== undefined
      && (channel.requestHandlers ?? []).length + (channel.sendHandlers ?? []).length === 0
    ) {
      throw new ZLinkConfigurationException(
        `Channel '${channelName}' server must register at least one request or send handler.`
      );
    }
    validateDuplicatePacketNames(
      `channel '${channelName}' request handler`,
      channel.requestHandlers?.map((handler) => handler.packetName)
    );
    validateDuplicatePacketNames(
      `channel '${channelName}' send handler`,
      channel.sendHandlers?.map((handler) => handler.packetName)
    );
    validateDuplicatePacketNames(
      `channel '${channelName}' publish handler`,
      channel.publishHandlers?.map((handler) => handler.packetName)
    );
    if (channel.routeMesh !== undefined) {
      validateRouteMeshCapability(
        `channel '${channelName}' route mesh`,
        channel.routeMesh,
        peerLocationConfigured);
    }
  }
}

function validateSpotNodes(registration: ZLinkFrameworkRegistration): void {
  for (const [spotNodeName, spotNode] of registration.spotNodes.entries()) {
    if (spotNodeName.trim().length === 0 || spotNodeName.trim() !== spotNodeName) {
      throw new ZLinkConfigurationException('SpotNode name must not be empty or padded.');
    }
    if (spotNode.router === undefined && spotNode.pubSub === undefined) {
      throw new ZLinkConfigurationException(
        `SpotNode '${spotNodeName}' must enable router or pubSub capability.`
      );
    }
    if (toActorFactoryCount(spotNode.actorFactories) > 0 && spotNode.router === undefined) {
      throw new ZLinkConfigurationException(
        `SpotNode '${spotNodeName}' must enable router capability when actor factories are registered.`
      );
    }
    validateSpotNodeCapability(`SpotNode '${spotNodeName}' router`, spotNode.router);
    validateMeshChannels(spotNodeName, spotNode.meshChannels);
    if (spotNode.router !== undefined) {
      validateListenerNetworkIdentity(
        `SpotNode '${spotNodeName}' router`,
        spotNode.router.bind,
        spotNode.router.bindHost,
        spotNode.router.advertiseHost
      );
    }
    validateSpotNodeCapability(`SpotNode '${spotNodeName}' pubSub`, spotNode.pubSub);
    requireNonNegativeSafeInteger(
      `SpotNode '${spotNodeName}' instanceSpotIdleTimeoutMs`,
      spotNode.instanceSpotIdleTimeoutMs
    );
    requireValidSendTimeoutMs(
      `SpotNode '${spotNodeName}' publisher sendTimeoutMs`,
      spotNode.publisherConfig?.sendTimeoutMs
    );
    validateSpotNodeFactories(spotNodeName, spotNode);
    validateSpotNodeTimers(spotNode);
    if (
      spotNode.objectRole === 'client'
      && (
        (spotNode.routeSendHandlers?.length ?? 0) > 0
        || (spotNode.routeRequestHandlers?.length ?? 0) > 0
      )
    ) {
      throw new ZLinkConfigurationException(
        `Object Client RouteMesh '${spotNodeName}' cannot register Node-direct handlers.`
      );
    }
    if (
      spotNode.router?.routingId !== undefined &&
      spotNode.pubSub?.routingId !== undefined &&
      spotNode.router.routingId !== spotNode.pubSub.routingId
    ) {
      throw new ZLinkConfigurationException(
        `SpotNode '${spotNodeName}' router and pubSub routingId must match.`
      );
    }
  }
}

function validateMeshChannels(
  spotNodeName: string,
  meshChannels: Readonly<Record<string, ZLinkMeshChannelOptions>> | undefined
): void {
  for (const [channelName, channel] of Object.entries(meshChannels ?? {})) {
    requireName(`RouteMesh '${spotNodeName}' channel`, channelName);
    const handlerCount = (channel.requestHandlers?.length ?? 0)
      + (channel.sendHandlers?.length ?? 0);
    if (channel.client === true && channel.server === true) {
      throw new ZLinkConfigurationException(
        `RouteMesh '${spotNodeName}' channel '${channelName}' must register exactly one role.`
      );
    }
    if (channel.server !== true && handlerCount > 0) {
      throw new ZLinkConfigurationException(
        `RouteMesh '${spotNodeName}' channel '${channelName}' handlers require a server role.`
      );
    }
    if (channel.client !== true && channel.server !== true) {
      throw new ZLinkConfigurationException(
        `RouteMesh '${spotNodeName}' channel '${channelName}' must register a client or server role.`
      );
    }
    validateDuplicatePacketNames(
      `RouteMesh '${spotNodeName}' channel '${channelName}' request handler`,
      channel.requestHandlers?.map((handler) => handler.packetName)
    );
    validateDuplicatePacketNames(
      `RouteMesh '${spotNodeName}' channel '${channelName}' send handler`,
      channel.sendHandlers?.map((handler) => handler.packetName)
    );
  }
}

function validateSpotNodeTimers(spotNode: ZLinkSpotNodeOptions): void {
  for (const timer of spotNode.entrySpotTimerHandlers ?? []) {
    validateTimerRegistration(timer.name, timer.periodMs, timer.options);
  }
  for (const timer of spotNode.spotTimerHandlers ?? []) {
    validateTimerRegistration(timer.name, timer.periodMs, timer.options);
  }
}

function validateSpotNodeFactories(spotNodeName: string, spotNode: ZLinkSpotNodeOptions): void {
  const seen = new Set<Type<ZLinkSpot>>();
  for (const factory of spotNode.spotFactories ?? []) {
    if (seen.has(factory)) {
      throw new ZLinkConfigurationException(
        `Duplicate SPOT factory registration on SpotNode '${spotNodeName}'.`
      );
    }
    seen.add(factory);
  }
}

function validateSpotNodeCapability(
  capabilityName: string,
  capability: ZLinkSpotRouterCapabilityOptions | ZLinkSpotPubSubCapabilityOptions | undefined
): void {
  if (capability === undefined) {
    return;
  }
  validateManualConnections(capabilityName, capability.manualConnections);
  validateRouterPeerConnections(capabilityName, 'manualPeerConnections' in capability ? capability.manualPeerConnections : undefined);
  requireEndpoint(capabilityName, capability.bind);
  if (capability.routingId !== undefined && (capability.routingId.trim().length === 0 || capability.routingId.trim() !== capability.routingId)) {
    throw new ZLinkConfigurationException(`${capabilityName} routingId must not be empty or padded.`);
  }
}

function validateListenerNetworkIdentity(
  capabilityName: string,
  bindEndpoint: string | undefined,
  configuredBindHost: string | undefined,
  advertiseHost: string | undefined
): void {
  const bindHost = configuredBindHost ?? tcpEndpointHost(bindEndpoint);
  if (configuredBindHost !== undefined) {
    requireName(`${capabilityName} bind host`, configuredBindHost);
  }
  if (advertiseHost !== undefined) {
    requireName(`${capabilityName} advertise host`, advertiseHost);
    if (isWildcardHost(advertiseHost)) {
      throw new ZLinkConfigurationException(
        `${capabilityName} advertise host must identify a connectable host, not a wildcard address.`
      );
    }
  }
  if (bindHost !== undefined && isWildcardHost(bindHost) && advertiseHost === undefined) {
    throw new ZLinkConfigurationException(
      `${capabilityName} must define an advertise host when its bind host is a wildcard address.`
    );
  }
}

function tcpEndpointHost(endpoint: string | undefined): string | undefined {
  const match = /^tcp:\/\/(\[[^\]]+\]|[^:]+):\d+$/.exec(endpoint ?? '');
  return match?.[1];
}

function isWildcardHost(host: string): boolean {
  const normalized = host.startsWith('[') && host.endsWith(']')
    ? host.slice(1, -1)
    : host;
  return normalized === '0.0.0.0' || normalized === '::';
}

function validateManualConnections(capabilityName: string, manualConnections: readonly string[] | undefined): void {
  if ((manualConnections ?? []).some((endpoint) => endpoint.trim().length === 0)) {
    throw new ZLinkConfigurationException(`${capabilityName} manual connection endpoint must not be empty.`);
  }
}

function validateRouterPeerConnections(
  capabilityName: string,
  manualPeerConnections: readonly ZLinkSpotRouterPeerConnectionOptions[] | undefined
): void {
  const seenPeerRids = new Set<string>();
  for (const connection of manualPeerConnections ?? []) {
    const peerRid = String(connection.peerRid);
    if (peerRid.trim().length === 0) {
      throw new ZLinkConfigurationException(`${capabilityName} manual peer routing id must not be empty.`);
    }
    if (seenPeerRids.has(peerRid)) {
      throw new ZLinkConfigurationException(`${capabilityName} manual peer routing id must be unique.`);
    }
    seenPeerRids.add(peerRid);
    if (connection.endpoint.trim().length === 0) {
      throw new ZLinkConfigurationException(`${capabilityName} manual peer connection endpoint must not be empty.`);
    }
  }
}

function validateDuplicatePacketNames(label: string, packetNames: readonly string[] | undefined): void {
  const seen = new Set<string>();
  for (const packetName of packetNames ?? []) {
    requireName(label, packetName);
    if (seen.has(packetName)) {
      throw new ZLinkConfigurationException(`Duplicate ${label} packet '${packetName}'.`);
    }
    seen.add(packetName);
  }
}

function requireName(label: string, value: string): void {
  if (value.trim().length === 0 || value.trim() !== value) {
    throw new ZLinkConfigurationException(`${label} must not be empty or padded.`);
  }
}

function requirePeerSource(
  capabilityName: string,
  manualConnections: readonly string[] | undefined,
  peerLocationConfigured: boolean
): void {
  validateManualConnections(capabilityName, manualConnections);
  if ((manualConnections ?? []).length > 0 || peerLocationConfigured) {
    return;
  }
  throw new ZLinkConfigurationException(`${capabilityName} requires location stores or manual connections.`);
}

function validateFanoutPublisherIdentity(
  channelName: string,
  routingId: string | undefined,
  routingIdPrefix: string | undefined,
  peerLocationConfigured: boolean
): void {
  if (!peerLocationConfigured) return;
  if (routingId !== undefined && routingIdPrefix !== undefined) {
    throw new ZLinkConfigurationException(
      `channel '${channelName}' publisher cannot combine a fixed routing id with an automatic routing id prefix.`
    );
  }
  if (routingId === undefined && routingIdPrefix === undefined) {
    throw new ZLinkConfigurationException(
      `channel '${channelName}' publisher must select a fixed routing id or an automatic routing id prefix.`
    );
  }
  if (routingId !== undefined) requireName(`channel '${channelName}' publisher routingId`, routingId);
}

function requireEndpoint(capabilityName: string, endpoint: string | undefined): void {
  if (endpoint === undefined || endpoint.trim().length === 0) {
    throw new ZLinkConfigurationException(`${capabilityName} must define a bind endpoint.`);
  }
}

function requireFilePath(label: string, value: string | undefined): void {
  if (value === undefined || value.trim().length === 0) {
    throw new ZLinkConfigurationException(`${label} must define a file path.`);
  }
}

function validateStreamNodes(registration: ZLinkFrameworkRegistration): void {
  for (const [streamNodeName, streamNode] of registration.streamNodes.entries()) {
    requireNonNegativeInteger(
      `STREAM node '${streamNodeName}' maxMessageSize`,
      streamNode.maxMessageSize
    );
    requireEndpoint(`STREAM node '${streamNodeName}'`, streamNode.bind);
    validateListenerNetworkIdentity(
      `STREAM node '${streamNodeName}'`,
      streamNode.bind,
      streamNode.bindHost,
      streamNode.advertiseHost
    );
    if (streamNode.tlsServer !== undefined) {
      requireFilePath(`STREAM node '${streamNodeName}' TLS certificate`, streamNode.tlsServer.certificatePath);
      requireFilePath(`STREAM node '${streamNodeName}' TLS key`, streamNode.tlsServer.keyPath);
    }
    if (streamNode.session === undefined) {
      throw new ZLinkConfigurationException(
        `STREAM node '${streamNodeName}' must register a header stream session.`
      );
    }
    if (streamNode.actorDispatchEnabled === true) {
      if (!hasLocationStores(registration)) {
        throw new ZLinkConfigurationException(
          `STREAM node '${streamNodeName}' enables Actor dispatch but no Location Store is registered.`
        );
      }
      if (![...registration.spotNodes.values()].some((spotNode) => spotNode.objectRole !== undefined)) {
        throw new ZLinkConfigurationException(
          `STREAM node '${streamNodeName}' requires at least one local Object Client or Server MeshNode.`
        );
      }
    }
  }
}

function validateRouteChannels(registration: ZLinkFrameworkRegistration, peerLocationConfigured: boolean): void {
  for (const routeChannel of registration.routeChannelOptions.values()) {
    if (!isRouteTransportDeclared(routeChannel) && !isRouteTransportConfigured(routeChannel)) {
      continue;
    }
    if (
      !isRoutePacketCapabilityConfigured(routeChannel) &&
      isAcceptedSpotRouteChannel(registration, routeChannel)
    ) {
      continue;
    }
    validateRouteMeshCapability(`route channel '${routeChannel.routerChannelId}'`, routeChannel, peerLocationConfigured);
    if (routeChannelHandlerCount(routeChannel) > 0 && !hasBind(routeChannel.bind)) {
      requireEndpoint(`route channel '${routeChannel.routerChannelId}' router`, routeChannel.bind);
    }
  }
}

function isRouteTransportConfigured(routeChannel: ZLinkRouteChannelOptions): boolean {
  return isRoutePacketCapabilityConfigured(routeChannel);
}

function isRoutePacketCapabilityConfigured(routeChannel: ZLinkRouteChannelOptions): boolean {
  return hasBind(routeChannel.bind)
    || isRouteClientEnabled(routeChannel)
    || (routeChannel.manualConnections ?? []).length > 0
    || routeChannelHandlerCount(routeChannel) > 0;
}

function isAcceptedSpotRouteChannel(
  registration: ZLinkFrameworkRegistration,
  routeChannel: ZLinkRouteChannelOptions
): boolean {
  if (!isRouteTransportDeclared(routeChannel)) {
    return false;
  }
  if (registration.spotNodes.get(routeChannel.routerChannelId)?.router !== undefined) {
    return true;
  }
  if (routeChannel.routingId !== undefined) {
    return [...registration.spotNodes.values()].some((spotNode) =>
      spotNode.router?.routingId === routeChannel.routingId);
  }
  return [...registration.spotNodes.values()].filter((spotNode) => spotNode.router !== undefined).length === 1;
}

function validateRouteMeshCapability(
  capabilityName: string,
  routeChannel: ZLinkRouteChannelOptions | ZLinkRouteMeshChannelOptions,
  peerLocationConfigured = false
): void {
  const clientEnabled = isRouteClientEnabled(routeChannel)
    || (routeChannel.manualConnections ?? []).length > 0;
  if (!hasBind(routeChannel.bind) && !clientEnabled) {
    throw new ZLinkConfigurationException(`${capabilityName} must enable server or client capability.`);
  }
  if (clientEnabled) {
    requirePeerSource(capabilityName, routeChannel.manualConnections, peerLocationConfigured);
  }
  requirePeerWeight(`${capabilityName} weight`, routeChannel.weight);
  requireSocketOptions(capabilityName, routeChannel);
}

function routeChannelHandlerCount(routeChannel: ZLinkRouteChannelOptions): number {
  return (routeChannel.handlers ?? []).length +
    (routeChannel.sendHandlers ?? []).length +
    (routeChannel.requestHandlers ?? []).length;
}

function hasBind(endpoint: string | undefined): boolean {
  return endpoint !== undefined && endpoint.trim().length > 0;
}

function requirePeerWeight(label: string, value: number | undefined): void {
  if (value === undefined) {
    return;
  }
  if (!Number.isInteger(value) || value < 0 || value > 10_000) {
    throw new ZLinkConfigurationException(`${label} must be an integer in 0..10000.`);
  }
}

function requireSocketOptions(
  label: string,
  options: {
    readonly sendHighWaterMark?: number;
    readonly receiveHighWaterMark?: number;
    readonly sendTimeoutMs?: number;
    readonly maxMessageSize?: number;
  }
): void {
  requireNonNegativeInteger(`${label} sendHighWaterMark`, options.sendHighWaterMark);
  requireNonNegativeInteger(`${label} receiveHighWaterMark`, options.receiveHighWaterMark);
  requireNonNegativeInteger(`${label} maxMessageSize`, options.maxMessageSize);
  requireValidSendTimeoutMs(`${label} sendTimeoutMs`, options.sendTimeoutMs);
}
