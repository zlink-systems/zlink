import type {
  ZLinkLocationOptions,
  ZLinkLocationStore,
  ZLinkStoreScanRequest,
  ZLinkStoreScanResult
} from '@zlink-systems/framework';
import { ZLinkRedisLocationStore } from '@zlink-systems/framework-locations-redis';

export interface RedisLocationOptions {
  readonly redisEndpoint: string;
  readonly redisKeyPrefix: string;
}

export interface StoreResponseGateSnapshot {
  readonly open: boolean;
  readonly waiting: number;
}

export interface StoreResponseGate {
  close(): void;
  open(): void;
  snapshot(): StoreResponseGateSnapshot;
  wait(signal?: AbortSignal): Promise<void>;
}

class GatedLocationStore implements ZLinkLocationStore {
  public constructor(
    private readonly inner: ZLinkLocationStore,
    private readonly responseGate: StoreResponseGate
  ) {}

  read(...args: Parameters<ZLinkLocationStore['read']>): ReturnType<ZLinkLocationStore['read']> {
    return this.inner.read(...args);
  }

  write(...args: Parameters<ZLinkLocationStore['write']>): ReturnType<ZLinkLocationStore['write']> {
    return this.inner.write(...args);
  }

  async scan(
    request: ZLinkStoreScanRequest,
    signal?: AbortSignal
  ): Promise<ZLinkStoreScanResult> {
    await this.responseGate.wait(signal);
    return await this.inner.scan(request, signal);
  }

  async dispose(): Promise<void> {
    await this.inner.dispose?.();
  }
}

class ApplicationStoreResponseGate implements StoreResponseGate {
  private isOpen = true;
  private readonly waiters = new Set<() => void>();

  close(): void {
    this.isOpen = false;
  }

  open(): void {
    this.isOpen = true;
    for (const resolve of this.waiters) resolve();
    this.waiters.clear();
  }

  snapshot(): StoreResponseGateSnapshot {
    return { open: this.isOpen, waiting: this.waiters.size };
  }

  async wait(signal?: AbortSignal): Promise<void> {
    if (this.isOpen) return;
    if (signal?.aborted) throw new DOMException('The Store response gate wait was aborted.', 'AbortError');
    await new Promise<void>((resolve, reject) => {
      const release = (): void => {
        signal?.removeEventListener('abort', abort);
        this.waiters.delete(release);
        resolve();
      };
      const abort = (): void => {
        this.waiters.delete(release);
        reject(new DOMException('The Store response gate wait was aborted.', 'AbortError'));
      };
      this.waiters.add(release);
      signal?.addEventListener('abort', abort, { once: true });
    });
  }
}

export function createStoreResponseGate(): StoreResponseGate {
  return new ApplicationStoreResponseGate();
}

export function createRedisLocationStore(options: RedisLocationOptions): ZLinkRedisLocationStore {
  return new ZLinkRedisLocationStore({
    url: `redis://${options.redisEndpoint}`,
    keyPrefix: options.redisKeyPrefix
  });
}

export function createGatedRedisLocationStore(
  options: RedisLocationOptions,
  responseGate: StoreResponseGate
): ZLinkLocationStore {
  return new GatedLocationStore(createRedisLocationStore(options), responseGate);
}

export function configureStoreFailureLocationOptions(options: ZLinkLocationOptions): void {
  options
    .pollingIntervalMs(100)
    .ownerLeaseRenewIntervalMs(1000)
    .ownerLeaseTtlMs(3000)
    .storeFailureGraceMs(6000);
}
