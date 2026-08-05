import { ZLinkRedisRelocationStore } from '@zlink-systems/framework-locations-redis';
import type { TicTacToeSampleConfig } from './sample-config';

function createTicTacToeRelocationStore(
  config: Pick<TicTacToeSampleConfig, 'redisEndpoint' | 'redisKeyPrefix'>
): ZLinkRedisRelocationStore {
  return new ZLinkRedisRelocationStore({
    url: `redis://${config.redisEndpoint}`,
    keyPrefix: `${config.redisKeyPrefix}relocation`
  });
}

export { createTicTacToeRelocationStore };
