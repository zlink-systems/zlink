import type { InjectionToken, OnModuleDestroy, OnModuleInit, Provider } from '@nestjs/common';
import { Scope } from '@nestjs/common';
import {
  OPTIONAL_DEPS_METADATA,
  OPTIONAL_PROPERTY_DEPS_METADATA,
  PARAMTYPES_METADATA,
  PROPERTY_DEPS_METADATA,
  SELF_DECLARED_DEPS_METADATA
} from '@nestjs/common/constants';
import { ContextIdFactory, DiscoveryService, ModuleRef } from '@nestjs/core';
import { UnknownElementException } from '@nestjs/core/errors/exceptions/unknown-element.exception';
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
  ZLINK_SPOT_PUBLISHER_CLIENT
} from './tokens';
import { ZLINK_ACTOR_SPOT_HANDLE_RESOLVER, ZLINK_SPOT_HANDLE_RESOLVER } from './internal-tokens';
import { framework, type FrameworkRuntimeHost } from './framework-loader';
import { currentNestDispatchContext, runInNestDispatchScope } from './dispatch-scope';

type RuntimeHostWithNestLifecycle = FrameworkRuntimeHost & OnModuleInit & OnModuleDestroy;

interface AlwaysAvailableClientProviderSpec {
  readonly token: InjectionToken;
  create(registration: ZLinkFrameworkRegistration, runtime: FrameworkRuntimeHost): unknown;
}

const ALWAYS_AVAILABLE_CLIENT_PROVIDER_SPECS: readonly AlwaysAvailableClientProviderSpec[] = [
  {
    token: ZLINK_CHANNEL_CLIENT,
    create: (registration, runtime) =>
      framework.createIntegrationChannelClient(registration, runtime)
  },
  {
    token: ZLINK_CHANNEL_RUNTIME_OPTIONS,
    create: (_registration, runtime) => runtime.channelRuntimeOptions
  },
  {
    token: ZLINK_FANOUT_CLIENT,
    create: (registration, runtime) =>
      framework.createIntegrationFanoutClient(registration, runtime)
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

export function alwaysAvailableClientProviders(
  registration?: ZLinkFrameworkRegistration
): Provider[] {
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
    ZLINK_MESSAGE_METADATA_POLICY
  ];
}

export function conditionalClientProviders(registration: ZLinkFrameworkRegistration): Provider[] {
  return CONDITIONAL_CLIENT_PROVIDER_SPECS.filter((spec) => spec.isEnabled(registration)).map(
    (spec) => createConditionalClientProvider(spec, registration)
  );
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
    isEnabled: (registration) =>
      [...registration.channels.values()].some(
        (channel) => channel.client !== undefined || channel.server !== undefined
      ),
    create: (_registration, runtime) => requireRuntime(runtime).clientServerRuntime
  },
  {
    token: ZLINK_FANOUT_RUNTIME,
    requiresRuntime: true,
    isEnabled: (registration) =>
      [...registration.channels.values()].some(
        (channel) =>
          channel.subscriber !== undefined &&
          (channel.subscriber.manualConnections?.length ?? 0) === 0
      ),
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
    isEnabled: (registration) =>
      framework.hasSpotNode(registration) && hasLocationStores(registration),
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
    isEnabled: (registration) =>
      framework.hasSpotNode(registration) && hasLocationStores(registration),
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
        moduleRef === undefined
          ? undefined
          : createProviderResolver(moduleRef, discovery, registration)
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
        throw new framework.ZLinkConfigurationException(
          'Location runtime query requires location stores.'
        );
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
        throw new framework.ZLinkConfigurationException(
          'SpotHandle resolver requires location stores.'
        );
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
        throw new framework.ZLinkConfigurationException(
          'Actor SpotHandle resolver requires location stores.'
        );
      }
      return resolver;
    }
  }
];

