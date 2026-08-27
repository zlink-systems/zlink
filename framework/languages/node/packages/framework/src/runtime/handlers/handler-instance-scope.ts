import { AsyncLocalStorage, AsyncResource } from 'node:async_hooks';
import type { Type, ZLinkMessageContext } from '../../contracts';
import type { ZLinkProviderResolver } from '../../contracts/Common/ZLinkProviderResolver';
import { ZLinkStateLane } from '../execution/state-lane';

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

interface HandlerResolution<T> {
  readonly promise: Promise<T>;
  readonly activation?: Deferred<unknown>;
}

interface HandlerScopeDisposal {
  readonly pending: readonly Promise<unknown>[];
  readonly owned: readonly unknown[];
}

interface LifecycleScopeDisposal {
  readonly completion: Promise<void>;
  readonly deferred?: Deferred<void>;
  readonly idle: Promise<void> | undefined;
}

const HANDLER_SCOPE_FACTORY = Symbol.for(
  '@zlink-systems/framework.handler-instance-scope-factory'
);
const activeHandlerScope = new AsyncLocalStorage<ActiveHandlerScope>();
const activeLifecycleScope =
  new AsyncLocalStorage<LifecycleHandlerInstanceScope>();
const detachedStateLaneResource = new AsyncResource('zlink:handler-instance-scope');
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
  private readonly lane = new ZLinkStateLane();
  private readonly instances = new Map<Type, Promise<unknown>>();
  private readonly owned: unknown[] = [];
  private disposed = false;

  constructor(private readonly providerResolver?: ZLinkProviderResolver) {}

  async resolve<T>(type: Type<T>): Promise<T> {
    const resolution = await this.lane.run(() => this.resolveCore(type));
    if (resolution.activation !== undefined) {
      startOutsideStateLane(() => {
        void this.activate(type, resolution.activation!);
      });
    }
    return await resolution.promise;
  }

  async dispose(): Promise<void> {
    const disposal = await this.lane.run(() => this.beginDisposeCore());
    if (disposal === undefined) return;

    await Promise.allSettled(disposal.pending);
    for (const instance of disposal.owned) {
      await disposeOwnedInstance(instance);
    }
    await this.lane.run(() => this.completeDisposeCore());
  }

  private resolveCore<T>(type: Type<T>): HandlerResolution<T> {
    if (this.disposed) {
      throw new Error('Handler instance scope is already disposed.');
    }
    let instance = this.instances.get(type) as Promise<T> | undefined;
    if (instance !== undefined) return { promise: instance };

    const activation = createDeferred<unknown>();
    instance = activation.promise as Promise<T>;
    this.instances.set(type, activation.promise);
    return { promise: instance, activation };
  }

  private beginDisposeCore(): HandlerScopeDisposal | undefined {
    if (this.disposed) return undefined;
    this.disposed = true;
    return {
      pending: [...this.instances.values()],
      owned: [...this.owned].reverse()
    };
  }

  private completeDisposeCore(): void {
    this.owned.length = 0;
    this.instances.clear();
  }

  private async activate<T>(type: Type<T>, activation: Deferred<unknown>): Promise<void> {
    let instance: T;
    try {
      instance = await this.providerResolver?.create?.(type)
        ?? new type();
    } catch (error) {
      activation.reject(error);
      return;
    }

    const accepted = await this.lane.run(() => this.acceptActivationCore(instance));
    if (accepted) {
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

  private acceptActivationCore(instance: unknown): boolean {
    if (this.disposed) return false;
    this.owned.push(instance);
    return true;
  }
}

class LifecycleHandlerInstanceScope {
  private readonly lane = new ZLinkStateLane();
  private activeInvocations = 0;
  private closing = false;
  private idle?: Promise<void>;
  private resolveIdle?: () => void;
  private disposal?: Promise<void>;

  constructor(private readonly instances: ZLinkHandlerInstanceScope) {}

  async resolve<T>(type: Type<T>): Promise<T> {
    const resolution = await this.lane.run(() => ({ promise: this.resolveCore(type) }));
    return await resolution.promise;
  }

  private resolveCore<T>(type: Type<T>): Promise<T> {
    this.throwIfClosingCore();
    return this.instances.resolve(type);
  }

  async run<THandler, TResult>(
    type: Type<THandler>,
    callback: (handler: THandler) => Promise<TResult>
  ): Promise<TResult> {
    await this.lane.run(() => this.beginInvocationCore());
    try {
      return await activeLifecycleScope.run(
        this,
        async () => callback(await this.instances.resolve(type))
      );
    } finally {
      await this.lane.run(() => this.completeInvocationCore());
    }
  }

  async dispose(): Promise<void> {
    const disposeFromActiveInvocation = activeLifecycleScope.getStore() === this;
    const disposal = await this.lane.run(() => this.beginDisposeCore());
    if (disposal.deferred !== undefined) {
      startOutsideStateLane(() => {
        void this.disposeWhenIdle(disposal);
      });
    }
    if (disposeFromActiveInvocation) {
      // Waiting here would make the current handler wait for its own terminal
      // completion. The same disposal promise continues after run() releases
      // the final active invocation.
      void disposal.completion.catch(() => undefined);
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
      await this.lane.run(() => this.failDisposalCore(deferred.promise));
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

function startOutsideStateLane<T>(work: () => T): T {
  return detachedStateLaneResource.runInAsyncScope(work);
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
