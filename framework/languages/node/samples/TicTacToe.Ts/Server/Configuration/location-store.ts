import { ZLinkRedisLocationStore } from '@zlink-systems/framework-locations-redis';
import type { TicTacToeSampleConfig } from './sample-config';

function createTicTacToeLocationStore(
  config: Pick<TicTacToeSampleConfig, 'redisEndpoint' | 'redisKeyPrefix'>
): ZLinkRedisLocationStore {
  return new ZLinkRedisLocationStore({
    url: `redis://${config.redisEndpoint}`,
    keyPrefix: `${config.redisKeyPrefix}location`
  });
}

export { createTicTacToeLocationStore };
