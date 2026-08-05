import type { InjectionToken, OnModuleDestroy, OnModuleInit, Provider } from '@nestjs/common';
import {
  OPTIONAL_DEPS_METADATA,
  OPTIONAL_PROPERTY_DEPS_METADATA,
  PARAMTYPES_METADATA,
  PROPERTY_DEPS_METADATA,
  SELF_DECLARED_DEPS_METADATA
} from '@nestjs/common/constants';
import { ContextIdFactory, DiscoveryService, ModuleRef } from '@nestjs/core';
import type { Type } from '@zlink-systems/framework';
import type {
  ZLinkFrameworkRegistration,
  ZLinkProviderResolver
} from './framework-integration-contracts';
import {
  ZLINK_ACTOR_CLIENT,
  ZLINK_ACTOR_MANAGER,
  ZLINK_BOUND_SESSION_FACTORY,
  ZLINK_CHANNEL_CLIENT,
  ZLINK_CHANNEL_RUNTIME_OPTIONS,
  ZLINK_CLIENT_SERVER_RUNTIME,
  ZLINK_FANOUT_CLIENT,
  ZLINK_FANOUT_RUNTIME,
  ZLINK_FRAMEWORK_REGISTRATION,
  ZLINK_FRAMEWORK_RUNTIME,
  ZLINK_LOCATION_RUNTIME_QUERY,
  ZLINK_MESSAGE_METADATA_POLICY,
  ZLINK_ROUTE_CLIENT,
  ZLINK_ROUTE_MESH_RUNTIME_OPTIONS,
  ZLINK_ROUTE_MESH_RUNTIME,
  ZLINK_SPOT_MANAGER,
  ZLINK_SPOT_OUTBOUND,
  ZLINK_SPOT_PUBLISHER_CLIENT,
} from './tokens';
import {
  ZLINK_ACTOR_SPOT_HANDLE_RESOLVER,
  ZLINK_SPOT_HANDLE_RESOLVER
} from './internal-tokens';
import { framework, type FrameworkRuntimeHost } from './framework-loader';
import {
  currentNestDispatchContext,
  runInNestDispatchScope
} from './dispatch-scope';

type RuntimeHostWithNestLifecycle = FrameworkRuntimeHost & OnModuleInit & OnModuleDestroy;

interface AlwaysAvailableClientProviderSpec {
  readonly token: InjectionToken;
  create(registration: ZLinkFrameworkRegistration, runtime: FrameworkRuntimeHost): unknown;
}

const ALWAYS_AVAILABLE_CLIENT_PROVIDER_SPECS: readonly AlwaysAvailableClientProviderSpec[] = [
  {
    token: ZLINK_CHANNEL_CLIENT,
    create: (registration, runtime) => framework.createIntegrationChannelClient(registration, runtime)
  },
  {
    token: ZLINK_CHANNEL_RUNTIME_OPTIONS,
    create: (_registration, runtime) => runtime.channelRuntimeOptions
  },
  {
    token: ZLINK_FANOUT_CLIENT,
    create: (registration, runtime) => framework.createIntegrationFanoutClient(registration, runtime)
  },
  {
    token: ZLINK_ROUTE_CLIENT,
    create: (registration, runtime) => framework.createIntegrationRouteClient(registration, runtime)
  },
  {
    token: ZLINK_BOUND_SESSION_FACTORY,
    create: (_registration, runtime) => runtime.boundSessionFactory
  }
];

export function alwaysAvailableClientProviders(registration?: ZLinkFrameworkRegistration): Provider[] {
  return [
    ...ALWAYS_AVAILABLE_CLIENT_PROVIDER_SPECS.map((spec) =>
      createAlwaysAvailableClientProvider(spec, registration)
    ),
    { provide: ZLINK_MESSAGE_METADATA_POLICY, useValue: Object.freeze({ forward: true }) }
  ];
}

function createAlwaysAvailableClientProvider(
  spec: AlwaysAvailableClientProviderSpec,
  registration: ZLinkFrameworkRegistration | undefined
): Provider {
  if (registration !== undefined) {
    return {
      provide: spec.token,
      inject: [ZLINK_FRAMEWORK_RUNTIME],
      useFactory: (runtime: FrameworkRuntimeHost) => spec.create(registration, runtime)
    };
  }
  return {
    provide: spec.token,
    inject: [ZLINK_FRAMEWORK_REGISTRATION, ZLINK_FRAMEWORK_RUNTIME],
    useFactory: (resolved: ZLinkFrameworkRegistration, runtime: FrameworkRuntimeHost) =>
      spec.create(resolved, runtime)
  };
}

