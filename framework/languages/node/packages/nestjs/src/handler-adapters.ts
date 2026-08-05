import type { InjectionToken } from '@nestjs/common';
import { ContextIdFactory, ModuleRef } from '@nestjs/core';
import type {
  Type,
  ZLinkMessageContext,
  ZLinkPublishMessageContext,
  ZLinkRouteMessageContext
} from '@zlink-systems/framework';
import type { ZLinkChannelOptions } from './framework-integration-contracts';
import { framework } from './framework-loader';
import type { ZLinkNestManualHandlerOptions } from './contracts';
import type { ZLinkNestHandlerMetadata } from './handler-metadata';
import type { DiscoveredNestProvider } from './provider-discovery';
import { currentNestDispatchContext } from './dispatch-scope';
import {
  createNestHandlerInstance,
  disposeNestOwnedHandler
} from './providers';

export function createDiscoveredRequestHandlers(
  providerRefs: readonly DiscoveredNestProvider[],
  handlerGroups: readonly string[] | undefined,
  moduleRef: ModuleRef
): NonNullable<ZLinkChannelOptions['requestHandlers']> {
  return createDiscoveredHandlerRegistrations(providerRefs, handlerGroups, 'request', (ref, metadata) => ({
    async handle(payload: Buffer, context: ZLinkMessageContext) {
      const result = await invokeDiscoveredHandler(moduleRef, ref, metadata, payload, context);
      return metadata.encodeResult === undefined ? result : metadata.encodeResult(result, context);
    }
  }));
}

export function createDiscoveredSendHandlers(
  providerRefs: readonly DiscoveredNestProvider[],
  handlerGroups: readonly string[] | undefined,
  moduleRef: ModuleRef
): NonNullable<NonNullable<ZLinkChannelOptions['routeMesh']>['sendHandlers']> {
  return createDiscoveredHandlerRegistrations(providerRefs, handlerGroups, 'send', (ref, metadata) => ({
    async handle(payload: Buffer, context: ZLinkRouteMessageContext) {
      await invokeDiscoveredHandler(moduleRef, ref, metadata, payload, context);
    }
  }));
}

export function createDiscoveredChannelSendHandlers(
  providerRefs: readonly DiscoveredNestProvider[],
  handlerGroups: readonly string[] | undefined,
  moduleRef: ModuleRef
): NonNullable<ZLinkChannelOptions['sendHandlers']> {
  return createDiscoveredHandlerRegistrations(providerRefs, handlerGroups, 'send', (ref, metadata) => ({
    async handle(payload: Buffer, context: ZLinkMessageContext) {
      await invokeDiscoveredHandler(moduleRef, ref, metadata, payload, context);
    }
  }));
}

export function createDiscoveredPublishHandlers(
  providerRefs: readonly DiscoveredNestProvider[],
  handlerGroups: readonly string[] | undefined,
  moduleRef: ModuleRef
): NonNullable<ZLinkChannelOptions['publishHandlers']> {
  return createDiscoveredHandlerRegistrations(providerRefs, handlerGroups, 'publish', (ref, metadata) => ({
    async handle(payload: Buffer, context: ZLinkPublishMessageContext) {
      await invokeDiscoveredHandler(moduleRef, ref, metadata, payload, context);
    }
  }));
}

export function createManualRequestHandlers(
  handlerTypes: readonly ZLinkNestManualHandlerOptions[] | undefined,
  moduleRef: ModuleRef
): NonNullable<ZLinkChannelOptions['requestHandlers']> {
  return createManualHandlerRegistrations<ZLinkMessageContext, unknown>(handlerTypes, moduleRef, (result) => result);
}

export function createManualPublishHandlers(
  handlerTypes: readonly ZLinkNestManualHandlerOptions[] | undefined,
  moduleRef: ModuleRef
): NonNullable<ZLinkChannelOptions['publishHandlers']> {
  return createManualHandlerRegistrations<ZLinkPublishMessageContext, void>(handlerTypes, moduleRef, () => undefined);
}

export function createManualSendHandlers(
  handlerTypes: readonly ZLinkNestManualHandlerOptions[] | undefined,
  moduleRef: ModuleRef
): NonNullable<ZLinkChannelOptions['sendHandlers']> {
  return createManualHandlerRegistrations<ZLinkMessageContext, void>(handlerTypes, moduleRef, () => undefined);
}

export function createManualRouteSendHandlers(
  handlerTypes: readonly ZLinkNestManualHandlerOptions[] | undefined,
  moduleRef: ModuleRef
): NonNullable<NonNullable<ZLinkChannelOptions['routeMesh']>['sendHandlers']> {
  return createManualHandlerRegistrations<ZLinkRouteMessageContext, void>(handlerTypes, moduleRef, () => undefined);
}

export function createManualRouteRequestHandlers(
  handlerTypes: readonly ZLinkNestManualHandlerOptions[] | undefined,
  moduleRef: ModuleRef
): NonNullable<NonNullable<ZLinkChannelOptions['routeMesh']>['requestHandlers']> {
  return createManualHandlerRegistrations<ZLinkRouteMessageContext, unknown>(handlerTypes, moduleRef, (result) => result);
}

type ManualHandlerContext =
  | ZLinkMessageContext
  | ZLinkRouteMessageContext
  | ZLinkPublishMessageContext;

