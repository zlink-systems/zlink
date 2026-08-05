import { ZLinkRedisRelocationStore } from '@zlink-systems/framework-locations-redis';
import type { BingoSampleConfig } from './sample-config';

function createBingoRelocationStore(
  config: Pick<BingoSampleConfig, 'redisEndpoint' | 'redisKeyPrefix'>
): ZLinkRedisRelocationStore {
  return new ZLinkRedisRelocationStore({
    url: `redis://${config.redisEndpoint}`,
    keyPrefix: `${config.redisKeyPrefix}relocation`
  });
}

export { createBingoRelocationStore };
