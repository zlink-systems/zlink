import { createRequire } from 'node:module';
import fs from 'node:fs';
import path from 'node:path';
import { Module } from '@nestjs/common';
import type { DynamicModule, ModuleMetadata, Provider } from '@nestjs/common';
import { DiscoveryModule, DiscoveryService, ModuleRef } from '@nestjs/core';
import type { Type } from '@zlink-systems/framework';
import type { ZLinkFrameworkRegistration } from './framework-integration-contracts';
import {
  ZLINK_FRAMEWORK_REGISTRATION,
  ZLINK_FRAMEWORK_RUNTIME,
} from './tokens';
import {
  hasNestSpotTimerHandlerMetadata,
  readNestHandlerMetadata,
  readNestSpotActorHandlerMetadata,
  readNestSpotHandlerMetadata
} from './handler-metadata';
import type {
  ZLinkModuleFactoryOptions,
  ZLinkModuleOptions,
  ZLinkNestModuleMetadata,
  ZLinkNestModuleRegistrationOptions,
  ZLinkNestModuleRoleRoot,
  ZLinkNestProviderDiscoveryOptions,
  ZLinkNestProviderDiscoveryRoot
} from './contracts';
import { framework } from './framework-loader';
import { createZLinkNestFrameworkOptionsBuilder } from './options-builder';
import {
  assertBuiltModuleOptions,
  createDiscoveredOptions,
  createRegistrationOptions,
  hasNestHandlerDiscovery
} from './registration-composer';
import {
  alwaysAvailableClientProviders,
  alwaysAvailableClientTokens,
  conditionalClientProviders,
  conditionalClientProvidersForFactory,
  conditionalClientTokens,
  createRuntimeHost,
  providerToken
} from './providers';
import { markAutoDiscoveredProvider } from './auto-discovery-marker';


export function zlinkDiscoverProviders(
  rootDir: string,
  options: ZLinkNestProviderDiscoveryOptions = {}
): Provider[] {
  return [...loadDecoratedProviderModules(rootDir, options)];
}

export function zlinkModule(metadata: ZLinkNestModuleMetadata): ClassDecorator;
export function zlinkModule(roleRoot: ZLinkNestModuleRoleRoot, metadata: ModuleMetadata): ClassDecorator;
export function zlinkModule(
  metadataOrRoleRoot: ZLinkNestModuleMetadata | ZLinkNestModuleRoleRoot,
  metadata?: ModuleMetadata
): ClassDecorator {
  const moduleMetadata = typeof metadataOrRoleRoot === 'string' ? metadata : metadataOrRoleRoot;
  if (moduleMetadata === undefined) {
    throw new framework.ZLinkConfigurationException('zlinkModule metadata is required.');
  }
  const { providerDiscovery, providers, ...rest } = moduleMetadata as ZLinkNestModuleMetadata;
  const discoveredProviders = typeof metadataOrRoleRoot === 'string'
    ? createDefaultProviderDiscoveryProviders(metadataOrRoleRoot)
    : createProviderDiscoveryProviders(providerDiscovery);
  return Module({
    ...rest,
    providers: [
      ...(providers ?? []),
      ...discoveredProviders
    ]
  });
}

function loadDecoratedProviderModules(rootDir: string, options: ZLinkNestProviderDiscoveryOptions): Set<Type> {
  if (!fs.existsSync(rootDir)) {
    throw new framework.ZLinkConfigurationException(`ZLink provider discovery root does not exist: ${rootDir}`);
  }
  const providers = new Set<Type>();
  const stat = fs.statSync(rootDir);
  if (stat.isFile()) {
    addDecoratedProviderModuleExports(providers, rootDir);
    return providers;
  }
  for (const entry of fs.readdirSync(rootDir, { withFileTypes: true })) {
    const fullPath = path.join(rootDir, entry.name);
    if (entry.isDirectory()) {
      if (options.recursive === true) {
        for (const provider of loadDecoratedProviderModules(fullPath, options)) {
          providers.add(provider);
        }
      }
      continue;
    }
    if (entry.isFile()) {
      addDecoratedProviderModuleExports(providers, fullPath);
    }
  }
  return providers;
}

function addDecoratedProviderModuleExports(providers: Set<Type>, filePath: string): void {
  if (!/\.(?:cjs|mjs|js)$/.test(filePath) || /\.d\.js$/.test(filePath)) {
    return;
  }
  const source = fs.readFileSync(filePath, 'utf8');
  if (!/(?:zlink(?:Request|Send|Publish|Spot|EntrySpot)[A-Za-z]*Handler|ZLinkPacket)/.test(source)) {
    return;
  }
  const loaded = createRequire(__filename)(filePath) as Record<string, unknown>;
  for (const value of Object.values(loaded)) {
    const frameworkMetadata = typeof value === 'function'
      ? ((value as unknown as Record<symbol, readonly { readonly kind?: string }[]>)[
        Symbol.for('@zlink-systems/framework:decorator')
      ] ?? [])
      : [];
    if (
      typeof value === 'function'
      && (
        readNestHandlerMetadata(value as Type).length > 0
        || readNestSpotActorHandlerMetadata(value as Type).length > 0
        || readNestSpotHandlerMetadata(value as Type).length > 0
        || hasNestSpotTimerHandlerMetadata(value as Type)
        || (
          frameworkMetadata.some((entry) => entry.kind === 'packet')
          && typeof (value as { prototype?: { handle?: unknown } }).prototype?.handle === 'function'
        )
      )
    ) {
      providers.add(markAutoDiscoveredProvider(value as Type));
    }
  }
}