function createManualHandlerRegistrations<TContext extends ManualHandlerContext, TResult>(
  handlerTypes: readonly ZLinkNestManualHandlerOptions[] | undefined,
  moduleRef: ModuleRef,
  result: (value: unknown) => TResult
): Array<{
  readonly packetName: string;
  readonly handler: {
    handle(payload: Buffer, context: TContext): Promise<TResult>;
  };
}> {
  return (handlerTypes ?? []).map((registration) => ({
    packetName: registration.packetName,
    handler: {
      async handle(payload: Buffer, context: TContext): Promise<TResult> {
        return result(await invokeManualHandler(moduleRef, registration.handlerType, payload, context));
      }
    }
  }));
}

function createDiscoveredHandlerRegistrations<THandler>(
  providerRefs: readonly DiscoveredNestProvider[],
  handlerGroups: readonly string[] | undefined,
  kind: string,
  createHandler: (ref: DiscoveredNestProvider, metadata: ZLinkNestHandlerMetadata) => THandler
): Array<{ readonly packetName: string; readonly handler: THandler }> {
  const descriptors = createDiscoveredHandlerDescriptors(providerRefs, handlerGroups, kind);
  return descriptors.map(({ ref, metadata }) => ({
    packetName: metadata.packetName,
    handler: createHandler(ref, metadata)
  }));
}

function createDiscoveredHandlerDescriptors(
  providerRefs: readonly DiscoveredNestProvider[],
  handlerGroups: readonly string[] | undefined,
  kind: string
): Array<{ readonly ref: DiscoveredNestProvider; readonly metadata: ZLinkNestHandlerMetadata }> {
  if ((handlerGroups ?? []).length === 0) {
    return [];
  }
  const groups = new Set(handlerGroups);
  const seen = new Map<string, InjectionToken>();
  const selected: Array<{ readonly ref: DiscoveredNestProvider; readonly metadata: ZLinkNestHandlerMetadata }> = [];
  for (const ref of providerRefs) {
    const metadata = ref.metadata;
    if (metadata.kind !== kind || !groups.has(metadata.groupName)) {
      continue;
    }
    const key = `${metadata.kind}:${metadata.packetName}`;
    const previousType = seen.get(key);
    if (previousType === ref.handlerKey) {
      continue;
    }
    if (previousType !== undefined) {
      throw new framework.ZLinkConfigurationException(
        `Duplicate handler '${metadata.groupName}:${metadata.kind}:${metadata.packetName}'.`
      );
    }
    seen.set(key, ref.handlerKey);
    selected.push({ ref, metadata });
  }
  return selected;
}

async function invokeDiscoveredHandler(
  moduleRef: ModuleRef,
  ref: DiscoveredNestProvider,
  metadata: ZLinkNestHandlerMetadata,
  payload: Buffer,
  context: ManualHandlerContext
): Promise<unknown> {
  const instance = await resolveHandlerInstance(moduleRef, ref.handlerKey as Type, context);
  try {
    const method = instance[metadata.methodName];
    if (typeof method !== 'function') {
      throw new framework.ZLinkConfigurationException(
        `Discovered handler ${ref.handlerName}.${metadata.methodName} is not callable.`
      );
    }
    return await method.call(instance, decodePayload(metadata, payload, context), context);
  } finally {
    await disposeNestOwnedHandler(instance);
  }
}

async function invokeManualHandler(
  moduleRef: ModuleRef,
  handlerType: Type,
  payload: Buffer,
  context: ManualHandlerContext
): Promise<unknown> {
  const instance = await resolveHandlerInstance(moduleRef, handlerType, context);
  try {
    const method = instance.handle;
    if (typeof method !== 'function') {
      throw new framework.ZLinkConfigurationException(
        `Manual handler ${handlerType.name}.handle is not callable.`
      );
    }
    return await method.call(instance, decodePayload(undefined, payload, context), context);
  } finally {
    await disposeNestOwnedHandler(instance);
  }
}

async function resolveHandlerInstance(
  moduleRef: ModuleRef,
  handlerType: Type,
  context: ManualHandlerContext
): Promise<Record<string, unknown>> {
  const currentContextId = currentNestDispatchContext();
  const contextId = currentContextId ?? ContextIdFactory.create();
  if (currentContextId === undefined) {
    moduleRef.registerRequestByContextId({ zlinkContext: context }, contextId);
  }
  return await createNestHandlerInstance(
    moduleRef,
    contextId,
    new Map(),
    handlerType
  ) as Record<string, unknown>;
}

function decodePayload(
  metadata: ZLinkNestHandlerMetadata | undefined,
  payload: Buffer | Uint8Array | string | unknown,
  context: ManualHandlerContext
): unknown {
  if (Buffer.isBuffer(payload) || payload instanceof Uint8Array) {
    if (metadata?.decodePayload !== undefined) {
      return metadata.decodePayload(Buffer.from(payload), context);
    }
    if (context.contentType !== undefined && context.contentType !== 'application/json') {
      return Buffer.from(payload);
    }
    return parseWireJson(Buffer.from(payload).toString());
  }
  return typeof payload === 'string' ? parseWireJson(payload) : payload;
}

function parseWireJson(payload: string): unknown {
  return JSON.parse(payload, (key, value) => {
    if (key === '__proto__' || key === 'constructor' || key === 'prototype') {
      throw new framework.ZLinkConfigurationException(`NestJS handler JSON key '${key}' is not allowed.`);
    }
    return value;
  });
}
