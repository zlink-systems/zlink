import type { ZLinkLocationOptions } from '@zlink-systems/framework';
import {
  ZLinkRedisLocationStore,
  ZLinkRedisRelocationStore
} from '@zlink-systems/framework-locations-redis';

export function createRedisLocationStore(options: { readonly redisEndpoint: string; readonly redisKeyPrefix: string }): ZLinkRedisLocationStore {
  return new ZLinkRedisLocationStore({ url: `redis://${options.redisEndpoint}`, keyPrefix: options.redisKeyPrefix });
}

export function createRedisRelocationStore(options: { readonly redisEndpoint: string; readonly redisKeyPrefix: string }): ZLinkRedisRelocationStore {
  return new ZLinkRedisRelocationStore({
    url: `redis://${options.redisEndpoint}`,
    keyPrefix: `${options.redisKeyPrefix}:relocation`
  });
}

export function locationOptions(options: ZLinkLocationOptions): void {
  options
    .pollingIntervalMs(100)
    .ownerLeaseRenewIntervalMs(500)
    .ownerLeaseRenewTimeoutMs(250)
    .ownerLeaseFencingMarginMs(500)
    .ownerLeaseTtlMs(30_000)
    .routeCacheMaxAgeMs(500)
    .messageFollowDurationMs(6000);
}
