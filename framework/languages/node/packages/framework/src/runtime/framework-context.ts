import { AsyncLocalStorage } from 'node:async_hooks';

type FrameworkContext = Readonly<Record<symbol, unknown>>;

const storage = new AsyncLocalStorage<FrameworkContext | undefined>();

/**
 * A module owns its field; only the immutable carrier is shared. Values retain
 * their owner's lifetime semantics (for example, a terminal Actor claim).
 */
export class FrameworkContextField<T> {
  private readonly key = Symbol();

  getStore(): T | undefined {
    return storage.getStore()?.[this.key] as T | undefined;
  }

  run<TResult, TArgs extends unknown[]>(
    value: T | undefined,
    callback: (...args: TArgs) => TResult,
    ...args: TArgs
  ): TResult {
    return storage.run(this.withValue(value), callback, ...args);
  }

  enterWith(value: T | undefined): FrameworkContext | undefined {
    const parent = storage.getStore();
    storage.enterWith(this.withValue(value, parent));
    return parent;
  }

  restore(parent: FrameworkContext | undefined): void {
    storage.enterWith(parent);
  }

  exit<TResult>(callback: () => TResult): TResult {
    return this.run(undefined, callback);
  }

  private withValue(
    value: T | undefined,
    parent = storage.getStore()
  ): FrameworkContext | undefined {
    return parent?.[this.key] === value ? parent : Object.freeze({ ...parent, [this.key]: value });
  }
}
