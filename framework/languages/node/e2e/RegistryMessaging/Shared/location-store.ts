import type { ZLinkLocationOptions } from '@zlink-systems/framework';
import { ZLinkRedisLocationStore } from '@zlink-systems/framework-locations-redis';

export interface RedisLocationOptions {
  readonly redisEndpoint: string;
  readonly redisKeyPrefix: string;
}

export function createRedisLocationStore(options: RedisLocationOptions): ZLinkRedisLocationStore {
  return new ZLinkRedisLocationStore({
    url: `redis://${options.redisEndpoint}`,
    keyPrefix: options.redisKeyPrefix
  });
}

export function locationMessagingOptions(options: ZLinkLocationOptions): void {
  options
    .pollingIntervalMs(100)
    .ownerLeaseRenewIntervalMs(1000)
    .ownerLeaseTtlMs(5000);
}
