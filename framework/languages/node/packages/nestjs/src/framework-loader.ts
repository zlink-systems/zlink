import { createRequire } from 'node:module';
import path from 'node:path';
import type {
  Type,
  ZLinkActorClient,
  ZLinkActorManager,
  ZLinkChannelClient,
  ZLinkCodecRegistrar,
  ZLinkCodecRegistryBuilder,
  ZLinkDispatchOptions,
  ZLinkDispatchOptionsBuilder,
  ZLinkInboundDispatchOptions,
  ZLinkLocationOptionValues,
  ZLinkLocationOptions,
  ZLinkEntrySpot,
  ZLinkFanoutClient,
  ZLinkHandlerFilterContext,
  ZLinkRouteClient,
  ZLinkSpot,
  ZLinkSpotManager,
  ZLinkSpotOutbound,
  ZLinkSpotPublisherClient,
  ZLinkStreamCompressionBuilder,
  ZLinkStreamCompressionCodec
} from '@zlink-systems/framework';
import type { ZLinkHttpExecutionScheduler } from '@zlink-systems/http-client';
import type {
  ZLinkCodecSerializerRegistration,
  ZLinkFrameworkRegistration,
  ZLinkFrameworkRegistrationOptions,
  ZLinkNestIntegrationRuntimeHost,
  ZLinkProviderResolver,
  ZLinkStreamCodecRegistration
} from './framework-integration-contracts';

export type FrameworkRuntimeHost = ZLinkNestIntegrationRuntimeHost;

