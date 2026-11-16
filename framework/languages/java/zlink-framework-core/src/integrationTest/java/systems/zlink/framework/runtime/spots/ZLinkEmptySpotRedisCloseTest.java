package systems.zlink.framework.runtime.spots;

import org.junit.jupiter.params.ParameterizedTest;
import org.junit.jupiter.params.provider.ValueSource;

import systems.zlink.framework.locations.redis.ZLinkRedisLocationOptions;
import systems.zlink.framework.locations.redis.ZLinkRedisLocationStore;
import systems.zlink.framework.testsupport.ZLinkTestRedis;

import java.util.UUID;

final class ZLinkEmptySpotRedisCloseTest {
    @ParameterizedTest
    @ValueSource(booleans = {false, true})
    void emptySpotClosesThroughCreatedAndFoundReferences(boolean find) throws Exception {
        String endpoint = ZLinkTestRedis.endpoint();
        try (var store =
                new ZLinkRedisLocationStore(
                        new ZLinkRedisLocationOptions()
                                .setConnectionString(endpoint)
                                .setKeyPrefix("zlink:empty-spot-close:" + UUID.randomUUID()))) {
            ZLinkSpotCloseConformanceTest.closeEmptySpot(find, store);
        }
    }
}