export function alwaysAvailableClientTokens(): InjectionToken[] {
  return [
    ZLINK_CHANNEL_CLIENT,
    ZLINK_ROUTE_CLIENT,
    ZLINK_FANOUT_CLIENT,
    ZLINK_BOUND_SESSION_FACTORY,
    ZLINK_MESSAGE_METADATA_POLICY,
  ];
}

export function conditionalClientProviders(registration: ZLinkFrameworkRegistration): Provider[] {
  return CONDITIONAL_CLIENT_PROVIDER_SPECS
    .filter((spec) => spec.isEnabled(registration))
    .map((spec) => createConditionalClientProvider(spec, registration));
}

interface ConditionalClientProviderSpec {
  readonly token: InjectionToken;
  readonly requiresRuntime: boolean;
  isEnabled(registration: ZLinkFrameworkRegistration): boolean;
  create(
    registration: ZLinkFrameworkRegistration,
    runtime: FrameworkRuntimeHost | undefined,
    moduleRef: ModuleRef | undefined,
    discovery: DiscoveryService | undefined
  ): unknown | Promise<unknown>;
}

const CONDITIONAL_CLIENT_PROVIDER_SPECS: readonly ConditionalClientProviderSpec[] = [
  {
    token: ZLINK_CLIENT_SERVER_RUNTIME,
    requiresRuntime: true,
    isEnabled: (registration) => [...registration.channels.values()]
      .some(channel => channel.client !== undefined || channel.server !== undefined),
    create: (_registration, runtime) => requireRuntime(runtime).clientServerRuntime
  },
  {
    token: ZLINK_FANOUT_RUNTIME,
    requiresRuntime: true,
    isEnabled: (registration) => [...registration.channels.values()]
      .some(channel => channel.subscriber !== undefined
        && (channel.subscriber.manualConnections?.length ?? 0) === 0),
    create: (_registration, runtime) => requireRuntime(runtime).fanoutRuntime
  },
  {
    token: ZLINK_ROUTE_MESH_RUNTIME_OPTIONS,
    requiresRuntime: true,
    isEnabled: (registration) => registration.spotNodes.size > 0,
    create: (_registration, runtime) => requireRuntime(runtime).routeMeshRuntimeOptions
  },
  {
    token: ZLINK_ROUTE_MESH_RUNTIME,
    requiresRuntime: true,
    isEnabled: (registration) => registration.spotNodes.size > 0,
    create: (_registration, runtime) => requireRuntime(runtime).routeMeshRuntime
  },
  {
    token: ZLINK_SPOT_MANAGER,
    requiresRuntime: true,
    isEnabled: (registration) => framework.hasSpotNode(registration) && hasLocationStores(registration),
    create: (registration, runtime, moduleRef, discovery) =>
      createSpotManager(registration, requireRuntime(runtime), moduleRef, discovery)
  },
  {
    token: ZLINK_SPOT_OUTBOUND,
    requiresRuntime: true,
    isEnabled: (registration) => framework.hasSpotNode(registration),
    create: (registration, runtime, moduleRef, discovery) =>
      createSpotOutbound(registration, requireRuntime(runtime), moduleRef, discovery)
  },
  {
    token: ZLINK_SPOT_PUBLISHER_CLIENT,
    requiresRuntime: true,
    isEnabled: (registration) => framework.hasSpotPublisherClient(registration),
    create: (registration, runtime) =>
      framework.createIntegrationSpotPublisherClient(registration, requireRuntime(runtime))
  },
  {
    token: ZLINK_ACTOR_CLIENT,
    requiresRuntime: true,
    isEnabled: (registration) => framework.hasSpotNode(registration) && hasLocationStores(registration),
    create: (_registration, runtime) =>
      framework.createIntegrationActorClient(requireRuntime(runtime))
  },
  {
    token: ZLINK_ACTOR_MANAGER,
    requiresRuntime: true,
    isEnabled: (registration) => framework.hasActorManager(registration),
    create: async (registration, runtime, moduleRef, discovery) => {
      const host = requireRuntime(runtime);
      return framework.createIntegrationActorManager(
        registration,
        host,
        moduleRef === undefined ? undefined : createProviderResolver(moduleRef, discovery)
      );
    }
  },
  {
    token: ZLINK_LOCATION_RUNTIME_QUERY,
    requiresRuntime: true,
    isEnabled: (registration) => hasLocationStores(registration),
    create: (_registration, runtime) => {
      const query = requireRuntime(runtime).locationRuntimeQuery;
      if (query === undefined) {
        throw new framework.ZLinkConfigurationException('Location runtime query requires location stores.');
      }
      return query;
    }
  },
  {
    token: ZLINK_SPOT_HANDLE_RESOLVER,
    requiresRuntime: true,
    isEnabled: (registration) => hasLocationStores(registration),
    create: (_registration, runtime) => {
      const resolver = requireRuntime(runtime).createLocationHandleResolver();
      if (resolver === undefined) {
        throw new framework.ZLinkConfigurationException('SpotHandle resolver requires location stores.');
      }
      return resolver;
    }
  },
  {
    token: ZLINK_ACTOR_SPOT_HANDLE_RESOLVER,
    requiresRuntime: true,
    isEnabled: (registration) => hasLocationStores(registration),
    create: (_registration, runtime) => {
      const resolver = requireRuntime(runtime).createLocationHandleResolver();
      if (resolver === undefined) {
        throw new framework.ZLinkConfigurationException('Actor SpotHandle resolver requires location stores.');
      }
      return resolver;
    }
  }
];

