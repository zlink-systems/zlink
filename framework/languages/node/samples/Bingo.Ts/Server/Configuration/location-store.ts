import { ZLinkRedisLocationStore } from '@zlink-systems/framework-locations-redis';
import type { ZLinkLocationOptions } from '@zlink-systems/framework';
import type { BingoSampleConfig } from './sample-config';

function createBingoLocationStore(config: Pick<BingoSampleConfig, 'redisEndpoint' | 'redisKeyPrefix'>): ZLinkRedisLocationStore {
  return new ZLinkRedisLocationStore({
    url: `redis://${config.redisEndpoint}`,
    keyPrefix: `${config.redisKeyPrefix}location`
  });
}

function bingoLocationOptions(options: ZLinkLocationOptions): void {
  options
    .pollingIntervalMs(100)
    .ownerLeaseRenewIntervalMs(10_000)
    .ownerLeaseTtlMs(30_000)
    .ownerLeaseFencingMarginMs(5_000)
    .ownerLeaseRenewTimeoutMs(3_000);
}

export { bingoLocationOptions, createBingoLocationStore };
