import type { InjectionToken } from '@nestjs/common';
import type { DiscoveryService, ModuleRef } from '@nestjs/core';
import type { Type } from '@zlink-systems/framework';
import { framework } from './framework-loader';
import { isAutoDiscoveredProvider } from './auto-discovery-marker';
import {
  nestHandlerMetadataEntries,
  nestSpotActorHandlerMetadataEntries,
  nestSpotHandlerMetadataEntries,
  nestSpotTimerHandlerMetadataEntries,
  readNestHandlerMetadata,
  readNestSpotActorHandlerMetadata,
  readNestSpotHandlerMetadata,
  readNestSpotTimerHandlerMetadata,
  type ZLinkNestHandlerMetadata,
  type ZLinkNestSpotActorHandlerMetadata,
  type ZLinkNestSpotHandlerMetadata,
  type ZLinkNestSpotTimerHandlerMetadata
} from './handler-metadata';

export interface DiscoveredNestProvider {
  readonly handlerKey: InjectionToken;
  readonly handlerName: string;
  readonly token: InjectionToken;
  readonly metadata: ZLinkNestHandlerMetadata;
}

export interface DiscoveredNestSpotActorProvider {
  readonly handlerKey: Type;
  readonly handlerName: string;
  readonly token: InjectionToken;
  readonly metadata: ZLinkNestSpotActorHandlerMetadata;
}

export interface DiscoveredNestSpotProvider {
  readonly handlerKey: Type;
  readonly handlerName: string;
  readonly token: InjectionToken;
  readonly metadata: ZLinkNestSpotHandlerMetadata;
}

export interface DiscoveredNestSpotTimerProvider {
  readonly handlerKey: Type;
  readonly handlerName: string;
  readonly token: InjectionToken;
  readonly metadata: ZLinkNestSpotTimerHandlerMetadata;
}

export interface DiscoveredNestSessionProvider {
  readonly handlerKey: Type;
}

export function discoverSessionProviderRefs(
  discovery: DiscoveryService,
  moduleRef: ModuleRef
): DiscoveredNestSessionProvider[] {
  const refs: DiscoveredNestSessionProvider[] = [];
  const seen = new Set<Type>();
  const metadataSymbol = Symbol.for('@zlink-systems/framework:decorator');
  for (const wrapper of discovery.getProviders()) {
    const handlerType = wrapper.metatype as Type | undefined;
    if (
      typeof handlerType !== 'function'
      || seen.has(handlerType)
      || !isAutoDiscoveredProvider(handlerType)
    ) {
      continue;
    }
    //  The symbol is only present on decorated handlers, so the lookup has to
    //  admit undefined for the fallback to mean anything.
    const metadata = (handlerType as unknown as
      Record<symbol, readonly { readonly kind?: string }[] | undefined>
    )[metadataSymbol] ?? [];
    if (
      metadata.some((entry) => entry.kind === 'packet')
      && typeof (handlerType as { prototype?: { handle?: unknown } }).prototype?.handle === 'function'
      && tryGetProviderInstance(moduleRef, handlerType) !== undefined
    ) {
      seen.add(handlerType);
      refs.push({ handlerKey: handlerType });
    }
  }
  return refs;
}

export function discoverProviderRefs(discovery: DiscoveryService, moduleRef: ModuleRef): DiscoveredNestProvider[] {
  return discoverDecoratedProviderRefs({
    discovery,
    moduleRef,
    metadataEntries: nestHandlerMetadataEntries(),
    readMetadata: readNestHandlerMetadata,
    metadataKey: (metadata) => `${metadata.groupName}:${metadata.kind}:${metadata.packetName}`,
    createRef: ({ handlerKey, handlerName, token, metadata }) => ({
      handlerKey,
      handlerName,
      token,
      metadata
    })
  });
}

export function discoverSpotProviderRefs(discovery: DiscoveryService, moduleRef: ModuleRef): DiscoveredNestSpotProvider[] {
  return discoverDecoratedClassProviderRefs({
    discovery,
    moduleRef,
    metadataEntries: nestSpotHandlerMetadataEntries(),
    readMetadata: readNestSpotHandlerMetadata,
    metadataKey: (metadata) => `${metadata.kind}:${metadata.packetName ?? metadata.topic ?? ''}`,
    invalidClassMessage: 'ZLink SPOT handler decorators must be applied to class providers.',
    createRef: ({ handlerKey, handlerName, token, metadata }) => ({
      handlerKey,
      handlerName,
      token,
      metadata
    })
  });
}

export function discoverSpotActorProviderRefs(discovery: DiscoveryService, moduleRef: ModuleRef): DiscoveredNestSpotActorProvider[] {
  return discoverDecoratedClassProviderRefs({
    discovery,
    moduleRef,
    metadataEntries: nestSpotActorHandlerMetadataEntries(),
    readMetadata: readNestSpotActorHandlerMetadata,
    metadataKey: (metadata) => `${metadata.kind}:${metadata.packetName}`,
    invalidClassMessage: 'ZLink SPOT actor handler decorators must be applied to class providers.',
    createRef: ({ handlerKey, handlerName, token, metadata }) => ({
      handlerKey,
      handlerName,
      token,
      metadata
    })
  });
}

