import { Module } from '@nestjs/common';
import type { DynamicModule, InjectionToken, ModuleMetadata, OnModuleDestroy, Provider } from '@nestjs/common';
import { ModuleRef } from '@nestjs/core';
import {
  ZLinkHttpClient,
  type HttpResponse,
  type ZLinkHttpClientBuilder,
  type ZLinkHttpRequestBuilder
} from '@zlink-systems/http-client';
import type { ZLinkNestIntegrationRuntimeHost } from './framework-integration-contracts';
import { framework } from './framework-loader';
import { ZLINK_FRAMEWORK_RUNTIME } from './tokens';

export interface ZLinkNamedHttpClientOptions {
  readonly name: string;
  readonly baseUrl: string;
  readonly configure?: (builder: ZLinkHttpClientBuilder) => void;
}

export interface ZLinkHttpClientModuleOptions {
  readonly imports: ModuleMetadata['imports'];
  readonly clients: readonly ZLinkNamedHttpClientOptions[];
}

export interface ZLinkServerHttpRequestBuilder extends ZLinkHttpRequestBuilder {
  submit(): Promise<void>;
  yield<T>(): Promise<HttpResponse<T>>;
}

export interface ZLinkServerHttpClient extends Omit<
  ZLinkHttpClient,
  'get' | 'post' | 'put' | 'delete' | 'patch' | 'head' | 'options'
> {
  get(path: string): ZLinkServerHttpRequestBuilder;
  post(path: string): ZLinkServerHttpRequestBuilder;
  put(path: string): ZLinkServerHttpRequestBuilder;
  delete(path: string): ZLinkServerHttpRequestBuilder;
  patch(path: string): ZLinkServerHttpRequestBuilder;
  head(path: string): ZLinkServerHttpRequestBuilder;
  options(path: string): ZLinkServerHttpRequestBuilder;
}

const ZLINK_HTTP_CLIENT_REGISTRY = Symbol.for('@zlink-systems/nestjs:http-client-registry');

export function zlinkHttpClientToken(name: string): InjectionToken {
  const normalized = name.trim();
  if (normalized.length === 0) {
    throw new framework.ZLinkConfigurationException('HTTP client registration name is required.');
  }
  return Symbol.for(`@zlink-systems/nestjs:http-client:${normalized}`);
}

class ZLinkHttpClientRegistry implements OnModuleDestroy {
  private readonly clients = new Map<string, ZLinkServerHttpClient>();

  constructor(
    registrations: readonly ZLinkNamedHttpClientOptions[],
    moduleRef: ModuleRef
  ) {
    const runtime = new Proxy({} as ZLinkNestIntegrationRuntimeHost, {
      get: (_target, property) => {
        const current = moduleRef.get<ZLinkNestIntegrationRuntimeHost>(
          ZLINK_FRAMEWORK_RUNTIME,
          { strict: false }
        );
        return Reflect.get(current as object, property);
      }
    });
    const scheduler = framework.createIntegrationHttpExecutionScheduler(runtime);
    for (const registration of registrations) {
      const name = registration.name.trim();
      zlinkHttpClientToken(name);
      if (this.clients.has(name)) {
        throw new framework.ZLinkConfigurationException(`HTTP client '${name}' is already registered.`);
      }
      const builder = ZLinkHttpClient.create(registration.baseUrl).executionScheduler(scheduler);
      registration.configure?.(builder);
      this.clients.set(name, builder.build() as unknown as ZLinkServerHttpClient);
    }
  }

  get(name: string): ZLinkServerHttpClient {
    const client = this.clients.get(name);
    if (client === undefined) {
      throw new framework.ZLinkConfigurationException(`HTTP client '${name}' is not registered.`);
    }
    return client;
  }

  async onModuleDestroy(): Promise<void> {
    await Promise.all([...this.clients.values()].map((client) => client.close()));
    this.clients.clear();
  }
}

@Module({})
export class ZLinkHttpClientModule {
  static forRoot(options: ZLinkHttpClientModuleOptions): DynamicModule {
    validateRegistrations(options.clients);
    const registry: Provider = {
      provide: ZLINK_HTTP_CLIENT_REGISTRY,
      inject: [ModuleRef],
      useFactory: (moduleRef: ModuleRef) =>
        new ZLinkHttpClientRegistry(options.clients, moduleRef)
    };
    const clients: Provider[] = options.clients.map((registration) => ({
      provide: zlinkHttpClientToken(registration.name),
      inject: [ZLINK_HTTP_CLIENT_REGISTRY],
      useFactory: (owner: ZLinkHttpClientRegistry) => owner.get(registration.name.trim())
    }));
    return {
      module: ZLinkHttpClientModule,
      imports: options.imports,
      providers: [registry, ...clients],
      exports: clients.map((provider) => providerToken(provider))
    };
  }
}

function validateRegistrations(registrations: readonly ZLinkNamedHttpClientOptions[]): void {
  const names = new Set<string>();
  for (const registration of registrations) {
    const name = registration.name.trim();
    zlinkHttpClientToken(name);
    if (names.has(name)) {
      throw new framework.ZLinkConfigurationException(`HTTP client '${name}' is already registered.`);
    }
    names.add(name);
  }
}

function providerToken(provider: Provider): InjectionToken {
  return typeof provider === 'function' ? provider : provider.provide;
}