@Module({})
export class ZLinkModule {
  static forRoot(options: ZLinkModuleOptions = createZLinkNestFrameworkOptionsBuilder().build()): DynamicModule {
    const resolvedOptions = assertBuiltModuleOptions(options);
    if (hasNestHandlerDiscovery(resolvedOptions)) {
      return createDiscoveringZLinkDynamicModule(resolvedOptions);
    }
    return createZLinkDynamicModule(framework.createFrameworkRegistration(createRegistrationOptions(resolvedOptions)));
  }

  static forRootFactory<TArgs extends unknown[]>(
    options: ZLinkModuleFactoryOptions<TArgs>
  ): DynamicModule {
    const registrationProvider: Provider<Promise<ZLinkFrameworkRegistration>> = {
      provide: ZLINK_FRAMEWORK_REGISTRATION,
      inject: [...(options.inject ?? []), DiscoveryService, ModuleRef],
      useFactory: async (...args: unknown[]) => {
        const discovery = args[args.length - 2] as DiscoveryService;
        const moduleRef = args[args.length - 1] as ModuleRef;
        const factoryArgs = args.slice(0, -2) as TArgs;
        const resolvedOptions = assertBuiltModuleOptions(await options.useFactory(...factoryArgs));
        return framework.createFrameworkRegistration(createDiscoveredOptions(resolvedOptions, discovery, moduleRef));
      }
    };

    return {
      module: ZLinkModule,
      imports: [...(options.imports ?? []), DiscoveryModule],
      providers: [
        registrationProvider,
        {
          provide: ZLINK_FRAMEWORK_RUNTIME,
          inject: [ZLINK_FRAMEWORK_REGISTRATION, ModuleRef, DiscoveryService],
          useFactory: (registration: ZLinkFrameworkRegistration, moduleRef: ModuleRef, discovery: DiscoveryService) =>
            createRuntimeHost(registration, moduleRef, discovery)
        },
        ...alwaysAvailableClientProviders(),
        ...conditionalClientProvidersForFactory()
      ],
      exports: [
        ZLINK_FRAMEWORK_RUNTIME,
        ...alwaysAvailableClientTokens(),
        ...conditionalClientTokens()
      ]
    };
  }
}

export function createZLinkDynamicModule(registration: ZLinkFrameworkRegistration): DynamicModule {
  const providers: Provider[] = [
    { provide: ZLINK_FRAMEWORK_REGISTRATION, useValue: registration },
    {
      provide: ZLINK_FRAMEWORK_RUNTIME,
      inject: [ModuleRef, DiscoveryService],
      useFactory: (moduleRef: ModuleRef, discovery: DiscoveryService) => createRuntimeHost(registration, moduleRef, discovery)
    },
    ...alwaysAvailableClientProviders(registration),
    ...conditionalClientProviders(registration)
  ];

  return {
    module: ZLinkModule,
    imports: [DiscoveryModule],
    providers,
    exports: providers.map(providerToken)
  };
}

function createDiscoveringZLinkDynamicModule(options: ZLinkNestModuleRegistrationOptions): DynamicModule {
  const registrationProvider: Provider = {
    provide: ZLINK_FRAMEWORK_REGISTRATION,
    inject: [DiscoveryService, ModuleRef],
    useFactory: (discovery: DiscoveryService, moduleRef: ModuleRef) =>
      framework.createFrameworkRegistration(createDiscoveredOptions(options, discovery, moduleRef))
  };

  return {
    module: ZLinkModule,
    imports: [DiscoveryModule],
    providers: [
      registrationProvider,
      {
        provide: ZLINK_FRAMEWORK_RUNTIME,
        inject: [ZLINK_FRAMEWORK_REGISTRATION, ModuleRef, DiscoveryService],
        useFactory: (registration: ZLinkFrameworkRegistration, moduleRef: ModuleRef, discovery: DiscoveryService) =>
          createRuntimeHost(registration, moduleRef, discovery)
      },
      ...alwaysAvailableClientProviders(),
      ...conditionalClientProvidersForFactory()
    ],
    exports: [
      ZLINK_FRAMEWORK_RUNTIME,
      ...alwaysAvailableClientTokens(),
      ...conditionalClientTokens()
    ]
  };
}

function createProviderDiscoveryProviders(
  roots: readonly ZLinkNestProviderDiscoveryRoot[] | undefined
): Provider[] {
  return (roots ?? []).flatMap((root) => {
    if (typeof root === 'string') {
      return zlinkDiscoverProviders(root);
    }
    return zlinkDiscoverProviders(root.rootDir, root.options);
  });
}

function createDefaultProviderDiscoveryProviders(roleRoot: string): Provider[] {
  return createProviderDiscoveryProviders(defaultProviderDiscoveryRoots(roleRoot));
}

function defaultProviderDiscoveryRoots(roleRoot: string): ZLinkNestProviderDiscoveryRoot[] {
  return fs.existsSync(roleRoot)
    ? [{ rootDir: roleRoot, options: { recursive: true } }]
    : [];
}
