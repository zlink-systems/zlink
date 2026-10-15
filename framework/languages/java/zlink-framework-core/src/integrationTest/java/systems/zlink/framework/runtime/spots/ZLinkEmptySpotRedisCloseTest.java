package systems.zlink.framework.runtime.spots;

import static org.junit.jupiter.api.Assumptions.assumeTrue;

import org.junit.jupiter.params.ParameterizedTest;
import org.junit.jupiter.params.provider.ValueSource;

import systems.zlink.framework.locations.redis.ZLinkRedisLocationOptions;
import systems.zlink.framework.locations.redis.ZLinkRedisLocationStore;

import java.util.UUID;

final class ZLinkEmptySpotRedisCloseTest {
    @ParameterizedTest
    @ValueSource(booleans = {false, true})
    void emptySpotClosesThroughCreatedAndFoundReferences(boolean find) throws Exception {
        String endpoint = System.getenv("ZLINK_REDIS_LOCATION_ENDPOINT");
        assumeTrue(
                endpoint != null && !endpoint.isBlank(),
                "ZLINK_REDIS_LOCATION_ENDPOINT is not set");
        try (var store =
                new ZLinkRedisLocationStore(
                        new ZLinkRedisLocationOptions()
                                .setConnectionString(endpoint)
                                .setKeyPrefix("zlink:empty-spot-close:" + UUID.randomUUID()))) {
            ZLinkSpotCloseConformanceTest.closeEmptySpot(find, store);
        }
    }
}