export function conditionalClientProvidersForFactory(): Provider[] {
  return CONDITIONAL_CLIENT_PROVIDER_SPECS.map(createConditionalClientProviderForFactory);
}

function createConditionalClientProviderForFactory(spec: ConditionalClientProviderSpec): Provider {
  return createConditionalClientProviderFromSpec(spec, { checkEnabled: true });
}

function createConditionalClientProvider(
  spec: ConditionalClientProviderSpec,
  registration: ZLinkFrameworkRegistration
): Provider {
  return createConditionalClientProviderFromSpec(spec, { registration, checkEnabled: false });
}

interface ConditionalClientProviderOptions {
  readonly registration?: ZLinkFrameworkRegistration;
  readonly checkEnabled: boolean;
}

function createConditionalClientProviderFromSpec(
  spec: ConditionalClientProviderSpec,
  options: ConditionalClientProviderOptions
): Provider {
  return {
    provide: spec.token,
    inject: conditionalClientProviderInject(spec, options),
    useFactory: (...args: unknown[]) => {
      const { registration, runtime, moduleRef, discovery } = conditionalClientProviderArgs(spec, options, args);
      if (options.checkEnabled && !spec.isEnabled(registration)) {
        return null;
      }
      return spec.create(registration, runtime, moduleRef, discovery);
    }
  };
}

function conditionalClientProviderInject(
  spec: ConditionalClientProviderSpec,
  options: ConditionalClientProviderOptions
): InjectionToken[] {
  return [
    ...(options.registration === undefined ? [ZLINK_FRAMEWORK_REGISTRATION] : []),
    ...(spec.requiresRuntime ? [ZLINK_FRAMEWORK_RUNTIME] : []),
    ModuleRef,
    DiscoveryService
  ];
}

function conditionalClientProviderArgs(
  spec: ConditionalClientProviderSpec,
  options: ConditionalClientProviderOptions,
  args: readonly unknown[]
): {
  readonly registration: ZLinkFrameworkRegistration;
  readonly runtime: FrameworkRuntimeHost | undefined;
  readonly moduleRef: ModuleRef;
  readonly discovery: DiscoveryService;
} {
  let index = 0;
  const registration = options.registration ?? args[index++] as ZLinkFrameworkRegistration;
  const runtime = spec.requiresRuntime ? args[index++] as FrameworkRuntimeHost : undefined;
  const moduleRef = args[index++] as ModuleRef;
  const discovery = args[index++] as DiscoveryService;
  return { registration, runtime, moduleRef, discovery };
}

function requireRuntime(runtime: FrameworkRuntimeHost | undefined): FrameworkRuntimeHost {
  if (runtime === undefined) {
    throw new framework.ZLinkConfigurationException('ZLink runtime host is not available.');
  }
  return runtime;
}

export function conditionalClientTokens(): InjectionToken[] {
  return [
    ZLINK_CLIENT_SERVER_RUNTIME,
    ZLINK_FANOUT_RUNTIME,
    ZLINK_ROUTE_MESH_RUNTIME_OPTIONS,
    ZLINK_ROUTE_MESH_RUNTIME,
    ZLINK_SPOT_MANAGER,
    ZLINK_SPOT_OUTBOUND,
    ZLINK_SPOT_PUBLISHER_CLIENT,
    ZLINK_ACTOR_CLIENT,
    ZLINK_ACTOR_MANAGER,
    ZLINK_LOCATION_RUNTIME_QUERY
  ];
}

function hasLocationStores(registration: ZLinkFrameworkRegistration): boolean {
  return registration.locations.useInMemoryStores
    || registration.locations.storeInstance !== undefined;
}

