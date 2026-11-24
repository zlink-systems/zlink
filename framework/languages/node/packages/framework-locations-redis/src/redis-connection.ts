import { createHash } from 'node:crypto';
import { createClient, ErrorReply, RESP_TYPES } from 'redis';
import type { RedisClientType } from 'redis';
import type { ZLinkRedisLocationOptions, ZLinkRedisRelocationOptions } from './redis-options';

export type RedisCommandValue = string | Buffer;
export type RedisCommandClient = Pick<
  RedisClientType,
  'isOpen' | 'isReady' | 'connect' | 'disconnect' | 'sendCommand' | 'quit' | 'on'
>;

const scriptDigests = new Map<RedisCommandValue, string>();
const neverSettled = () => false;

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
    return await this.execute((client) => sendCommand(client, args), signal);
  }

  async eval(
    script: RedisCommandValue,
    keys: readonly string[],
    args: readonly RedisCommandValue[],
    signal?: AbortSignal
  ): Promise<unknown> {
    return await this.execute((client, isSettled) => {
      let digest = scriptDigests.get(script);
      if (digest === undefined) {
        digest = createHash('sha1').update(script).digest('hex');
        scriptDigests.set(script, digest);
      }
      const shaCommand = ['EVALSHA', digest, String(keys.length), ...keys, ...args];
      return sendCommand(client, shaCommand).catch((error: unknown) => {
        const missingScript =
          error instanceof ErrorReply &&
          (error.message === 'NOSCRIPT' || error.message.startsWith('NOSCRIPT '));
        if (!missingScript || isSettled()) throw error;

        const fallback = shaCommand.slice();
        fallback[0] = 'EVAL';
        fallback[1] = script;
        return sendCommand(client, fallback);
      });
    }, signal);
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

  private async execute(
    send: (client: RedisCommandClient, isSettled: () => boolean) => Promise<unknown>,
    signal?: AbortSignal
  ): Promise<unknown> {
    const client = await this.connected(signal);
    return await waitForOperation(
      (isSettled) => send(client, isSettled),
      signal,
      this.operationTimeoutMs
    );
  }

  private async connected(signal?: AbortSignal): Promise<RedisCommandClient> {
    signal?.throwIfAborted();
    if (this.disposed) throw new Error('Redis Store is disposed.');
    const client = this.providedClient ?? this.client;
    if (client === undefined) throw new Error('Redis Store is disposed.');
    if (isClientReady(client)) return client;
    if (this.connectionAttempt === undefined) {
      // Publish the attempt before any await. A failed connection can leave
      // node-redis open but not ready; the close and reconnect sequence must
      // still be owned by this single promise when callers arrive together.
      const attempt = (async () => {
        if (client.isOpen === true && !isClientReady(client)) {
          await client.disconnect();
        }
        await client.connect();
      })();
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
    const attempt = this.connectionAttempt;
    await waitForOperation(() => attempt, signal, this.operationTimeoutMs);
    if (!isClientReady(client)) {
      throw new Error('Redis client connection completed before it became ready.');
    }
    return client;
  }
}

function sendCommand(client: RedisCommandClient, args: RedisCommandValue[]): Promise<unknown> {
  // Store values are opaque bytes. Returning RESP bulk strings as UTF-8
  // strings would replace invalid byte sequences before the provider can
  // copy them into Uint8Array.
  return client.sendCommand(args, {
    typeMapping: {
      [RESP_TYPES.BLOB_STRING]: Buffer
    }
  });
}

function isClientReady(client: RedisCommandClient): boolean {
  return client.isReady === true;
}

function requireOptions(options: ZLinkRedisLocationOptions | ZLinkRedisRelocationOptions): void {
  if (options.keyPrefix.length === 0) {
    throw new Error('Redis Store keyPrefix is required.');
  }
  if (options.keyPrefix.includes('{') || options.keyPrefix.includes('}')) {
    throw new Error('Redis Store keyPrefix must not contain hash-tag braces.');
  }
  if (
    options.client === undefined &&
    options.url === undefined &&
    options.clientOptions === undefined
  ) {
    throw new Error('Redis Store requires url, clientOptions, or client.');
  }
  if (
    options.operationTimeoutMs !== undefined &&
    (!Number.isSafeInteger(options.operationTimeoutMs) || options.operationTimeoutMs < 1)
  ) {
    throw new RangeError('Redis Store operationTimeoutMs must be a positive safe integer.');
  }
}

async function waitForOperation<T>(
  start: (isSettled: () => boolean) => Promise<T>,
  signal: AbortSignal | undefined,
  timeoutMs: number | undefined
): Promise<T> {
  signal?.throwIfAborted();
  if (signal === undefined && timeoutMs === undefined) return await start(neverSettled);
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
    const onAbort = () =>
      settle(() => reject(signal?.reason ?? new Error('Redis Store operation aborted.')));
    signal?.addEventListener('abort', onAbort, { once: true });
    if (timeoutMs !== undefined) {
      timeout = setTimeout(
        () =>
          settle(() => reject(new Error(`Redis Store operation timed out after ${timeoutMs} ms.`))),
        timeoutMs
      );
    }
    try {
      start(() => settled).then(
        (value) => settle(() => resolve(value)),
        (error) => settle(() => reject(error))
      );
    } catch (error) {
      settle(() => reject(error));
    }
  });
}