export function conditionalClientProvidersForFactory(
  registration?: ZLinkFrameworkRegistration
): Provider[] {
  return CONDITIONAL_CLIENT_PROVIDER_SPECS.filter(
    (spec) => registration === undefined || spec.isEnabled(registration)
  ).map((spec) =>
    createConditionalClientProviderFromSpec(spec, { checkEnabled: registration === undefined })
  );
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
      const { registration, runtime, moduleRef, discovery } = conditionalClientProviderArgs(
        spec,
        options,
        args
      );
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
  const registration = options.registration ?? (args[index++] as ZLinkFrameworkRegistration);
  const runtime = spec.requiresRuntime ? (args[index++] as FrameworkRuntimeHost) : undefined;
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
  return (
    registration.locations.useInMemoryStores || registration.locations.storeInstance !== undefined
  );
}

export function createRuntimeHost(
  registration: ZLinkFrameworkRegistration,
  moduleRef: ModuleRef,
  discovery: DiscoveryService
): RuntimeHostWithNestLifecycle {
  const runtime = framework.createIntegrationRuntimeHost(
    registration,
    createProviderResolver(moduleRef, discovery, registration)
  ) as RuntimeHostWithNestLifecycle;
  runtime.onModuleInit = async () => {
    await runtime.start();
  };
  runtime.onModuleDestroy = async () => {
    await runtime.shutdown();
  };
  runtime.onApplicationBootstrap = async () => {};
  return runtime;
}

