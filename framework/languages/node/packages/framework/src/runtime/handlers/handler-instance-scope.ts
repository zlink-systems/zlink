import { AsyncLocalStorage } from 'node:async_hooks';
import type { Type, ZLinkMessageContext } from '../../contracts';
import type { ZLinkProviderResolver } from '../../contracts/Common/ZLinkProviderResolver';

export interface ZLinkHandlerInstanceScope {
  resolve<T>(type: Type<T>): Promise<T>;
  dispose(): Promise<void>;
}

interface ZLinkHandlerInstanceScopeFactory {
  create(context?: ZLinkMessageContext): ZLinkHandlerInstanceScope;
}

interface ActiveHandlerScope {
  readonly scope: ZLinkHandlerInstanceScope;
}

const HANDLER_SCOPE_FACTORY = Symbol.for(
  '@zlink-systems/framework.handler-instance-scope-factory'
);
const activeHandlerScope = new AsyncLocalStorage<ActiveHandlerScope>();
const activeLifecycleScope =
  new AsyncLocalStorage<LifecycleHandlerInstanceScope>();
const dispatchScopes = new WeakMap<object, ZLinkHandlerInstanceScope>();
const lifecycleScopes = new WeakMap<object, Promise<LifecycleHandlerInstanceScope>>();

export async function runInHandlerInstanceScope<T>(
  providerResolver: ZLinkProviderResolver | undefined,
  context: ZLinkMessageContext | undefined,
  callback: (scope: ZLinkHandlerInstanceScope) => Promise<T>
): Promise<T> {
  const active = activeHandlerScope.getStore();
  if (active !== undefined) {
    return callback(active.scope);
  }
  if (context !== undefined) {
    const existing = dispatchScopes.get(context);
    if (existing !== undefined) {
      return callback(existing);
    }
  }

  const scope = createHandlerInstanceScope(providerResolver, context);
  if (context !== undefined) {
    dispatchScopes.set(context, scope);
  }
  try {
    return await activeHandlerScope.run(
      { scope },
      () => callback(scope)
    );
  } finally {
    if (context !== undefined && dispatchScopes.get(context) === scope) {
      dispatchScopes.delete(context);
    }
    await scope.dispose();
  }
}

export async function resolveLifecycleHandler<T>(
  owner: object,
  type: Type<T>,
  providerResolver?: ZLinkProviderResolver
): Promise<T> {
  let scope = lifecycleScopes.get(owner);
  if (scope === undefined) {
    scope = Promise.resolve(
      new LifecycleHandlerInstanceScope(
        createHandlerInstanceScope(providerResolver)
      )
    );
    lifecycleScopes.set(owner, scope);
  }
  return (await scope).resolve(type);
}

export async function runWithLifecycleHandler<THandler, TResult>(
  owner: object,
  type: Type<THandler>,
  providerResolver: ZLinkProviderResolver | undefined,
  callback: (handler: THandler) => Promise<TResult>
): Promise<TResult> {
  let scope = lifecycleScopes.get(owner);
  if (scope === undefined) {
    scope = Promise.resolve(
      new LifecycleHandlerInstanceScope(
        createHandlerInstanceScope(providerResolver)
      )
    );
    lifecycleScopes.set(owner, scope);
  }
  return (await scope).run(type, callback);
}

export async function disposeLifecycleHandlers(owner: object): Promise<void> {
  const scope = lifecycleScopes.get(owner);
  if (scope === undefined) return;
  const resolved = await scope;
  await resolved.dispose();
  // Keep the closed scope as a tombstone for the lifetime of the owner object.
  // A late dispatch must fail instead of creating a second activation.
}

function createHandlerInstanceScope(
  providerResolver?: ZLinkProviderResolver,
  context?: ZLinkMessageContext
): ZLinkHandlerInstanceScope {
  const factory = providerResolver === undefined
    ? undefined
    : (providerResolver as unknown as Record<PropertyKey, unknown>)[HANDLER_SCOPE_FACTORY];
  if (isHandlerInstanceScopeFactory(factory)) {
    return factory.create(context);
  }
  return new DefaultHandlerInstanceScope(providerResolver);
}

