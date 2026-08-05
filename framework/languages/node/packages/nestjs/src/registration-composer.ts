import type { DiscoveryService, ModuleRef } from '@nestjs/core';
import type { Type } from '@zlink-systems/framework';
import type {
  ZLinkChannelOptions,
  ZLinkFrameworkRegistrationOptions,
  ZLinkMeshChannelOptions
} from './framework-integration-contracts';
import {
  ZLINK_MODULE_OPTIONS_BRAND,
  type ZLinkModuleOptions,
  type ZLinkNestModuleRegistrationOptions,
  type ZLinkNestTypeResolver
} from './contracts';
import {
  discoverProviderRefs,
  discoverSessionProviderRefs,
  discoverSpotActorProviderRefs,
  discoverSpotProviderRefs,
  discoverSpotTimerProviderRefs,
  type DiscoveredNestSpotActorProvider,
  type DiscoveredNestSpotProvider,
  type DiscoveredNestSpotTimerProvider
} from './provider-discovery';
import {
  createDiscoveredChannelSendHandlers,
  createDiscoveredPublishHandlers,
  createDiscoveredRequestHandlers,
  createDiscoveredSendHandlers,
  createManualPublishHandlers,
  createManualRequestHandlers,
  createManualSendHandlers
} from './handler-adapters';
import { framework } from './framework-loader';
import { SpotNodeHandlerRegistry } from './spot-node-handler-registry';


export function hasNestHandlerDiscovery(options: ZLinkNestModuleRegistrationOptions): boolean {
  return hasConfiguredSpotNodes(options.spotNodes)
    || Object.values(options.clientServerChannels ?? {}).some(
      (channel) => (channel.handlerGroups ?? []).length > 0
        || (channel.requestHandlerTypes ?? []).length > 0
        || (channel.sendHandlerTypes ?? []).length > 0
    )
    || Object.values(options.fanoutChannels ?? {}).some(
      (channel) => (channel.handlerGroups ?? []).length > 0
        || (channel.publishHandlerTypes ?? []).length > 0
    );
}

function hasConfiguredSpotNodes(value: ZLinkNestModuleRegistrationOptions['spotNodes']): boolean {
  if (value === undefined) {
    return false;
  }
  return Array.isArray(value) ? value.length > 0 : Object.keys(value).length > 0;
}


export function createDiscoveredOptions(
  options: ZLinkNestModuleRegistrationOptions,
  discovery: DiscoveryService,
  moduleRef: ModuleRef
): ZLinkFrameworkRegistrationOptions {
  const registrationOptions = createRegistrationOptions(options);
  const channels: Record<string, ZLinkChannelOptions> = { ...(registrationOptions.channels ?? {}) };
  const providerRefs = discoverProviderRefs(discovery, moduleRef);
  const spotActorProviderRefs = discoverSpotActorProviderRefs(discovery, moduleRef);
  const spotProviderRefs = discoverSpotProviderRefs(discovery, moduleRef);
  const spotTimerProviderRefs = discoverSpotTimerProviderRefs(discovery, moduleRef);
  const sessionProviderRefs = discoverSessionProviderRefs(discovery, moduleRef);
  const spotNodes = createDiscoveredMeshChannelOptions(createDiscoveredSpotNodeOptions(
    registrationOptions.spotNodes,
    spotActorProviderRefs,
    spotProviderRefs,
    spotTimerProviderRefs), providerRefs, moduleRef);

  for (const [channelName, channel] of Object.entries(options.fanoutChannels ?? {})) {
    const existingChannel = channels[channelName] as ZLinkChannelOptions | undefined;
    const publishHandlers = createDiscoveredPublishHandlers(providerRefs, channel.handlerGroups, moduleRef);
    const manualPublishHandlers = createManualPublishHandlers(channel.publishHandlerTypes, moduleRef);
    channels[channelName] = {
      ...existingChannel,
      publishHandlers: [
        ...(existingChannel?.publishHandlers ?? []),
        ...manualPublishHandlers,
        ...publishHandlers
      ]
    };
  }

  for (const [channelName, channel] of Object.entries(options.clientServerChannels ?? {})) {
    const existingChannel = channels[channelName];
    channels[channelName] = {
      ...existingChannel,
      requestHandlers: [
        ...(existingChannel.requestHandlers ?? []),
        ...createManualRequestHandlers(channel.requestHandlerTypes, moduleRef),
        ...createDiscoveredRequestHandlers(providerRefs, channel.handlerGroups, moduleRef)
      ],
      sendHandlers: [
        ...(existingChannel.sendHandlers ?? []),
        ...createManualSendHandlers(channel.sendHandlerTypes, moduleRef),
        ...createDiscoveredChannelSendHandlers(providerRefs, channel.handlerGroups, moduleRef)
      ]
    };
  }

  return {
    ...registrationOptions,
    channels,
    spotNodes,
    streamNodes: attachDiscoveredSessionHandlers(
      registrationOptions.streamNodes,
      sessionProviderRefs.map((ref) => ref.handlerKey)
    )
  };
}

