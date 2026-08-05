import type {
  Type,
  ZLinkSpot,
  ZLinkStreamCompressionOptions,
  ZLinkStreamCompressionCodec
} from '../../contracts';
import { ZLinkConfigurationException } from './ConfigurationException';
import {
  copyRouteInternalState,
  markRouteTransportDeclared
} from './RouteChannelInternalState';
import type {
  ZLinkChannelOptions,
  ZLinkFrameworkRegistration,
  ZLinkFrameworkRegistrationOptions,
  ZLinkLocationRegistration,
  ZLinkRouteChannelOptions,
  ZLinkSpotNodeOptions,
  ZLinkStreamNodeOptions,
  ZLinkWorkerOptions
} from './RegistrationTypes';
import type { ZLinkNetworkOptions } from './Builders';
import {
  defaultWorkerMaxThreads,
  DEFAULT_STREAM_NODE_MAX_MESSAGE_SIZE,
  DEFAULT_WORKER_IDLE_TIMEOUT_MS,
  DEFAULT_WORKER_MIN_THREADS,
  DEFAULT_WORKER_QUEUE_LENGTH
} from './InternalDefaults';

export function normalizeWorkerOptions(
  value: ZLinkFrameworkRegistrationOptions['worker']
): ZLinkWorkerOptions | undefined {
  if (value === undefined) return undefined;
  const partial = value as Partial<ZLinkWorkerOptions>;
  return {
    minThreads: partial.minThreads ?? DEFAULT_WORKER_MIN_THREADS,
    maxThreads: partial.maxThreads ?? defaultWorkerMaxThreads(),
    idleTimeoutMs: partial.idleTimeoutMs ?? DEFAULT_WORKER_IDLE_TIMEOUT_MS,
    maxQueueLength: partial.maxQueueLength ?? DEFAULT_WORKER_QUEUE_LENGTH
  };
}

export function normalizeNetworkOptions(
  value: ZLinkFrameworkRegistrationOptions['network']
): ZLinkNetworkOptions {
  return {
    bindHost: value?.bindHost ?? '127.0.0.1',
    ...(value?.advertiseHost === undefined ? {} : { advertiseHost: value.advertiseHost })
  };
}

export function toChannelMap(
  channels: ZLinkFrameworkRegistrationOptions['channels'],
  network: ZLinkNetworkOptions
): Map<string, ZLinkChannelOptions> {
  return new Map(Object.entries(channels ?? {}).map(([name, channel]) => {
    const server = channel.server === undefined
      ? undefined
      : normalizeListener(channel.server, network);
    const publisher = channel.publisher === undefined
      ? undefined
      : normalizeListener(channel.publisher, network);
    return [name, {
      ...channel,
      ...(server === undefined ? {} : { server }),
      ...(publisher === undefined ? {} : { publisher }),
      requestTimeoutMs: normalizeOptionalPositiveInteger(channel.requestTimeoutMs, `${name}.requestTimeoutMs`)
    }];
  }));
}

export function toStreamNodeMap(
  streamNodes: ZLinkFrameworkRegistrationOptions['streamNodes'],
  network: ZLinkNetworkOptions
): Map<string, ZLinkStreamNodeOptions> {
  return new Map(Object.entries(streamNodes ?? {}).map(([name, streamNode]) => {
    const normalized = normalizeListener(streamNode, network);
    return [name, {
      ...normalized,
      maxMessageSize: streamNode.maxMessageSize ?? DEFAULT_STREAM_NODE_MAX_MESSAGE_SIZE
    }];
  }));
}

export function toRouteChannelOptions(
  options: ZLinkFrameworkRegistrationOptions
): Map<string, ZLinkRouteChannelOptions> {
  const routeOptions = new Map<string, ZLinkRouteChannelOptions>();
  for (const routeChannel of options.routeChannels ?? []) {
    if (typeof routeChannel === 'string') {
      routeOptions.set(routeChannel, { routerChannelId: routeChannel });
      continue;
    }
    const normalized = {
      ...routeChannel,
      requestTimeoutMs: normalizeOptionalPositiveInteger(
        routeChannel.requestTimeoutMs,
        `${routeChannel.routerChannelId}.requestTimeoutMs`
      )
    } as ZLinkRouteChannelOptions;
    copyRouteInternalState(routeChannel, normalized);
    routeOptions.set(routeChannel.routerChannelId, normalized);
  }
  for (const [channelName, channel] of Object.entries(options.channels ?? {})) {
    if (channel.routeMesh === undefined) {
      continue;
    }
    if (routeOptions.has(channelName)) {
      throw new ZLinkConfigurationException(`Route mesh channel '${channelName}' is already registered.`);
    }
    const routeChannel = {
      routerChannelId: channelName,
      ...channel.routeMesh,
      requestTimeoutMs: normalizeOptionalPositiveInteger(
        channel.routeMesh.requestTimeoutMs,
        `${channelName}.routeMesh.requestTimeoutMs`
      )
    } as ZLinkRouteChannelOptions;
    markRouteTransportDeclared(routeChannel);
    copyRouteInternalState(channel.routeMesh, routeChannel);
    routeOptions.set(channelName, routeChannel);
  }
  return routeOptions;
}

