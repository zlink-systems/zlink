export {
  ZLinkConfigurationException,
  createFrameworkRegistration,
  hasActorManager,
  hasSpotNode,
  hasSpotPublisherClient
} from './contracts/Configuration/Registration';
export type * from './contracts/Configuration/RegistrationTypes';
export type * from './contracts';
export {
  registerActorFactory,
  validateRoutingIdPrefix,
  registerEntrySpot,
  registerSpotFactory,
  validateActorTransferTimeout,
  validateMessageFollowDuration
} from './contracts/Configuration/RegistrationBuilderPolicy';

import type {
  ZLinkLocationRuntimeQuery,
  ZLinkActorClient,
  ZLinkActorManager,
  ZLinkChannelClient,
  ZLinkClientServerRuntime,
  ZLinkCodecRegistryBuilder,
  ZLinkCodecRegistrar,
  ZLinkDispatchOptions,
  ZLinkDispatchOptionsBuilder,
  ZLinkFanoutClient,
  ZLinkFanoutRuntime,
  ZLinkRouteClient,
  ZLinkRouteMeshRuntime,
  ZLinkRouteMeshRuntimeOptions,
  ZLinkSpotManager,
  ZLinkSpotOutbound,
  ZLinkSpotPublisherClient,
  ZLinkStreamCompressionBuilder,
  ZLinkStreamCompressionCodec
} from './contracts';
import { ZLinkApplicationHwmProfile } from './contracts/Configuration/InboundDispatch';
import type { ZLinkProviderResolver } from './contracts/Common/ZLinkProviderResolver';
import type { ZLinkSpotHandleResolver } from './runtime/spots/spot-handle';
import type { ZLinkBoundSessionFactory } from './runtime/streams/session-context';
import type { ZLinkFrameworkRegistration } from './contracts/Configuration/Registration';
import type {
  ZLinkCodecSerializerRegistration,
  ZLinkStreamCodecRegistration
} from './contracts/Configuration/RegistrationTypes';
import type {
  ZLinkInboundDispatchOptionValues,
  ZLinkInboundDispatchOptions
} from './contracts/Configuration/InboundDispatch';
import {
  DefaultDispatchOptionsBuilder,
  DefaultInboundDispatchOptionsBuilder,
  DefaultLocationOptionsBuilder,
  DefaultStreamCompressionBuilder
} from './contracts/Configuration/RegistrationBuilders';
import { RegistrationCodecRegistryBuilder } from './contracts/Configuration/RegistrationCodecRegistry';
import {
  DefaultZLinkChannelClient,
  DefaultZLinkFanoutClient,
  DefaultZLinkRouteClient,
  DefaultZLinkSpotPublisherClient
} from './runtime/channels';
import { DefaultZLinkActorClient, DefaultZLinkActorManager } from './runtime/actors';
import { ZLinkFrameworkRuntimeHost } from './runtime/host';
import {
  DefaultZLinkSpotManager,
  DefaultZLinkSpotOutbound,
  ZLinkSpotSerialExecutor
} from './runtime/spots';
import { ZLinkWorkerRuntime } from './runtime/workers';
import { captureZLinkExecutionTurn } from './runtime/execution';
export {
  registerHandlerFilterScope as registerIntegrationHandlerFilterScope,
  type ZLinkHandlerFilterScopeRunner
} from './runtime/channels/handler-filter-scope';

export interface ZLinkNestIntegrationRuntimeHost {
  readonly channelRuntimeOptions: unknown;
  readonly routeMeshRuntimeOptions: ZLinkRouteMeshRuntimeOptions;
  readonly boundSessionFactory: ZLinkBoundSessionFactory;
  readonly locationRuntimeQuery?: ZLinkLocationRuntimeQuery;
  readonly routeMeshRuntime: ZLinkRouteMeshRuntime;
  readonly clientServerRuntime: ZLinkClientServerRuntime;
  readonly fanoutRuntime: ZLinkFanoutRuntime;
  createLocationHandleResolver(): ZLinkSpotHandleResolver | undefined;
  start(): Promise<void>;
  stop(): Promise<void>;
  onApplicationBootstrap?(): Promise<void> | void;
  onApplicationShutdown?(): Promise<void> | void;
}

export function createIntegrationDispatchOptionsBuilder(
  dispatch: ZLinkDispatchOptions
): ZLinkDispatchOptionsBuilder {
  return new DefaultDispatchOptionsBuilder(dispatch);
}

export function createIntegrationInboundDispatchOptionsBuilder(
  options: Partial<ZLinkInboundDispatchOptionValues>
): ZLinkInboundDispatchOptions {
  // Keep the caller-owned options object as the builder state.  The Nest
  // adapter stores this same object in its registration state; copying it
  // here would make fluent HWM changes disappear from build().
  options.applicationHwmProfile ??= ZLinkApplicationHwmProfile.Balanced;
  return new DefaultInboundDispatchOptionsBuilder(
    options as ZLinkInboundDispatchOptionValues
  );
}

export function createIntegrationLocationOptionsBuilder(
  options: Partial<import('./contracts').ZLinkLocationOptionValues>
): import('./contracts').ZLinkLocationOptions {
  return new DefaultLocationOptionsBuilder(options);
}

export function createIntegrationStreamCompressionBuilder(
  options: { disabled?: boolean; codec?: ZLinkStreamCompressionCodec }
): ZLinkStreamCompressionBuilder {
  return new DefaultStreamCompressionBuilder(options);
}

export function createIntegrationCodecRegistryBuilder(
  options: {
    serializers: ZLinkCodecSerializerRegistration[];
    streamCodecs: ZLinkStreamCodecRegistration[];
  }
): ZLinkCodecRegistryBuilder & ZLinkCodecRegistrar {
  return new RegistrationCodecRegistryBuilder(options);
}

