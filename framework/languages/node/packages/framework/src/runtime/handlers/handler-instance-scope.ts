import { AsyncLocalStorage, AsyncResource } from 'node:async_hooks';
import type { Type, ZLinkMessageContext } from '../../contracts';
import type { ZLinkProviderResolver } from '../../contracts/Common/ZLinkProviderResolver';
import type { ZLinkDetachedTaskRunner } from '../spots/spot-actor-join-dispatch';

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

interface Deferred<T> {
  readonly promise: Promise<T>;
  readonly resolve: (value: T) => void;
  readonly reject: (error: unknown) => void;
}

interface LifecycleScopeDisposal {
  readonly completion: Promise<void>;
  readonly deferred?: Deferred<void>;
  readonly idle: Promise<void> | undefined;
}

const HANDLER_SCOPE_FACTORY = Symbol.for('@zlink-systems/framework.handler-instance-scope-factory');
const activeHandlerScope = new AsyncLocalStorage<ActiveHandlerScope>();
const activeLifecycleScope = new AsyncLocalStorage<LifecycleHandlerInstanceScope>();
const detachedLifecycleResource = new AsyncResource('zlink:handler-instance-scope');
const dispatchScopes = new WeakMap<object, ZLinkHandlerInstanceScope>();
const lifecycleScopes = new WeakMap<object, LifecycleHandlerInstanceScope>();

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
    return await activeHandlerScope.run({ scope }, () => callback(scope));
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
  return lifecycleHandlerScope(owner, providerResolver).resolve(type);
}

export async function runWithLifecycleHandler<THandler, TResult>(
  owner: object,
  type: Type<THandler>,
  providerResolver: ZLinkProviderResolver | undefined,
  callback: (handler: THandler) => Promise<TResult>
): Promise<TResult> {
  return lifecycleHandlerScope(owner, providerResolver).run(type, callback);
}

function lifecycleHandlerScope(
  owner: object,
  providerResolver?: ZLinkProviderResolver
): LifecycleHandlerInstanceScope {
  let scope = lifecycleScopes.get(owner);
  if (scope === undefined) {
    scope = new LifecycleHandlerInstanceScope(createHandlerInstanceScope(providerResolver));
    lifecycleScopes.set(owner, scope);
  }
  return scope;
}

export async function disposeLifecycleHandlers(
  owner: object,
  detachedTaskRunner: ZLinkDetachedTaskRunner
): Promise<void> {
  if ((detachedTaskRunner as unknown) === undefined) {
    throw new Error('Lifecycle handler disposal requires a detached task runner.');
  }
  const scope = lifecycleScopes.get(owner);
  if (scope === undefined) return;
  await scope.dispose(detachedTaskRunner);
  // Keep the closed scope as a tombstone for the lifetime of the owner object.
  // A late dispatch must fail instead of creating a second activation.
}
function createHandlerInstanceScope(
  providerResolver?: ZLinkProviderResolver,
  context?: ZLinkMessageContext
): ZLinkHandlerInstanceScope {
  const factory =
    providerResolver === undefined
      ? undefined
      : (providerResolver as unknown as Record<PropertyKey, unknown>)[HANDLER_SCOPE_FACTORY];
  if (isHandlerInstanceScopeFactory(factory)) {
    return factory.create(context);
  }
  return new DefaultHandlerInstanceScope(providerResolver);
}

function isHandlerInstanceScopeFactory(value: unknown): value is ZLinkHandlerInstanceScopeFactory {
  return (
    typeof value === 'object' &&
    value !== null &&
    typeof (value as { create?: unknown }).create === 'function'
  );
}

class DefaultHandlerInstanceScope implements ZLinkHandlerInstanceScope {
  private readonly instances = new Map<Type, Promise<unknown>>();
  private readonly owned: unknown[] = [];
  private disposed = false;

  constructor(private readonly providerResolver?: ZLinkProviderResolver) {}

  resolve<T>(type: Type<T>): Promise<T> {
    if (this.disposed) {
      return Promise.reject(new Error('Handler instance scope is already disposed.'));
    }
    const existing = this.instances.get(type) as Promise<T> | undefined;
    if (existing !== undefined) return existing;
    const activation = createDeferred<unknown>();
    this.instances.set(type, activation.promise);
    void this.activate(type, activation);
    return activation.promise as Promise<T>;
  }