export function requirePositiveInteger(label: string, value: number | undefined): void {
  if (value === undefined) {
    return;
  }
  if (!Number.isInteger(value) || value <= 0) {
    throw new ZLinkConfigurationException(`${label} must be a positive integer.`);
  }
}

export function normalizeOptionalPositiveInteger(value: number | undefined, label: string): number | undefined {
  requirePositiveInteger(label, value);
  return value;
}

export function hasSpotNode(registration: ZLinkFrameworkRegistration): boolean {
  return registration.spotNodes.size > 0;
}

export function hasActorManager(registration: ZLinkFrameworkRegistration): boolean {
  const hasObjectCapability = [...registration.spotNodes.values()].some(node =>
    node.objectRole === 'client'
    || node.objectRole === 'server'
    || (
      node.actorFactories instanceof Map
        ? node.actorFactories.size > 0
        : Object.keys(node.actorFactories ?? {}).length > 0
    )
    || Object.keys(node.actorFactoryRegistrations ?? {}).length > 0
  );
  return hasObjectCapability && (
    registration.locations.useInMemoryStores
    || registration.locations.storeInstance !== undefined
  );
}

export function hasSpotPublisherClient(registration: ZLinkFrameworkRegistration): boolean {
  return registration.spotPublisherClients.size > 0;
}

export function toTypeMap(value: ZLinkSpotNodeOptions['actorFactories']): Map<string, Type> {
  if (value === undefined) {
    return new Map();
  }
  if (value instanceof Map) {
    return new Map(value);
  }
  return new Map(Object.entries(value));
}

export function typeMapToRecord(value: ZLinkSpotNodeOptions['actorFactories']): Record<string, Type> {
  if (value === undefined) {
    return {};
  }
  if (isTypeMap(value)) {
    return Object.fromEntries(value) as Record<string, Type>;
  }
  return { ...(value as Readonly<Record<string, Type>>) };
}

function isTypeMap(value: ZLinkSpotNodeOptions['actorFactories']): value is Map<string, Type> {
  return value instanceof Map;
}

export function actorFactoriesFromSpotNodes(spotNodes: ReadonlyMap<string, ZLinkSpotNodeOptions>): Map<string, Type> {
  const actorCapableNodes = [...spotNodes.values()].filter((spotNode) => toTypeMap(spotNode.actorFactories).size > 0);
  if (actorCapableNodes.length === 0) {
    return new Map();
  }
  return toTypeMap(actorCapableNodes[0].actorFactories);
}

export function toSpotNodeMap(
  value: ZLinkFrameworkRegistrationOptions['spotNodes'],
  network: ZLinkNetworkOptions
): Map<string, ZLinkSpotNodeOptions> {
  if (value === undefined) {
    return new Map();
  }
  if (!Array.isArray(value)) {
    return new Map(Object.entries(value).map(([name, spotNode]) => [
      name,
      normalizeSpotNodeOptions(spotNode, network)
    ]));
  }
  return new Map(value.map((spotNode) => {
    if (typeof spotNode === 'string') {
      return [spotNode, normalizeSpotNodeOptions({}, network)];
    }
    const { name, ...options } = spotNode;
    return [name, normalizeSpotNodeOptions(options, network)];
  }));
}

function normalizeSpotNodeOptions(
  spotNode: ZLinkSpotNodeOptions,
  network: ZLinkNetworkOptions
): ZLinkSpotNodeOptions {
  const router = spotNode.router === undefined
    ? undefined
    : normalizeListener(spotNode.router, network);
  return {
    ...spotNode,
    router: router === undefined
      ? undefined
      : {
          ...router,
          routingId: router.routingId ?? spotNode.routingId
        },
    pubSub: spotNode.pubSub === undefined
      ? undefined
      : { ...spotNode.pubSub, routingId: spotNode.pubSub.routingId ?? spotNode.routingId }
  };
}