export function createIntegrationRuntimeHost(
  registration: ZLinkFrameworkRegistration,
  providerResolver?: ZLinkProviderResolver
): ZLinkNestIntegrationRuntimeHost {
  return new ZLinkFrameworkRuntimeHost({ registration, providerResolver });
}

export function createIntegrationChannelClient(
  registration: ZLinkFrameworkRegistration,
  runtime: ZLinkNestIntegrationRuntimeHost
): ZLinkChannelClient {
  return new DefaultZLinkChannelClient(registration, runtimeHost(runtime).channelTransport);
}

export function createIntegrationFanoutClient(
  registration: ZLinkFrameworkRegistration,
  runtime: ZLinkNestIntegrationRuntimeHost
): ZLinkFanoutClient {
  return new DefaultZLinkFanoutClient(registration, runtimeHost(runtime).channelTransport);
}

export function createIntegrationRouteClient(
  registration: ZLinkFrameworkRegistration,
  runtime: ZLinkNestIntegrationRuntimeHost
): ZLinkRouteClient {
  const host = runtimeHost(runtime);
  return new DefaultZLinkRouteClient(
    registration,
    host.routeTransport,
    host.spotRouterChannelIdForMesh
  );
}

export function createIntegrationSpotPublisherClient(
  registration: ZLinkFrameworkRegistration,
  runtime: ZLinkNestIntegrationRuntimeHost
): ZLinkSpotPublisherClient {
  return new DefaultZLinkSpotPublisherClient(registration, runtimeHost(runtime).spotPublisherTransport);
}

export function createIntegrationActorClient(runtime: ZLinkNestIntegrationRuntimeHost): ZLinkActorClient {
  const host = runtimeHost(runtime);
  return new DefaultZLinkActorClient(host.createActorClientOptions());
}

export function createIntegrationActorManager(
  registration: ZLinkFrameworkRegistration,
  runtime: ZLinkNestIntegrationRuntimeHost,
  providerResolver?: ZLinkProviderResolver
): ZLinkActorManager {
  const host = runtimeHost(runtime);
  const runtimeOptions = host.createActorManagerOptions();
  const manager = new DefaultZLinkActorManager({
    actorFactories: registration.actorFactories,
    ...runtimeOptions,
    boundSessionFactory: runtimeOptions.boundSessionFactory ?? host.boundSessionFactory.create.bind(host.boundSessionFactory),
    providerResolver
  });
  host.setActorManager(manager);
  return manager;
}

export function createIntegrationSpotManager(
  registration: ZLinkFrameworkRegistration,
  runtime: ZLinkNestIntegrationRuntimeHost,
  providerResolver?: ZLinkProviderResolver
): ZLinkSpotManager {
  const host = runtimeHost(runtime);
  const runtimeOptions = host.createSpotManagerOptions();
  const manager = new DefaultZLinkSpotManager({
    spotFactories: [...registration.spotFactories],
    instanceSpotFactories: new Map(
      [...registration.spotNodes].map(([meshName, spotNode]) => [
        meshName,
        new Map(Object.entries(spotNode.instanceSpotFactories ?? {}))
      ])
    ),
    instanceSpotIdleTimeoutMs: new Map(
      [...registration.spotNodes].map(([meshName, spotNode]) => [
        meshName,
        spotNode.instanceSpotIdleTimeoutMs ?? 0
      ])
    ),
    spotTimerHandlers: [...registration.spotNodes.values()]
      .flatMap((spotNode) => [...(spotNode.spotTimerHandlers ?? [])]),
    spotPacketHandlers: [...registration.spotNodes.values()]
      .flatMap((spotNode) => [...(spotNode.spotPacketHandlers ?? [])]),
    spotSubscriptionHandlers: [...registration.spotNodes.values()]
      .flatMap((spotNode) => [...(spotNode.spotSubscriptionHandlers ?? [])]),
    spotActorSendHandlers: [...registration.spotNodes.values()]
      .flatMap((spotNode) => [...(spotNode.spotActorSendHandlers ?? [])]),
    spotActorRequestHandlers: [...registration.spotNodes.values()]
      .flatMap((spotNode) => [...(spotNode.spotActorRequestHandlers ?? [])]),
    ...runtimeOptions,
    spotRouteResolver: runtimeOptions.spotRouteResolver,
    routedTransport: host.routeTransport,
    providerResolver,
    workerRuntime: new ZLinkWorkerRuntime(registration.worker)
  });
  host.setSpotManager(manager);
  return host.createPublicSpotManager(manager);
}

export function createIntegrationSpotOutbound(runtime: ZLinkNestIntegrationRuntimeHost): ZLinkSpotOutbound {
  const host = runtimeHost(runtime);
  const runtimeOptions = host.createSpotManagerOptions();
  return new DefaultZLinkSpotOutbound(
    new ZLinkSpotSerialExecutor(),
    undefined,
    undefined,
    undefined,
    host.routeTransport,
    runtimeOptions.spotRouterChannelIdForMesh,
    undefined,
    undefined,
    undefined,
    host.spotAddressTransport
  );
}

export function createIntegrationHttpExecutionScheduler(runtime: ZLinkNestIntegrationRuntimeHost) {
  return {
    capture: captureZLinkExecutionTurn,
    reportError(error: unknown): void {
      const host = runtimeHost(runtime);
      host.errorSink?.reportRuntimeTaskException('http client submit', error);
    }
  };
}

function runtimeHost(runtime: ZLinkNestIntegrationRuntimeHost): ZLinkFrameworkRuntimeHost {
  return runtime as ZLinkFrameworkRuntimeHost;
}
