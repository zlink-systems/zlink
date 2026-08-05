import { ZLinkRedisLocationStore } from '@zlink-systems/framework-locations-redis';
import type { ZLinkLocationOptions } from '@zlink-systems/framework';
import type { GameQuestServerConfig } from './sample-config';

function createGameQuestLocationStore(
  config: Pick<GameQuestServerConfig, 'redisEndpoint' | 'redisKeyPrefix'>
): ZLinkRedisLocationStore {
  return new ZLinkRedisLocationStore({
    url: `redis://${config.redisEndpoint}`,
    keyPrefix: `${config.redisKeyPrefix}location`
  });
}

function gameQuestLocationOptions(options: ZLinkLocationOptions): void {
  options
    .pollingIntervalMs(100)
    .ownerLeaseRenewIntervalMs(1000)
    .ownerLeaseTtlMs(5000)
    .ownerLeaseFencingMarginMs(500)
    .ownerLeaseRenewTimeoutMs(500);
}

export { createGameQuestLocationStore, gameQuestLocationOptions };