function createProviderResolver(
  moduleRef: ModuleRef,
  discovery: DiscoveryService | undefined,
  registration: ZLinkFrameworkRegistration
): ZLinkProviderResolver {
  const factories = nestHandlerFactories(registration);
  const types = new Set<Type>([...registration.spotFactories, ...registration.filterTypes]);
  for (const node of registration.spotNodes.values()) {
    if (node.entrySpotType !== undefined) types.add(node.entrySpotType);
    for (const type of node.spotFactories ?? []) types.add(type);
    for (const type of Object.values(node.instanceSpotFactories ?? {})) types.add(type);
    for (const value of Object.values(node.spotFactoryRegistrations ?? {}))
      types.add(value.implementation);
    for (const value of Object.values(node.instanceSpotFactoryRegistrations ?? {}))
      types.add(value.implementation);
    for (const handler of [
      ...(node.routeSendHandlers ?? []),
      ...(node.routeRequestHandlers ?? []),
      ...(node.entrySpotTimerHandlers ?? []),
      ...(node.entrySpotPacketHandlers ?? []),
      ...(node.entrySpotSubscriptionHandlers ?? []),
      ...(node.entrySpotActorSendHandlers ?? []),
      ...(node.entrySpotActorRequestHandlers ?? []),
      ...(node.spotTimerHandlers ?? []),
      ...(node.spotPacketHandlers ?? []),
      ...(node.spotSubscriptionHandlers ?? []),
      ...(node.spotActorSendHandlers ?? []),
      ...(node.spotActorRequestHandlers ?? [])
    ]) {
      const handlerType = (handler as { readonly handlerType?: Type }).handlerType;
      if (handlerType !== undefined) types.add(handlerType);
    }
    for (const channel of Object.values(node.meshChannels ?? {})) {
      for (const handler of [...(channel.sendHandlers ?? []), ...(channel.requestHandlers ?? [])]) {
        const handlerType = (handler as { readonly handlerType?: Type }).handlerType;
        if (handlerType !== undefined) types.add(handlerType);
      }
    }
  }
  function prepareHandler(type: Type): (() => unknown) | undefined {
    return registeredNestHandlerFactory(factories, type, moduleRef).scopeFree;
  }
  for (const type of types) prepareHandler(type);
  function createHandler<T>(
    contextId: import('@nestjs/core').ContextId,
    dependencies: Map<unknown, unknown>,
    type: Type<T>
  ): T | Promise<T> {
    const factory = factories.get(type);
    if (factory === undefined) throw new Error(`Handler creation was not prepared: ${type.name}`);
    return factory(moduleRef, contextId, dependencies) as T | Promise<T>;
  }
  const resolver: ZLinkProviderResolver = {
    get<T>(type: Type<T>): T | undefined {
      if (discovery !== undefined) {
        return findDiscoveredProvider(discovery, type)?.instance as T | undefined;
      }
      try {
        return moduleRef.get(type, { strict: false });
      } catch {
        return undefined;
      }
    },
    async create<T>(type: Type<T>): Promise<T> {
      const owner = findDiscoveredProvider(discovery, type)?.host;
      const creationModuleRef = owner?.getProviderByKey<ModuleRef>(ModuleRef).instance ?? moduleRef;
      return await creationModuleRef.create(type as unknown as import('@nestjs/common').Type<T>);
    }
  };
  let freeScopeStorage: NestHandlerScopeStorage | undefined;
  framework.registerIntegrationHandlerInstanceScopeFactory(resolver, {
    prepare: prepareHandler,
    create(context?: import('@zlink-systems/framework').ZLinkMessageContext) {
      const currentContextId = currentNestDispatchContext();
      const contextId = currentContextId ?? ContextIdFactory.create();
      if (context !== undefined && currentContextId === undefined) {
        moduleRef.registerRequestByContextId({ zlinkContext: context }, contextId);
      }
      const storage = freeScopeStorage ?? new NestHandlerScopeStorage();
      freeScopeStorage = storage.next;
      storage.next = undefined;
      const { instances, dependencies, owned } = storage;
      let disposed = false;
      return {
        resolve<T>(type: Type<T>): Promise<T> {
          if (disposed) {
            return Promise.reject(new Error('Handler instance scope is already disposed.'));
          }
          let instance = instances.get(type);
          if (instance === undefined) {
            try {
              const finish = (created: unknown) => {
                if (disposed) {
                  return framework.disposeIntegrationHandler(created).then(() => {
                    throw new Error('Handler instance scope was disposed during activation.');
                  });
                }
                owned.push(created);
                return created;
              };
              const created = createHandler(contextId, dependencies, type);
              instance = isNestPromiseLike(created)
                ? Promise.resolve(created).then(finish)
                : Promise.resolve(finish(created));
              instances.set(type, instance);
            } catch (error) {
              return Promise.reject(error);
            }
          }
          return instance as Promise<T>;
        },
        async dispose(): Promise<void> {
          if (disposed) return;
          disposed = true;
          if (owned.length !== instances.size) await Promise.allSettled(instances.values());
          try {
            await framework.disposeIntegrationHandlers(owned);
          } finally {
            owned.length = 0;
            instances.clear();
            dependencies.clear();
            storage.next = freeScopeStorage;
            freeScopeStorage = storage;
          }
        }
      };
    }
  });
  framework.registerIntegrationHandlerFilterScope(resolver, (context, callback) =>
    runInNestDispatchScope(moduleRef, context, async () => {
      const contextId = currentNestDispatchContext()!;
      const dependencies = new Map<unknown, Promise<unknown>>();
      const owned: unknown[] = [];
      try {
        return await callback({
          async resolve<T>(type: Type<T>): Promise<T> {
            const instance = await createHandler(contextId, dependencies, type);
            owned.push(instance);
            return instance;
          }
        });
      } finally {
        await framework.disposeIntegrationHandlers(owned);
      }
    })
  );
  return resolver;
}

const NEST_HANDLER_FACTORIES = Symbol('Nest handler factories');
export type NestHandlerFactories = Map<Type, NestHandlerFactory>;
type PreparedNestRegistration = ZLinkFrameworkRegistration & {
  [NEST_HANDLER_FACTORIES]?: NestHandlerFactories;
};

export function nestHandlerFactories(
  registration: ZLinkFrameworkRegistration,
  prepared?: NestHandlerFactories
): NestHandlerFactories {
  const owner = registration as PreparedNestRegistration;
  return (owner[NEST_HANDLER_FACTORIES] ??= prepared ?? new Map());
}

