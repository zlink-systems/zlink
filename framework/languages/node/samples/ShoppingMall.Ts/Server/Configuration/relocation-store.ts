import { ZLinkRedisRelocationStore } from '@zlink-systems/framework-locations-redis';
import type { ShoppingMallServerConfig } from './sample-config';

function createShoppingMallRelocationStore(
  config: Pick<ShoppingMallServerConfig, 'redisEndpoint' | 'redisKeyPrefix'>
): ZLinkRedisRelocationStore {
  return new ZLinkRedisRelocationStore({
    url: `redis://${config.redisEndpoint}`,
    keyPrefix: `${config.redisKeyPrefix}relocation`
  });
}

export { createShoppingMallRelocationStore };
