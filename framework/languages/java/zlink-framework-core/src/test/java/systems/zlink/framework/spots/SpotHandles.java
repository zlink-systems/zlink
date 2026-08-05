package systems.zlink.framework.spots;

import systems.zlink.contracts.core.RoutingId;

public final class SpotHandles {
    private SpotHandles() {
    }

    public static SpotHandle create(String spotId) {
        return new FrameworkSpotHandle(
            "test-mesh", spotId, RoutingId.from("test-node"), 1L);
    }
}