function normalizeListener<
  T extends {
    readonly bind?: string;
    readonly bindHost?: string;
    readonly advertiseHost?: string;
    readonly port?: number;
  }
>(listener: T, network: ZLinkNetworkOptions): T & {
  readonly bind: string;
  readonly bindHost?: string;
  readonly port: number;
} {
  const usesConfiguredEndpoint = listener.bind !== undefined && listener.port === undefined;
  const port = listener.port ?? (usesConfiguredEndpoint ? endpointPort(listener.bind!) : 0);
  const bindHost = listener.bindHost
    ?? (usesConfiguredEndpoint ? endpointHost(listener.bind!) : network.bindHost);
  const bind = usesConfiguredEndpoint ? listener.bind! : tcpEndpoint(bindHost ?? network.bindHost, port);
  return {
    ...listener,
    bind,
    bindHost,
    port,
    ...(listener.advertiseHost !== undefined
      ? { advertiseHost: listener.advertiseHost }
      : network.advertiseHost === undefined
        ? {}
        : { advertiseHost: network.advertiseHost })
  };
}

function tcpEndpoint(host: string, port: number): string {
  const endpointHost = host.includes(':') && !host.startsWith('[') ? `[${host}]` : host;
  return `tcp://${endpointHost}:${port}`;
}

function endpointHost(endpoint: string): string | undefined {
  try {
    const parsed = new URL(endpoint);
    if (parsed.protocol !== 'tcp:') return undefined;
    return parsed.hostname.replace(/^\[|\]$/g, '');
  } catch {
    return undefined;
  }
}

function endpointPort(endpoint: string): number {
  try {
    const parsed = new URL(endpoint);
    if (parsed.port.length === 0) return 0;
    return Number(parsed.port);
  } catch {
    return 0;
  }
}

export function toSpotFactorySet(
  rootFactories: readonly Type<ZLinkSpot>[] | undefined,
  spotNodes: ReadonlyMap<string, ZLinkSpotNodeOptions>
): Set<Type<ZLinkSpot>> {
  const factories = new Set(rootFactories ?? []);
  for (const spotNode of spotNodes.values()) {
    for (const spotFactory of spotNode.spotFactories ?? []) {
      factories.add(spotFactory);
    }
  }
  return factories;
}

export function toSpotPublisherClientSet(
  explicitClients: readonly string[] | undefined,
  spotNodes: ReadonlyMap<string, ZLinkSpotNodeOptions>
): Set<string> {
  const clients = new Set(explicitClients ?? []);
  for (const spotNodeName of spotNodes.keys()) {
    clients.add(spotNodeName);
  }
  return clients;
}

export function channelNamesWith(
  channels: ZLinkFrameworkRegistrationOptions['channels'],
  predicate: (channel: ZLinkChannelOptions) => boolean
): Set<string> {
  const names = new Set<string>();
  for (const [name, channel] of Object.entries(channels ?? {})) {
    if (predicate(channel)) {
      names.add(name);
    }
  }
  return names;
}

export function normalizeStreamCompression(
  value: ZLinkFrameworkRegistrationOptions['streamCompression']
): ZLinkStreamCompressionOptions | undefined {
  if (value === undefined) {
    return undefined;
  }
  if (value.disabled === true && value.codec !== undefined) {
    throw new ZLinkConfigurationException('STREAM compression codec cannot be set when compression is disabled.');
  }
  if (value.codec !== undefined && !isStreamCompressionCodec(value.codec)) {
    throw new ZLinkConfigurationException('STREAM compression codec must provide compress and decompress functions.');
  }
  return {
    disabled: value.disabled,
    codec: value.codec
  };
}

export function isStreamCompressionCodec(value: unknown): value is ZLinkStreamCompressionCodec {
  return typeof value === 'object'
    && value !== null
    && typeof (value as { compress?: unknown }).compress === 'function'
    && typeof (value as { decompress?: unknown }).decompress === 'function';
}

export function normalizeLocationRegistration(
  value: ZLinkFrameworkRegistrationOptions['locations']
): ZLinkLocationRegistration {
  return {
    useInMemoryStores: value?.useInMemoryStores === true,
    storeInstance: value?.storeInstance,
    relocationStoreInstance: value?.relocationStoreInstance,
    options: value?.options === undefined ? {} : { ...value.options }
  };
}
