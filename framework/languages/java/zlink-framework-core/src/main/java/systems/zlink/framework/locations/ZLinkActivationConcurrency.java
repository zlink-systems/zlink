package systems.zlink.framework.locations;

import systems.zlink.framework.runtime.internal.locations.*;

public record ZLinkActivationConcurrency(int active, int limit) {
    public ZLinkActivationConcurrency {
        if (active < 0 || limit <= 0 || active > limit) {
            throw new IllegalArgumentException(
                "activation concurrency requires 0 <= active <= limit and limit > 0");
        }
    }
}
