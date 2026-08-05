import type { RedisClientOptions, RedisClientType } from 'redis';

export interface ZLinkRedisLocationOptions {
  readonly url?: string;
  readonly client?: RedisClientType;
  readonly clientOptions?: RedisClientOptions;
  readonly keyPrefix: string;
  readonly operationTimeoutMs?: number;
}

export interface ZLinkRedisRelocationOptions {
  readonly url?: string;
  readonly client?: RedisClientType;
  readonly clientOptions?: RedisClientOptions;
  readonly keyPrefix: string;
  readonly operationTimeoutMs?: number;
}
