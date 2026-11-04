package systems.zlink.framework.spring.internal.runtime;

import java.time.Duration;

public final class ZLinkFrameworkLifecycleTestAccess {
    private ZLinkFrameworkLifecycleTestAccess() {}

    public static Duration springShutdownDrainDeadline() {
        return ZLinkFrameworkLifecycle.SPRING_SHUTDOWN_DRAIN_DEADLINE;
    }
}