export function registeredNestHandlerFactory<T>(
  factories: NestHandlerFactories,
  type: Type<T>,
  moduleRef: ModuleRef
): NestHandlerFactory<T> {
  let factory = factories.get(type);
  if (factory === undefined) {
    factory = prepareNestHandlerFactory(type, moduleRef);
    factories.set(type, factory);
  }
  return factory as NestHandlerFactory<T>;
}

class NestHandlerScopeStorage {
  readonly instances = new Map<Type, Promise<unknown>>();
  readonly dependencies = new Map<unknown, unknown>();
  readonly owned: unknown[] = [];
  next?: NestHandlerScopeStorage;
}

export type NestHandlerFactory<T = unknown> = ((
  moduleRef: ModuleRef,
  contextId: import('@nestjs/core').ContextId,
  dependencies: Map<unknown, unknown>
) => T | Promise<T>) & { readonly scopeFree?: () => T };

export function prepareNestHandlerFactory<T>(
  type: Type<T>,
  moduleRef?: ModuleRef
): NestHandlerFactory<T> {
  const { reflected, optional } = nestConstructorDependencies(type);
  const parameters = reflected.map((token, index) =>
    prepareNestDependency(nestDependencyToken(token), optional.has(index), moduleRef)
  );
  const optionalProperties = new Set<string | symbol>(
    (Reflect.getMetadata(OPTIONAL_PROPERTY_DEPS_METADATA, type) as
      readonly (string | symbol)[] | undefined) ?? []
  );
  const properties = (
    (Reflect.getMetadata(PROPERTY_DEPS_METADATA, type) as
      readonly { readonly key: string | symbol; readonly type: InjectionToken }[] | undefined) ?? []
  ).map((property) => ({
    key: property.key,
    resolve: prepareNestDependency(
      nestDependencyToken(property.type),
      optionalProperties.has(property.key),
      moduleRef
    )
  }));
  const Constructor = type as unknown as new (...args: unknown[]) => T;
  if (parameters.length === 0 && properties.length === 0) {
    const activate = () => new Constructor();
    return Object.assign(activate, { scopeFree: activate });
  }
  return (moduleRef, contextId, dependencies) => {
    const create = (arguments_: unknown[]) =>
      injectNestHandlerProperties(
        new Constructor(...arguments_),
        properties,
        0,
        moduleRef,
        contextId,
        dependencies
      );
    const arguments_ = parameters.map((resolve) => resolve(moduleRef, contextId, dependencies));
    return arguments_.some(isNestPromiseLike)
      ? Promise.all(arguments_).then(create)
      : create(arguments_);
  };
}

type NestDependencyResolver = (
  moduleRef: ModuleRef,
  contextId: import('@nestjs/core').ContextId,
  dependencies: Map<unknown, unknown>
) => unknown;

function prepareNestDependency(
  token: unknown,
  optional: boolean,
  moduleRef?: ModuleRef
): NestDependencyResolver {
  let lookup = (ref: ModuleRef, contextId: import('@nestjs/core').ContextId): unknown =>
    ref.resolve(token as Parameters<ModuleRef['resolve']>[0], contextId, { strict: false });
  if (moduleRef !== undefined) {
    try {
      if (
        moduleRef.introspect(token as Parameters<ModuleRef['introspect']>[0]).scope ===
        Scope.DEFAULT
      ) {
        lookup = (ref) => ref.get(token as Parameters<ModuleRef['get']>[0], { strict: false });
      }
    } catch (error) {
      if (!(error instanceof UnknownElementException) || !optional) throw error;
    }
  }
  return (ref, contextId, dependencies) =>
    resolveNestHandlerDependency(dependencies, token, optional, lookup, ref, contextId);
}

