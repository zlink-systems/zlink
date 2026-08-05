import {
  ZLinkRedisLocationStore,
  ZLinkRedisRelocationStore
} from '@zlink-systems/framework-locations-redis';
import type { ZLinkLocationOptions } from '@zlink-systems/framework';
import type { SharedSettings } from './configuration';

function createZoneWorldLocationStore(shared: SharedSettings): ZLinkRedisLocationStore {
  return new ZLinkRedisLocationStore({
    url: `redis://${shared.redisEndpoint}`,
    keyPrefix: `${shared.redisKeyPrefix}location`
  });
}

function createZoneWorldRelocationStore(shared: SharedSettings): ZLinkRedisRelocationStore {
  return new ZLinkRedisRelocationStore({
    url: `redis://${shared.redisEndpoint}`,
    keyPrefix: `${shared.redisKeyPrefix}relocation`
  });
}

function zoneWorldLocationOptions(options: ZLinkLocationOptions): void {
  options
    .pollingIntervalMs(100)
    .ownerLeaseRenewIntervalMs(1_000)
    .ownerLeaseTtlMs(3_000)
    .ownerLeaseFencingMarginMs(500)
    .ownerLeaseRenewTimeoutMs(500);
}

export {
  createZoneWorldLocationStore,
  createZoneWorldRelocationStore,
  zoneWorldLocationOptions
};