const ZLINK_SESSION_HANDLER_TYPES = Symbol.for('@zlink-systems/framework:session-handler-types');

function attachDiscoveredSessionHandlers(
  streamNodes: ZLinkFrameworkRegistrationOptions['streamNodes'],
  handlerTypes: readonly Type[]
): ZLinkFrameworkRegistrationOptions['streamNodes'] {
  if (streamNodes === undefined || handlerTypes.length === 0) {
    return streamNodes;
  }
  return Object.fromEntries(Object.entries(streamNodes).map(([name, stream]) => {
    const configured = { ...stream } as typeof stream & Record<symbol, readonly Type[]>;
    configured[ZLINK_SESSION_HANDLER_TYPES] = handlerTypes;
    return [name, configured];
  }));
}

function createDiscoveredMeshChannelOptions(
  value: ZLinkFrameworkRegistrationOptions['spotNodes'],
  providerRefs: ReturnType<typeof discoverProviderRefs>,
  moduleRef: ModuleRef
): ZLinkFrameworkRegistrationOptions['spotNodes'] {
  if (value === undefined || Array.isArray(value)) {
    return value;
  }
  return Object.fromEntries(Object.entries(value).map(([meshName, mesh]) => [meshName, {
    ...mesh,
    meshChannels: mesh.meshChannels === undefined
      ? undefined
      : Object.fromEntries((Object.entries(mesh.meshChannels) as Array<[string, ZLinkMeshChannelOptions]>).map(([channelName, channel]) => [channelName, {
        ...channel,
        requestHandlers: [
          ...createManualRequestHandlers(channel.requestHandlers, moduleRef),
          ...createDiscoveredRequestHandlers(providerRefs, channel.handlerGroups, moduleRef) as never
        ],
        sendHandlers: [
          ...createManualSendHandlers(channel.sendHandlers, moduleRef),
          ...createDiscoveredSendHandlers(providerRefs, channel.handlerGroups, moduleRef) as never
        ]
      }]))
  }]));
}

function createDiscoveredSpotNodeOptions(
  value: ZLinkFrameworkRegistrationOptions['spotNodes'],
  refs: readonly DiscoveredNestSpotActorProvider[],
  spotRefs: readonly DiscoveredNestSpotProvider[] = [],
  timerRefs: readonly DiscoveredNestSpotTimerProvider[] = []
): ZLinkFrameworkRegistrationOptions['spotNodes'] {
  if (refs.length === 0 && spotRefs.length === 0 && timerRefs.length === 0) {
    return value;
  }
  const registry = SpotNodeHandlerRegistry.from(value);
  if (registry.isEmpty) {
    throw new framework.ZLinkConfigurationException('ZLink SPOT actor handlers require a registered SpotNode.');
  }

  addDiscoveredSpotTimers(registry, timerRefs);
  addDiscoveredSpotHandlers(registry, spotRefs);
  addDiscoveredSpotActorHandlers(registry, refs);
  return registry.toOptions();
}

