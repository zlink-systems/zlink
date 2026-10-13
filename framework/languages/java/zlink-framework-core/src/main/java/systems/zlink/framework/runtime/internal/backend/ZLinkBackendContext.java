package systems.zlink.framework.runtime.internal.backend;

import systems.zlink.contracts.core.CoreHwmBudgetSnapshot;
import systems.zlink.framework.runtime.configuration.ZLinkInboundDispatchRegistration;
import systems.zlink.framework.runtime.internal.execution.ZLinkStateLane;

import java.util.Optional;

public interface ZLinkBackendContext extends ZLinkBackendObject {
    /** JVM source selection and transport metadata use the context's one state owner. */
    default ZLinkStateLane sourceTransportLane() {
        return null;
    }

    default void configureCoreHwm(ZLinkInboundDispatchRegistration options) {}

    default Optional<CoreHwmBudgetSnapshot> coreHwmBudgetSnapshot() {
        return Optional.empty();
    }

    default void resetCoreHwmBudgetMetrics() {
        throw new UnsupportedOperationException(
                "Core HWM metrics reset is not supported by this backend context");
    }

    void shutdown();
}