export function createRuntimeHost(
  registration: ZLinkFrameworkRegistration,
  moduleRef: ModuleRef,
  discovery: DiscoveryService
): RuntimeHostWithNestLifecycle {
  const runtime = framework.createIntegrationRuntimeHost(
    registration,
    createProviderResolver(moduleRef, discovery)
  ) as RuntimeHostWithNestLifecycle;
  runtime.onModuleInit = async () => {
    await runtime.start();
  };
  runtime.onModuleDestroy = async () => {
    // Nest application-context teardown does not necessarily run OS-signal
    // shutdown hooks. Close the runtime here as well; stop is idempotent after
    // an ordered shutdown has already completed.
    await runtime.stop();
  };
  runtime.onApplicationBootstrap = async () => {};
  return runtime;
}

function createProviderResolver(moduleRef: ModuleRef, discovery?: DiscoveryService): ZLinkProviderResolver {
  const resolver: ZLinkProviderResolver = {
    get<T>(type: Type<T>): T | undefined {
      const discovered = findDiscoveredProviderInstance<T>(discovery, type);
      if (discovered !== undefined) {
        return discovered;
      }
      try {
        return moduleRef.get(type, { strict: false });
      } catch {
        return undefined;
      }
    },
    async create<T>(type: Type<T>): Promise<T> {
      try {
        // Spot and actor lifecycles request a fresh application object for
        // each activation. ModuleRef.resolve() returns the registered
        // singleton when the application also lists that class as a Nest
        // provider, which would make later Spot contexts overwrite earlier
        // activations. ModuleRef.create() preserves dependency injection
        // while creating an independent object.
        return await moduleRef.create(type as unknown as import('@nestjs/common').Type<T>);
      } catch {
        // Fall back to direct construction through Nest for classes that are
        // not registered as providers.
      }
      return await moduleRef.resolve(type, undefined, { strict: false });
    }
  };
  Object.defineProperty(
    resolver,
    Symbol.for('@zlink-systems/framework.handler-instance-scope-factory'),
    {
      value: {
        create(context?: import('@zlink-systems/framework').ZLinkMessageContext) {
          const currentContextId = currentNestDispatchContext();
          const contextId = currentContextId ?? ContextIdFactory.create();
          if (context !== undefined && currentContextId === undefined) {
            moduleRef.registerRequestByContextId({ zlinkContext: context }, contextId);
          }
          const instances = new Map<Type, Promise<unknown>>();
          const dependencies = new Map<unknown, Promise<unknown>>();
          const owned: unknown[] = [];
          let disposed = false;
          return {
            async resolve<T>(type: Type<T>): Promise<T> {
              if (disposed) {
                throw new Error('Handler instance scope is already disposed.');
              }
              let instance = instances.get(type);
              if (instance === undefined) {
                instance = createNestHandlerInstance(
                  moduleRef,
                  contextId,
                  dependencies,
                  type
                )
                  .then(async (created) => {
                    if (disposed) {
                      await disposeNestOwnedHandler(created);
                      throw new Error(
                        'Handler instance scope was disposed during activation.'
                      );
                    }
                    owned.push(created);
                    return created;
                  });
                instances.set(type, instance);
              }
              return await instance as T;
            },
            async dispose(): Promise<void> {
              if (disposed) return;
              disposed = true;
              const pending = [...instances.values()];
              await Promise.allSettled(pending);
              for (let index = owned.length - 1; index >= 0; index -= 1) {
                await disposeNestOwnedHandler(owned[index]);
              }
              owned.length = 0;
              instances.clear();
              dependencies.clear();
            }
          };
        }
      },
      enumerable: false
    }
  );
  framework.registerIntegrationHandlerFilterScope(
    resolver,
    (context, callback) => runInNestDispatchScope(
      moduleRef,
      context,
      async () => {
        const contextId = currentNestDispatchContext()!;
        const dependencies = new Map<unknown, Promise<unknown>>();
        const owned: unknown[] = [];
        try {
          return await callback({
            async resolve<T>(type: Type<T>): Promise<T> {
              const instance = await createNestHandlerInstance(
                moduleRef,
                contextId,
                dependencies,
                type
              );
              owned.push(instance);
              return instance;
            }
          });
        } finally {
          for (let index = owned.length - 1; index >= 0; index -= 1) {
            await disposeNestOwnedHandler(owned[index]);
          }
        }
      }
    )
  );
  return resolver;
}

