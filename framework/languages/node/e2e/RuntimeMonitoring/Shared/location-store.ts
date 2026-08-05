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

export function monitoringLocationOptions(): {
  pollingIntervalMs: number;
  ownerLeaseRenewIntervalMs: number;
  ownerLeaseTtlMs: number;
  storeFailureGraceMs: number;
} {
  return {
    pollingIntervalMs: 100,
    ownerLeaseRenewIntervalMs: 1000,
    ownerLeaseTtlMs: 3000,
    storeFailureGraceMs: 6000
  };
}
