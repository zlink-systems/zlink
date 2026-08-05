import { createClient, RESP_TYPES } from 'redis';
import type { RedisClientType } from 'redis';
import type {
  ZLinkRedisLocationOptions,
  ZLinkRedisRelocationOptions
} from './redis-options';

export type RedisCommandValue = string | Buffer;
export type RedisCommandClient = Pick<
  RedisClientType,
  'isOpen' | 'isReady' | 'connect' | 'sendCommand' | 'quit' | 'on'
>;

export class RedisConnection {
  private readonly providedClient?: RedisCommandClient;
  private client?: RedisCommandClient;
  private connectionAttempt?: Promise<void>;
  private disposed = false;
  private readonly operationTimeoutMs?: number;

  constructor(options: ZLinkRedisLocationOptions | ZLinkRedisRelocationOptions) {
    requireOptions(options);
    this.providedClient = options.client;
    this.operationTimeoutMs = options.operationTimeoutMs;
    if (options.client === undefined) {
      this.client = createClient({
        disableOfflineQueue: true,
        ...(options.clientOptions ?? {}),
        socket: {
          reconnectStrategy: false,
          ...(options.clientOptions?.socket ?? {})
        },
        url: options.url ?? options.clientOptions?.url
      }) as RedisCommandClient;
      this.client.on('error', () => {});
    }
  }

  async command(args: RedisCommandValue[], signal?: AbortSignal): Promise<unknown> {
    signal?.throwIfAborted();
    const client = await this.connected(signal);
    // Store values are opaque bytes. Returning RESP bulk strings as UTF-8
    // strings would replace invalid byte sequences before the provider can
    // copy them into Uint8Array.
    const operation = client.sendCommand(args, {
      typeMapping: {
        [RESP_TYPES.BLOB_STRING]: Buffer
      }
    });
    return await waitForOperation(operation, signal, this.operationTimeoutMs);
  }

  async eval(
    script: string,
    keys: readonly string[],
    args: readonly RedisCommandValue[],
    signal?: AbortSignal
  ): Promise<unknown> {
    return await this.command(
      ['EVAL', script, String(keys.length), ...keys, ...args],
      signal
    );
  }

  async dispose(): Promise<void> {
    if (this.disposed) return;
    this.disposed = true;
    const client = this.client;
    this.client = undefined;
    if (client !== undefined && client.isOpen === true) {
      await client.quit();
    }
  }

  private async connected(signal?: AbortSignal): Promise<RedisCommandClient> {
    signal?.throwIfAborted();
    if (this.disposed) throw new Error('Redis Store is disposed.');
    const client = this.providedClient ?? this.client;
    if (client === undefined) throw new Error('Redis Store is disposed.');
    if (isClientReady(client)) return client;
    if (this.connectionAttempt === undefined) {
      if (client.isOpen === true) {
        throw new Error('Redis client is connecting outside the Store lifecycle.');
      }
      const attempt = client.connect().then(() => undefined);
      this.connectionAttempt = attempt;
      attempt.then(
        () => {
          if (this.connectionAttempt === attempt) this.connectionAttempt = undefined;
        },
        () => {
          if (this.connectionAttempt === attempt) this.connectionAttempt = undefined;
        }
      );
    }
    await waitForOperation(this.connectionAttempt, signal, this.operationTimeoutMs);
    if (!isClientReady(client)) {
      throw new Error('Redis client connection completed before it became ready.');
    }
    return client;
  }
}

function isClientReady(client: RedisCommandClient): boolean {
  return client.isReady === true;
}

function requireOptions(
  options: ZLinkRedisLocationOptions | ZLinkRedisRelocationOptions
): void {
  if (options.keyPrefix.length === 0) {
    throw new Error('Redis Store keyPrefix is required.');
  }
  if (options.keyPrefix.includes('{') || options.keyPrefix.includes('}')) {
    throw new Error('Redis Store keyPrefix must not contain hash-tag braces.');
  }
  if (
    options.client === undefined
    && options.url === undefined
    && options.clientOptions === undefined
  ) {
    throw new Error('Redis Store requires url, clientOptions, or client.');
  }
  if (
    options.operationTimeoutMs !== undefined
    && (!Number.isSafeInteger(options.operationTimeoutMs)
      || options.operationTimeoutMs < 1)
  ) {
    throw new RangeError('Redis Store operationTimeoutMs must be a positive safe integer.');
  }
}

async function waitForOperation<T>(
  operation: Promise<T>,
  signal: AbortSignal | undefined,
  timeoutMs: number | undefined
): Promise<T> {
  if (signal === undefined && timeoutMs === undefined) return await operation;
  return await new Promise<T>((resolve, reject) => {
    let timeout: NodeJS.Timeout | undefined;
    let settled = false;
    const cleanup = () => {
      signal?.removeEventListener('abort', onAbort);
      if (timeout !== undefined) clearTimeout(timeout);
    };
    const settle = (action: () => void) => {
      if (settled) return;
      settled = true;
      cleanup();
      action();
    };
    const onAbort = () => settle(
      () => reject(signal?.reason ?? new Error('Redis Store operation aborted.'))
    );
    signal?.addEventListener('abort', onAbort, { once: true });
    if (timeoutMs !== undefined) {
      timeout = setTimeout(
        () => settle(() =>
          reject(new Error(`Redis Store operation timed out after ${timeoutMs} ms.`))
        ),
        timeoutMs
      );
    }
    operation.then(
      value => settle(() => resolve(value)),
      error => settle(() => reject(error))
    );
  });
}