export async function disposeNestOwnedHandler(instance: unknown): Promise<void> {
  if (instance === null || instance === undefined) return;
  const value = instance as {
    dispose?: () => unknown;
    close?: () => unknown;
    onModuleDestroy?: () => unknown;
  };
  if (typeof value.dispose === 'function') {
    await value.dispose();
  } else if (typeof value.close === 'function') {
    await value.close();
  } else if (typeof value.onModuleDestroy === 'function') {
    await value.onModuleDestroy();
  }
}

export async function createNestHandlerInstance<T>(
  moduleRef: ModuleRef,
  contextId: import('@nestjs/core').ContextId,
  dependencies: Map<unknown, Promise<unknown>>,
  type: Type<T>
): Promise<T> {
  const reflected = [
    ...(Reflect.getMetadata(PARAMTYPES_METADATA, type) as readonly InjectionToken[] | undefined ?? [])
  ];
  const declared = Reflect.getMetadata(
    SELF_DECLARED_DEPS_METADATA,
    type
  ) as readonly { readonly index: number; readonly param: InjectionToken }[] | undefined;
  for (const dependency of declared ?? []) {
    reflected[dependency.index] = dependency.param;
  }
  const optional = new Set<number>(
    Reflect.getMetadata(OPTIONAL_DEPS_METADATA, type) as readonly number[] | undefined ?? []
  );
  const parameters = await Promise.all(reflected.map((token, index) =>
    resolveNestHandlerDependency(
      moduleRef,
      contextId,
      dependencies,
      token,
      optional.has(index)
    )
  ));
  const instance = new (type as unknown as new (...args: unknown[]) => T)(...parameters);
  const properties = Reflect.getMetadata(
    PROPERTY_DEPS_METADATA,
    type
  ) as readonly { readonly key: string | symbol; readonly type: InjectionToken }[] | undefined;
  const optionalProperties = new Set<string | symbol>(
    Reflect.getMetadata(
      OPTIONAL_PROPERTY_DEPS_METADATA,
      type
    ) as readonly (string | symbol)[] | undefined ?? []
  );
  for (const property of properties ?? []) {
    const dependency = await resolveNestHandlerDependency(
      moduleRef,
      contextId,
      dependencies,
      property.type,
      optionalProperties.has(property.key)
    );
    if (dependency !== undefined) {
      (instance as Record<string | symbol, unknown>)[property.key] = dependency;
    }
  }
  return instance;
}

async function resolveNestHandlerDependency(
  moduleRef: ModuleRef,
  contextId: import('@nestjs/core').ContextId,
  dependencies: Map<unknown, Promise<unknown>>,
  token: unknown,
  optional: boolean
): Promise<unknown> {
  const forwardReference = typeof token === 'object'
    && token !== null
    && 'forwardRef' in token
    ? token as { readonly forwardRef?: unknown }
    : undefined;
  const resolvedToken = typeof forwardReference?.forwardRef === 'function'
    ? (forwardReference.forwardRef as () => unknown)()
    : token;
  let dependency = dependencies.get(resolvedToken);
  if (dependency === undefined) {
    dependency = moduleRef.resolve(
      resolvedToken as Parameters<ModuleRef['get']>[0],
      contextId,
      { strict: false }
    );
    dependencies.set(resolvedToken, dependency);
  }
  try {
    return await dependency;
  } catch (error) {
    dependencies.delete(resolvedToken);
    if (optional) return undefined;
    throw error;
  }
}

function findDiscoveredProviderInstance<T>(discovery: DiscoveryService | undefined, type: Type<T>): T | undefined {
  for (const wrapper of discovery?.getProviders() ?? []) {
    if (
      wrapper.instance !== undefined
      && wrapper.instance !== null
      && (
        wrapper.token === type
        || wrapper.metatype === type
        || wrapper.instance.constructor === type
      )
    ) {
      return wrapper.instance as T;
    }
  }
  return undefined;
}

export function providerToken(provider: Provider): InjectionToken {
  return typeof provider === 'function' ? provider : provider.provide;
}

async function createSpotManager(
  registration: ZLinkFrameworkRegistration,
  runtime: FrameworkRuntimeHost,
  moduleRef: ModuleRef | undefined,
  discovery: DiscoveryService | undefined
): Promise<unknown> {
  return framework.createIntegrationSpotManager(
    registration,
    runtime,
    moduleRef === undefined ? undefined : createProviderResolver(moduleRef, discovery)
  );
}

async function createSpotOutbound(
  _registration: ZLinkFrameworkRegistration,
  runtime: FrameworkRuntimeHost,
  _moduleRef: ModuleRef | undefined,
  _discovery: DiscoveryService | undefined
): Promise<unknown> {
  return framework.createIntegrationSpotOutbound(runtime);
}