function injectNestHandlerProperties<T>(
  instance: T,
  properties: readonly {
    readonly key: string | symbol;
    readonly resolve: NestDependencyResolver;
  }[],
  index: number,
  moduleRef: ModuleRef,
  contextId: import('@nestjs/core').ContextId,
  dependencies: Map<unknown, unknown>
): T | Promise<T> {
  for (; index < properties.length; index++) {
    const property = properties[index]!;
    const dependency = property.resolve(moduleRef, contextId, dependencies);
    if (isNestPromiseLike(dependency)) {
      const next = index + 1;
      return Promise.resolve(dependency).then((value) => {
        assignNestHandlerProperty(instance, property.key, value);
        return injectNestHandlerProperties(
          instance,
          properties,
          next,
          moduleRef,
          contextId,
          dependencies
        );
      });
    }
    assignNestHandlerProperty(instance, property.key, dependency);
  }
  return instance;
}

function assignNestHandlerProperty(
  instance: unknown,
  key: string | symbol,
  dependency: unknown
): void {
  if (dependency !== undefined) (instance as Record<string | symbol, unknown>)[key] = dependency;
}

function isNestPromiseLike(value: unknown): value is Promise<unknown> {
  return (
    value !== null &&
    (typeof value === 'object' || typeof value === 'function') &&
    typeof (value as { then?: unknown }).then === 'function'
  );
}

function nestConstructorDependencies(type: Type) {
  const reflected = [
    ...((Reflect.getMetadata(PARAMTYPES_METADATA, type) as readonly InjectionToken[] | undefined) ??
      [])
  ];
  const declared = Reflect.getMetadata(SELF_DECLARED_DEPS_METADATA, type) as
    readonly { readonly index: number; readonly param: InjectionToken }[] | undefined;
  for (const dependency of declared ?? []) {
    reflected[dependency.index] = dependency.param;
  }
  const optional = new Set<number>(
    (Reflect.getMetadata(OPTIONAL_DEPS_METADATA, type) as readonly number[] | undefined) ?? []
  );
  return { reflected, optional };
}

function nestDependencyToken(token: unknown): unknown {
  const forwardReference =
    typeof token === 'object' && token !== null && 'forwardRef' in token
      ? (token as { readonly forwardRef?: unknown })
      : undefined;
  return typeof forwardReference?.forwardRef === 'function'
    ? (forwardReference.forwardRef as () => unknown)()
    : token;
}

function resolveNestHandlerDependency(
  dependencies: Map<unknown, unknown>,
  token: unknown,
  optional: boolean,
  lookup: (moduleRef: ModuleRef, contextId: import('@nestjs/core').ContextId) => unknown,
  moduleRef: ModuleRef,
  contextId: import('@nestjs/core').ContextId
): unknown {
  if (dependencies.has(token)) return dependencies.get(token);
  try {
    const dependency = lookup(moduleRef, contextId);
    const result = isNestPromiseLike(dependency)
      ? Promise.resolve(dependency).then(
          (value) => {
            dependencies.set(token, value);
            return value;
          },
          (error) => nestDependencyFailure(dependencies, token, optional, error)
        )
      : dependency;
    dependencies.set(token, result);
    return result;
  } catch (error) {
    return nestDependencyFailure(dependencies, token, optional, error);
  }
}

function nestDependencyFailure(
  dependencies: Map<unknown, unknown>,
  token: unknown,
  optional: boolean,
  error: unknown
): undefined {
  dependencies.delete(token);
  if (optional) return undefined;
  throw error;
}

function findDiscoveredProvider<T>(discovery: DiscoveryService | undefined, type: Type<T>) {
  for (const wrapper of discovery?.getProviders() ?? []) {
    if (
      wrapper.token === type ||
      wrapper.metatype === type ||
      wrapper.instance?.constructor === type
    ) {
      return wrapper;
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
    moduleRef === undefined ? undefined : createProviderResolver(moduleRef, discovery, registration)
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