function addDiscoveredSpotTimers(
  registry: SpotNodeHandlerRegistry,
  timerRefs: readonly DiscoveredNestSpotTimerProvider[]
): void {
  for (const ref of timerRefs) {
    if (ref.metadata.entrySpot !== undefined) {
      const entrySpotType = resolveNestType(ref.metadata.entrySpot, 'entrySpot');
      const targets = registry.entrySpotTargets(
        entrySpotType,
        `ZLink Entry Spot timer handler '${ref.handlerName}' targets an Entry Spot that is not registered on any SpotNode.`
      );
      for (const spotNode of targets) {
        registry.addEntrySpotTimer(spotNode, {
          entrySpotType,
          handlerType: ref.handlerKey,
          name: ref.metadata.name,
          options: ref.metadata.options,
          periodMs: ref.metadata.periodMs
        });
      }
      continue;
    }
    const spotType = resolveNestType(ref.metadata.spot, 'spot');
    const targets = registry.spotTargets(
      spotType,
      `ZLink SPOT timer handler '${ref.handlerName}' targets a Spot type that is not registered on any SpotNode.`
    );
    for (const spotNode of targets) {
      registry.addSpotTimer(spotNode, {
        handlerType: ref.handlerKey,
        name: ref.metadata.name,
        options: ref.metadata.options,
        periodMs: ref.metadata.periodMs,
        spotType
      });
    }
  }
}

function addDiscoveredSpotHandlers(
  registry: SpotNodeHandlerRegistry,
  spotRefs: readonly DiscoveredNestSpotProvider[]
): void {
  for (const ref of spotRefs) {
    if (ref.metadata.kind === 'entrySpotPacket' || ref.metadata.kind === 'entrySpotSubscription') {
      const entrySpotType = resolveNestType(ref.metadata.entrySpot, 'entrySpot');
      const targets = registry.entrySpotTargets(
        entrySpotType,
        `ZLink Entry Spot handler '${ref.handlerName}' targets an Entry Spot that is not registered on any SpotNode.`
      );
      for (const spotNode of targets) {
        if (ref.metadata.kind === 'entrySpotPacket') {
          registry.addEntrySpotPacket(spotNode, {
            entrySpotType,
            handlerType: ref.handlerKey,
            packetName: ref.metadata.packetName
          });
        } else {
          registry.addEntrySpotSubscription(spotNode, {
            entrySpotType,
            handlerType: ref.handlerKey,
            channelName: requireSpotSubscriptionChannelName(ref),
            topic: requireSpotSubscriptionTopic(ref)
          });
        }
      }
      continue;
    }

    const spotType = resolveNestType(ref.metadata.spot, 'spot');
    const targets = registry.spotTargets(
      spotType,
      `ZLink SPOT handler '${ref.handlerName}' targets a Spot type that is not registered on any SpotNode.`
    );
    for (const spotNode of targets) {
      if (ref.metadata.kind === 'spotPacket') {
        registry.addSpotPacket(spotNode, {
          handlerType: ref.handlerKey,
          packetName: ref.metadata.packetName,
          spotType: spotType as Type<import('@zlink-systems/framework').ZLinkSpot>
        });
      } else {
        registry.addSpotSubscription(spotNode, {
          channelName: requireSpotSubscriptionChannelName(ref),
          handlerType: ref.handlerKey,
          spotType: spotType as Type<import('@zlink-systems/framework').ZLinkSpot>,
          topic: requireSpotSubscriptionTopic(ref)
        });
      }
    }
  }
}

function addDiscoveredSpotActorHandlers(
  registry: SpotNodeHandlerRegistry,
  refs: readonly DiscoveredNestSpotActorProvider[]
): void {
  for (const ref of refs) {
    if (ref.metadata.kind === 'entrySpotActorSend' || ref.metadata.kind === 'entrySpotActorRequest') {
      const entrySpotType = resolveNestType(ref.metadata.entrySpot, 'entrySpot');
      const actorType = resolveNestType(ref.metadata.actor, 'actor');
      const targets = registry.entrySpotTargets(
        entrySpotType,
        `ZLink Entry Spot actor handler '${ref.handlerName}' targets an Entry Spot that is not registered on any SpotNode.`
      );
      for (const spotNode of targets) {
        const next = {
          actorType,
          entrySpotType,
          handlerType: ref.handlerKey,
          packetName: ref.metadata.packetName
        };
        registry.addEntrySpotActor(
          spotNode,
          ref.metadata.kind === 'entrySpotActorSend' ? 'send' : 'request',
          next
        );
      }
      continue;
    }

    const spotType = resolveNestType(ref.metadata.spot, 'spot');
    const actorType = resolveNestType(ref.metadata.actor, 'actor');
    const targets = registry.spotTargets(
      spotType,
      `ZLink SPOT actor handler '${ref.handlerName}' targets a Spot type that is not registered on any SpotNode.`
    );
    for (const spotNode of targets) {
      const next = {
        actorType,
        handlerType: ref.handlerKey,
        packetName: ref.metadata.packetName,
        spotType
      };
      registry.addSpotActor(
        spotNode,
        ref.metadata.kind === 'spotActorSend' ? 'send' : 'request',
        next
      );
    }
  }
}

