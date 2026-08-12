package systems.zlink.framework.spring;
import java.util.Objects;

import org.springframework.boot.actuate.health.Health;
import org.springframework.boot.actuate.health.HealthIndicator;
import systems.zlink.framework.spring.internal.runtime.ZLinkFrameworkLifecycle;

public final class ZLinkDrainReadinessContributor implements HealthIndicator {
    private final ZLinkFrameworkLifecycle lifecycle;

    public ZLinkDrainReadinessContributor(ZLinkFrameworkLifecycle lifecycle) {
        this.lifecycle = Objects.requireNonNull(lifecycle, "lifecycle");
    }

    @Override
    public Health health() {
        return lifecycle.isReady()
            ? Health.up().build()
            : Health.outOfService().withDetail("state", "draining").build();
    }
}