export function discoverSpotTimerProviderRefs(discovery: DiscoveryService, moduleRef: ModuleRef): DiscoveredNestSpotTimerProvider[] {
  return discoverDecoratedClassProviderRefs({
    discovery,
    moduleRef,
    metadataEntries: nestSpotTimerHandlerMetadataEntries(),
    readMetadata: readNestSpotTimerHandlerMetadata,
    metadataKey: (metadata) => metadata.name,
    invalidClassMessage: 'ZLink SPOT timer handler decorators must be applied to class providers.',
    createRef: ({ handlerKey, handlerName, token, metadata }) => ({
      handlerKey,
      handlerName,
      token,
      metadata
    })
  });
}

interface DecoratedProviderRefInput<TMetadata, THandlerKey extends InjectionToken = InjectionToken> {
  readonly handlerKey: THandlerKey;
  readonly handlerName: string;
  readonly token: InjectionToken;
  readonly instance?: Record<string, unknown>;
  readonly metadata: TMetadata;
}

interface DecoratedProviderDiscoveryOptions<TMetadata, TRef> {
  readonly discovery: DiscoveryService;
  readonly moduleRef: ModuleRef;
  readonly metadataEntries: readonly (readonly [InjectionToken, readonly TMetadata[]])[];
  readonly readMetadata: (handlerToken: InjectionToken | undefined) => readonly TMetadata[];
  readonly metadataKey: (metadata: TMetadata) => string;
  readonly createRef: (input: DecoratedProviderRefInput<TMetadata>) => TRef;
}

interface DecoratedClassProviderDiscoveryOptions<TMetadata, TRef> extends Omit<
  DecoratedProviderDiscoveryOptions<TMetadata, TRef>,
  'createRef'
> {
  readonly invalidClassMessage: string;
  readonly createRef: (input: DecoratedProviderRefInput<TMetadata, Type>) => TRef;
}

function discoverDecoratedProviderRefs<TMetadata, TRef>(
  options: DecoratedProviderDiscoveryOptions<TMetadata, TRef>
): TRef[] {
  const refs: TRef[] = [];
  const seen = new Set<string>();
  for (const wrapper of options.discovery.getProviders()) {
    const token = wrapper.token as InjectionToken | undefined;
    if (token === undefined) {
      continue;
    }
    const instance = wrapper.instance === undefined ? undefined : wrapper.instance as Record<string, unknown>;
    for (const handlerKey of providerMetadataCandidateTokens(wrapper.metatype, instance, token)) {
      appendDiscoveredProviderRefs(refs, seen, {
        handlerKey,
        token,
        instance,
        metadataList: options.readMetadata(handlerKey),
        metadataKey: options.metadataKey,
        createRef: options.createRef
      });
    }
  }

  for (const [handlerKey, metadataList] of options.metadataEntries) {
    const instance = tryGetProviderInstance(options.moduleRef, handlerKey);
    if (instance === undefined) {
      continue;
    }
    appendDiscoveredProviderRefs(refs, seen, {
      handlerKey,
      token: handlerKey,
      instance,
      metadataList,
      metadataKey: options.metadataKey,
      createRef: options.createRef
    });
  }
  return refs;
}

function discoverDecoratedClassProviderRefs<TMetadata, TRef>(
  options: DecoratedClassProviderDiscoveryOptions<TMetadata, TRef>
): TRef[] {
  return discoverDecoratedProviderRefs({
    ...options,
    createRef: (input) => {
      if (typeof input.handlerKey !== 'function') {
        throw new framework.ZLinkConfigurationException(options.invalidClassMessage);
      }
      return options.createRef({
        ...input,
        handlerKey: input.handlerKey as Type
      });
    }
  });
}

interface AppendDiscoveredProviderRefsOptions<TMetadata, TRef> {
  readonly handlerKey: InjectionToken;
  readonly token: InjectionToken;
  readonly instance?: Record<string, unknown>;
  readonly metadataList: readonly TMetadata[];
  readonly metadataKey: (metadata: TMetadata) => string;
  readonly createRef: (input: DecoratedProviderRefInput<TMetadata>) => TRef;
}

function appendDiscoveredProviderRefs<TMetadata, TRef>(
  refs: TRef[],
  seen: Set<string>,
  options: AppendDiscoveredProviderRefsOptions<TMetadata, TRef>
): void {
  const handlerName = handlerKeyName(options.handlerKey);
  for (const metadata of options.metadataList) {
    const key = `${String(options.token)}:${handlerName}:${options.metadataKey(metadata)}`;
    if (seen.has(key)) {
      continue;
    }
    seen.add(key);
    refs.push(options.createRef({
      handlerKey: options.handlerKey,
      handlerName,
      token: options.token,
      instance: options.instance,
      metadata
    }));
  }
}

function providerMetadataCandidateTokens(
  metatype: unknown,
  instance: Record<string, unknown> | undefined,
  token: InjectionToken
): readonly InjectionToken[] {
  return [...new Set([metatype, instance?.constructor, token].filter(isInjectionToken))];
}

function isInjectionToken(value: unknown): value is InjectionToken {
  return typeof value === 'function' || typeof value === 'string' || typeof value === 'symbol';
}

function tryGetProviderInstance(moduleRef: ModuleRef, token: InjectionToken): Record<string, unknown> | undefined {
  try {
    return moduleRef.get(token, { strict: false }) as Record<string, unknown>;
  } catch {
    return undefined;
  }
}

function handlerKeyName(handlerKey: InjectionToken): string {
  if (typeof handlerKey === 'function') {
    return handlerKey.name;
  }
  if (typeof handlerKey === 'symbol') {
    return handlerKey.description ?? handlerKey.toString();
  }
  return handlerKey;
}
