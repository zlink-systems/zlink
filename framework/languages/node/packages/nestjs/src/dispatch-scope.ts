import { AsyncLocalStorage } from 'node:async_hooks';
import type { Type, ZLinkMessageContext } from '@zlink-systems/framework';
import type { ContextId, ModuleRef } from '@nestjs/core';
import { ContextIdFactory } from '@nestjs/core';

const dispatchContext = new AsyncLocalStorage<ContextId>();

export async function runInNestDispatchScope<T>(
  moduleRef: ModuleRef,
  context: ZLinkMessageContext,
  callback: () => Promise<T>
): Promise<T> {
  const contextId = ContextIdFactory.create();
  moduleRef.registerRequestByContextId({ zlinkContext: context }, contextId);
  return dispatchContext.run(contextId, callback);
}

export async function resolveInNestDispatchScope<T>(
  moduleRef: ModuleRef,
  type: Type<T>
): Promise<T> {
  return await moduleRef.resolve(type, dispatchContext.getStore(), { strict: false });
}

export function currentNestDispatchContext(): ContextId | undefined {
  return dispatchContext.getStore();
}