function resolveNestType<T>(resolver: ZLinkNestTypeResolver<T> | undefined, name: string): Type<T> {
  if (resolver === undefined) {
    throw new framework.ZLinkConfigurationException(`ZLink SPOT actor handler ${name} type is required.`);
  }
  if (isClassType(resolver)) {
    return resolver;
  }
  const resolved = (resolver as () => Type<T>)();
  if (!isClassType(resolved)) {
    throw new framework.ZLinkConfigurationException(`ZLink SPOT actor handler ${name} type resolver must return a class.`);
  }
  return resolved;
}

function isClassType(value: unknown): value is Type {
  return typeof value === 'function' && /^class\s/.test(Function.prototype.toString.call(value));
}


function requireSpotSubscriptionTopic(ref: DiscoveredNestSpotProvider): string {
  const topic = ref.metadata.topic;
  if (topic === undefined || topic.trim().length === 0) {
    throw new framework.ZLinkConfigurationException(`ZLink SPOT subscription handler '${ref.handlerName}' requires a topic.`);
  }
  return topic;
}

function requireSpotSubscriptionChannelName(ref: DiscoveredNestSpotProvider): string {
  const channelName = ref.metadata.channelName;
  if (channelName === undefined || channelName.trim().length === 0) {
    throw new framework.ZLinkConfigurationException(
      `ZLink SPOT subscription handler '${ref.handlerName}' requires a channelName.`
    );
  }
  return channelName;
}

export function createRegistrationOptions(options: ZLinkNestModuleRegistrationOptions): ZLinkFrameworkRegistrationOptions {
  const channels: Record<string, ZLinkChannelOptions> = {};

  for (const [name, channel] of Object.entries(options.clientServerChannels ?? {})) {
    assertChannelNameAvailable(channels, name, 'ClientServerChannel');
    channels[name] = {
      client: channel.client,
      server: channel.server
    };
  }

  for (const [name, channel] of Object.entries(options.fanoutChannels ?? {})) {
    assertChannelNameAvailable(channels, name, 'FanoutChannel');
    channels[name] = {
      routingId: channel.routingId,
      routingIdPrefix: channel.routingIdPrefix,
      publishHandlers: channel.publishHandlers,
      publisher: channel.publisher,
      subscriber: channel.subscriber
    };
  }

  return {
    network: options.network,
    applicationVersion: options.applicationVersion,
    maintenanceWave: options.maintenanceWave,
    actorTransferTimeoutMs: options.actorTransferTimeoutMs,
    messageFollowDurationMs: options.messageFollowDurationMs,
    channels,
    codecs: options.codecs,
    dispatch: options.dispatch,
    filters: options.filters,
    inboundDispatch: options.inboundDispatch,
    locations: options.locations,
    metrics: options.metrics,
    requestTimeoutMs: options.requestTimeoutMs,
    spotFactories: options.spotFactories,
    spotNodes: options.spotNodes,
    spotPublisherClients: options.spotPublisherClients,
    streamCompression: options.streamCompression,
    streamNodes: options.streams,
    worker: options.worker
  };
}

function assertChannelNameAvailable(
  channels: Readonly<Record<string, ZLinkChannelOptions>>,
  name: string,
  kind: string
): void {
  if (Object.hasOwn(channels, name)) {
    throw new framework.ZLinkConfigurationException(`Channel '${name}' is already registered before ${kind}.`);
  }
}

export function assertBuiltModuleOptions(options: ZLinkModuleOptions): ZLinkNestModuleRegistrationOptions {
  if (!isBuiltModuleOptions(options)) {
    throw new framework.ZLinkConfigurationException('NestJS ZLinkModule options must be created with zlinkFramework().build().');
  }
  return options;
}

function isBuiltModuleOptions(options: unknown): options is ZLinkNestModuleRegistrationOptions {
  return typeof options === 'object'
    && options !== null
    && (options as { readonly [ZLINK_MODULE_OPTIONS_BRAND]?: unknown })[ZLINK_MODULE_OPTIONS_BRAND] === true;
}
