package systems.zlink.framework.runtime.internal.backend;

import java.util.Optional;
import systems.zlink.contracts.core.CoreHwmBudgetSnapshot;
import systems.zlink.framework.runtime.configuration.ZLinkInboundDispatchRegistration;

public interface ZLinkBackendContext extends ZLinkBackendObject {
    default void configureCoreHwm(ZLinkInboundDispatchRegistration options) {
    }
    default Optional<CoreHwmBudgetSnapshot> coreHwmBudgetSnapshot() {
        return Optional.empty();
    }
    default void resetCoreHwmBudgetMetrics() {
        throw new UnsupportedOperationException(
            "Core HWM metrics reset is not supported by this backend context");
    }
    void shutdown();
}
