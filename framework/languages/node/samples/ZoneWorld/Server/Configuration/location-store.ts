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

//  owner lease는 Location runtime 5절이 정한 기본값(TTL 15초, 갱신 5초, timeout 3초,
//  fencing margin 5초)을 그대로 쓴다. ZoneWorld 스펙은 report TTL 15초(2.2)만 정하고
//  owner lease 재정의를 요구하지 않으므로 어떤 역할도 덮어쓰지 않는다.
function zoneWorldLocationOptions(options: ZLinkLocationOptions, shared: SharedSettings): void {
  options.pollingIntervalMs(100);
  if (shared.sessionRelocationSealTimeoutMs !== undefined) {
    options.sessionRelocationSealTimeoutMs(shared.sessionRelocationSealTimeoutMs);
  }
}

export {
  createZoneWorldLocationStore,
  createZoneWorldRelocationStore,
  zoneWorldLocationOptions
};