function isHandlerInstanceScopeFactory(
  value: unknown
): value is ZLinkHandlerInstanceScopeFactory {
  return typeof value === 'object'
    && value !== null
    && typeof (value as { create?: unknown }).create === 'function';
}

class DefaultHandlerInstanceScope implements ZLinkHandlerInstanceScope {
  private readonly instances = new Map<Type, Promise<unknown>>();
  private readonly owned: unknown[] = [];
  private disposed = false;

  constructor(private readonly providerResolver?: ZLinkProviderResolver) {}

  async resolve<T>(type: Type<T>): Promise<T> {
    if (this.disposed) {
      throw new Error('Handler instance scope is already disposed.');
    }
    let instance = this.instances.get(type);
    if (instance === undefined) {
      instance = this.create(type);
      this.instances.set(type, instance);
    }
    return await instance as T;
  }

  async dispose(): Promise<void> {
    if (this.disposed) return;
    this.disposed = true;
    const pending = [...this.instances.values()];
    await Promise.allSettled(pending);
    for (let index = this.owned.length - 1; index >= 0; index -= 1) {
      await disposeOwnedInstance(this.owned[index]);
    }
    this.owned.length = 0;
    this.instances.clear();
  }

  private async create<T>(type: Type<T>): Promise<T> {
    const instance = await this.providerResolver?.create?.(type)
      ?? new type();
    if (this.disposed) {
      await disposeOwnedInstance(instance);
      throw new Error('Handler instance scope was disposed during activation.');
    }
    this.owned.push(instance);
    return instance;
  }
}

class LifecycleHandlerInstanceScope {
  private activeInvocations = 0;
  private closing = false;
  private idle?: Promise<void>;
  private resolveIdle?: () => void;
  private disposal?: Promise<void>;

  constructor(private readonly instances: ZLinkHandlerInstanceScope) {}

  resolve<T>(type: Type<T>): Promise<T> {
    if (this.closing) {
      return Promise.reject(new Error('Handler lifecycle scope is closing.'));
    }
    return this.instances.resolve(type);
  }

  async run<THandler, TResult>(
    type: Type<THandler>,
    callback: (handler: THandler) => Promise<TResult>
  ): Promise<TResult> {
    if (this.closing) {
      throw new Error('Handler lifecycle scope is closing.');
    }
    this.activeInvocations += 1;
    try {
      return await activeLifecycleScope.run(
        this,
        async () => callback(await this.instances.resolve(type))
      );
    } finally {
      this.activeInvocations -= 1;
      if (this.activeInvocations === 0) {
        this.resolveIdle?.();
        this.resolveIdle = undefined;
        this.idle = undefined;
      }
    }
  }

  async dispose(): Promise<void> {
    if (!this.closing) {
      this.closing = true;
      if (this.activeInvocations > 0) {
        this.idle = new Promise((resolve) => {
          this.resolveIdle = resolve;
        });
      }
    }
    const attempt = this.disposal ??= this.disposeWhenIdle();
    if (activeLifecycleScope.getStore() === this) {
      // Waiting here would make the current handler wait for its own terminal
      // completion. The same disposal promise continues after run() releases
      // the final active invocation.
      void attempt.catch(() => {
        if (this.disposal === attempt) {
          this.disposal = undefined;
        }
      });
      return;
    }
    try {
      await attempt;
    } catch (error) {
      if (this.disposal === attempt) {
        this.disposal = undefined;
      }
      throw error;
    }
  }

  private async disposeWhenIdle(): Promise<void> {
    await this.idle;
    await this.instances.dispose();
  }
}

export async function disposeOwnedInstance(instance: unknown): Promise<void> {
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