  async dispose(): Promise<void> {
    if (this.disposed) return;
    this.disposed = true;
    await Promise.allSettled(this.instances.values());
    try {
      await disposeOwnedInstances(this.owned);
    } finally {
      this.owned.length = 0;
      this.instances.clear();
    }
  }
  private async activate<T>(type: Type<T>, activation: Deferred<unknown>): Promise<void> {
    let instance: T;
    try {
      const created = this.providerResolver?.create?.(type);
      instance = created === undefined ? new type() : await created;
    } catch (error) {
      activation.reject(error);
      return;
    }

    if (!this.disposed) {
      this.owned.push(instance);
      activation.resolve(instance);
      return;
    }
    try {
      await disposeOwnedInstance(instance);
      activation.reject(new Error('Handler instance scope was disposed during activation.'));
    } catch (error) {
      activation.reject(error);
    }
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
    this.throwIfClosingCore();
    return this.instances.resolve(type);
  }

  async run<THandler, TResult>(
    type: Type<THandler>,
    callback: (handler: THandler) => Promise<TResult>
  ): Promise<TResult> {
    this.beginInvocationCore();
    try {
      return await activeLifecycleScope.run(this, async () =>
        callback(await this.instances.resolve(type))
      );
    } finally {
      this.completeInvocationCore();
    }
  }

  async dispose(detachedTaskRunner: ZLinkDetachedTaskRunner): Promise<void> {
    const disposeFromActiveInvocation = activeLifecycleScope.getStore() === this;
    const disposal = this.beginDisposeCore();
    if (disposal.deferred !== undefined) {
      startOutsideLifecycleInvocation(() => {
        void this.disposeWhenIdle(disposal);
        if (disposeFromActiveInvocation) {
          detachedTaskRunner.runDetached(
            LifecycleHandlerInstanceScope.name,
            () => disposal.completion
          );
        }
      });
    }
    if (disposeFromActiveInvocation) {
      // Waiting here would make the current handler wait for its own terminal
      // completion. The same disposal promise continues after run() releases
      // the final active invocation.
      return;
    }
    await disposal.completion;
  }

  private throwIfClosingCore(): void {
    if (this.closing) {
      throw new Error('Handler lifecycle scope is closing.');
    }
  }

  private beginInvocationCore(): void {
    this.throwIfClosingCore();
    this.activeInvocations += 1;
  }

  private completeInvocationCore(): void {
    this.activeInvocations -= 1;
    if (this.activeInvocations === 0) {
      this.resolveIdle?.();
      this.resolveIdle = undefined;
      this.idle = undefined;
    }
  }

  private beginDisposeCore(): LifecycleScopeDisposal {
    if (!this.closing) {
      this.closing = true;
      if (this.activeInvocations > 0) {
        this.idle = new Promise((resolve) => {
          this.resolveIdle = resolve;
        });
      }
    }
    if (this.disposal !== undefined) {
      return { completion: this.disposal, idle: undefined };
    }
    const deferred = createDeferred<void>();
    this.disposal = deferred.promise;
    return { completion: deferred.promise, deferred, idle: this.idle };
  }

  private async disposeWhenIdle(disposal: LifecycleScopeDisposal): Promise<void> {
    const deferred = disposal.deferred!;
    try {
      await disposal.idle;
      await this.instances.dispose();
      deferred.resolve();
    } catch (error) {
      this.failDisposalCore(deferred.promise);
      deferred.reject(error);
    }
  }

  private failDisposalCore(completion: Promise<void>): void {
    if (this.disposal === completion) {
      this.disposal = undefined;
    }
  }
}

function createDeferred<T>(): Deferred<T> {
  let resolve!: (value: T) => void;
  let reject!: (error: unknown) => void;
  const promise = new Promise<T>((complete, fail) => {
    resolve = complete;
    reject = fail;
  });
  return { promise, resolve, reject };
}

function startOutsideLifecycleInvocation<T>(work: () => T): T {
  return detachedLifecycleResource.runInAsyncScope(work);
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

export async function disposeOwnedInstances(instances: readonly unknown[]): Promise<void> {
  const failures: unknown[] = [];
  for (let index = instances.length - 1; index >= 0; index -= 1) {
    try {
      await disposeOwnedInstance(instances[index]);
    } catch (error) {
      failures.push(error);
    }
  }
  if (failures.length === 1) throw failures[0];
  if (failures.length > 1) throw new AggregateError(failures, 'Handler scope cleanup failed.');
}
