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
  ZLinkLocationOptionValues,
  ZLinkLocationOptions,
  ZLinkEntrySpot,
  ZLinkFanoutClient,
  ZLinkHandlerFilterContext,
  ZLinkInboundDispatchOptions,
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
  ZLinkInboundDispatchOptionValues,
  ZLinkNestIntegrationRuntimeHost,
  ZLinkProviderResolver,
  ZLinkStreamCodecRegistration
} from './framework-integration-contracts';

export type FrameworkRuntimeHost = ZLinkNestIntegrationRuntimeHost;

interface FrameworkIntegrationModule {
  readonly ZLinkConfigurationException: new (message: string) => Error;
  createFrameworkRegistration(options: ZLinkFrameworkRegistrationOptions): ZLinkFrameworkRegistration;
  hasActorManager(registration: ZLinkFrameworkRegistration): boolean;
  hasSpotNode(registration: ZLinkFrameworkRegistration): boolean;
  hasSpotPublisherClient(registration: ZLinkFrameworkRegistration): boolean;
  createIntegrationDispatchOptionsBuilder(dispatch: ZLinkDispatchOptions): ZLinkDispatchOptionsBuilder;
  createIntegrationInboundDispatchOptionsBuilder(
    options: Partial<ZLinkInboundDispatchOptionValues>
  ): ZLinkInboundDispatchOptions;
  createIntegrationLocationOptionsBuilder(
    options: Partial<ZLinkLocationOptionValues>
  ): ZLinkLocationOptions;
  createIntegrationStreamCompressionBuilder(
    options: { disabled?: boolean; codec?: ZLinkStreamCompressionCodec }
  ): ZLinkStreamCompressionBuilder;
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
  createIntegrationHttpExecutionScheduler(runtime: ZLinkNestIntegrationRuntimeHost): ZLinkHttpExecutionScheduler;
  validateActorTransferTimeout(timeoutMs: number): number;
  validateMessageFollowDuration(timeoutMs: number): number;
  validateRoutingIdPrefix(prefix: string): string;
  registerEntrySpot(
    options: { entrySpotType?: Type<ZLinkEntrySpot> },
    entrySpotType: Type<ZLinkEntrySpot>
  ): void;
  registerSpotFactory(options: { spotFactories?: Type<ZLinkSpot>[] }, spotType: Type<ZLinkSpot>): void;
  registerActorFactory(options: { actorFactories?: Record<string, Type> }, actorType: string, factoryType: Type): void;
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
  return requireFramework(
    path.join(path.dirname(frameworkEntry), 'nest-integration')
  ) as FrameworkIntegrationModule;
}

// The Nest package is a workspace adapter. It loads the framework's private
// composition bridge without turning that bridge into a package subpath.
export const framework = loadFramework();
