import { ZLinkRedisRelocationStore } from '@zlink-systems/framework-locations-redis';
import type { GameQuestServerConfig } from './sample-config';

function createGameQuestRelocationStore(
  config: Pick<GameQuestServerConfig, 'redisEndpoint' | 'redisKeyPrefix'>
): ZLinkRedisRelocationStore {
  return new ZLinkRedisRelocationStore({
    url: `redis://${config.redisEndpoint}`,
    keyPrefix: `${config.redisKeyPrefix}relocation`
  });
}

export { createGameQuestRelocationStore };