interface FrameworkIntegrationModule {
  ZLinkSpotActorSend(packetName?: string): MethodDecorator;
  ZLinkSpotActorRequest(packetName?: string): MethodDecorator;
  readonly MAX_LISTENER_PORT: number;
  readonly ZLINK_MAX_PUBLIC_WEIGHT: number;
  readonly ZLINK_MAX_CAPACITY: number;
  readonly ZLINK_MAX_STABLE_TYPE_BYTES: number;
  readonly ZLinkConfigurationException: new (message: string) => Error;
  createFrameworkRegistration(
    options: ZLinkFrameworkRegistrationOptions
  ): ZLinkFrameworkRegistration;
  normalizeFrameworkRegistration(
    options: ZLinkFrameworkRegistrationOptions
  ): ZLinkFrameworkRegistration;
  hasActorManager(registration: ZLinkFrameworkRegistration): boolean;
  hasSpotNode(registration: ZLinkFrameworkRegistration): boolean;
  hasSpotPublisherClient(registration: ZLinkFrameworkRegistration): boolean;
  createIntegrationDispatchOptionsBuilder(
    dispatch: ZLinkDispatchOptions
  ): ZLinkDispatchOptionsBuilder;
  createIntegrationInboundDispatchOptionsBuilder(
    options: Pick<ZLinkFrameworkRegistrationOptions, 'coreHwm' | 'applicationJobQueue'>
  ): ZLinkInboundDispatchOptions;
  createIntegrationLocationOptionsBuilder(
    options: Partial<ZLinkLocationOptionValues>
  ): ZLinkLocationOptions;
  createIntegrationStreamCompressionBuilder(options: {
    disabled?: boolean;
    codec?: ZLinkStreamCompressionCodec;
  }): ZLinkStreamCompressionBuilder;
  createIntegrationCodecRegistryBuilder(options: {
    serializers: ZLinkCodecSerializerRegistration[];
    streamCodecs: ZLinkStreamCodecRegistration[];
  }): ZLinkCodecRegistryBuilder & ZLinkCodecRegistrar;
  createIntegrationRuntimeHost(
    registration: ZLinkFrameworkRegistration,
    providerResolver?: ZLinkProviderResolver
  ): ZLinkNestIntegrationRuntimeHost;
  createIntegrationChannelClient(
    registration: ZLinkFrameworkRegistration,
    runtime: ZLinkNestIntegrationRuntimeHost
  ): ZLinkChannelClient;
  createIntegrationFanoutClient(
    registration: ZLinkFrameworkRegistration,
    runtime: ZLinkNestIntegrationRuntimeHost
  ): ZLinkFanoutClient;
  createIntegrationRouteClient(
    registration: ZLinkFrameworkRegistration,
    runtime: ZLinkNestIntegrationRuntimeHost
  ): ZLinkRouteClient;
  createIntegrationSpotPublisherClient(
    registration: ZLinkFrameworkRegistration,
    runtime: ZLinkNestIntegrationRuntimeHost
  ): ZLinkSpotPublisherClient;
  createIntegrationActorClient(runtime: ZLinkNestIntegrationRuntimeHost): ZLinkActorClient;
  createIntegrationActorManager(
    registration: ZLinkFrameworkRegistration,
    runtime: ZLinkNestIntegrationRuntimeHost,
    providerResolver?: ZLinkProviderResolver
  ): ZLinkActorManager;
  createIntegrationSpotManager(
    registration: ZLinkFrameworkRegistration,
    runtime: ZLinkNestIntegrationRuntimeHost,
    providerResolver?: ZLinkProviderResolver
  ): ZLinkSpotManager;
  createIntegrationSpotOutbound(runtime: ZLinkNestIntegrationRuntimeHost): ZLinkSpotOutbound;
  createIntegrationHttpExecutionScheduler(
    runtime: ZLinkNestIntegrationRuntimeHost
  ): ZLinkHttpExecutionScheduler;
  validateActorTransferTimeout(timeoutMs: number): number;
  validateMessageFollowDuration(timeoutMs: number): number;
  validateSessionReplacementCallbackTimeout(timeoutMs: number): number;
  validateRoutingIdPrefix(prefix: string): string;
  isValidPublicWeight(value: number): boolean;
  requirePublicWeight(value: number, label: string): number;
  isValidPositiveCapacity(value: number): boolean;
  isValidListenerPort(value: number): boolean;
  isValidCapacity(value: number): boolean;
  registerEntrySpot(
    options: { entrySpotType?: Type<ZLinkEntrySpot> },
    entrySpotType: Type<ZLinkEntrySpot>
  ): void;
  registerSpotFactory(
    options: { spotFactories?: Type<ZLinkSpot>[] },
    spotType: Type<ZLinkSpot>
  ): void;
  registerActorFactory(
    options: { actorFactories?: Record<string, Type> },
    actorType: string,
    factoryType: Type
  ): void;
  registerRelocationStore(
    options: { relocationStoreInstance?: import('@zlink-systems/framework').ZLinkRelocationStore },
    store: import('@zlink-systems/framework').ZLinkRelocationStore
  ): void;
  registerIntegrationHandlerFilterScope(
    resolver: ZLinkProviderResolver,
    runner: (
      context: ZLinkHandlerFilterContext,
      callback: (scope: { resolve<T>(type: Type<T>): Promise<T> }) => Promise<unknown>
    ) => Promise<unknown>
  ): void;
}

function loadFramework(): FrameworkIntegrationModule {
  const requireFramework = createRequire(__filename);
  const frameworkEntry = requireFramework.resolve('@zlink-systems/framework');
  const internal = requireFramework(path.join(path.dirname(frameworkEntry), 'internal'));
  return {
    ...requireFramework(path.join(path.dirname(frameworkEntry), 'nest-integration')),
    MAX_LISTENER_PORT: internal.MAX_LISTENER_PORT,
    ZLINK_MAX_PUBLIC_WEIGHT: internal.ZLINK_MAX_PUBLIC_WEIGHT,
    ZLINK_MAX_CAPACITY: internal.ZLINK_MAX_CAPACITY,
    ZLINK_MAX_STABLE_TYPE_BYTES: internal.ZLINK_MAX_STABLE_TYPE_BYTES,
    isValidPublicWeight: internal.isValidPublicWeight,
    isValidPositiveCapacity: internal.isValidPositiveCapacity,
    isValidListenerPort: internal.isValidListenerPort,
    isValidCapacity: internal.isValidCapacity,
    requirePublicWeight: internal.requirePublicWeight
  } as FrameworkIntegrationModule;
}

// The Nest package is a workspace adapter. It loads the framework's private
// composition bridge without turning that bridge into a package subpath.
export const framework = loadFramework();
