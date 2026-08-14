package systems.zlink.framework.runtime.binding;

import java.util.Optional;
import systems.zlink.contracts.core.Context;
import systems.zlink.contracts.core.CoreHwmBudgetSnapshot;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendContext;

record ZLinkJavaContext(Context nativeContext) implements ZLinkBackendContext {
    @Override public String name() { return "context"; }
    @Override
    public void configureCoreHwm(
        systems.zlink.framework.runtime.configuration.ZLinkInboundDispatchRegistration options) {
        nativeContext.options().coreHwmProfile(
            systems.zlink.contracts.sockets.AutoHwmProfile.valueOf(
                options.coreHwmProfile().name()));
        if (options.coreHwmMemoryLimitBytes().isPresent()) {
            nativeContext.options().coreHwmMemoryLimitBytes(
                options.coreHwmMemoryLimitBytes().getAsLong());
        } else {
            long runtimeMemoryHint = Runtime.getRuntime().maxMemory();
            if (runtimeMemoryHint > 0 && runtimeMemoryHint < Long.MAX_VALUE) {
                nativeContext.options().coreHwmMemoryLimitBytes(runtimeMemoryHint);
            }
        }
        if (options.coreHwmBudgetBytes().isPresent()) {
            nativeContext.options().coreHwmBudgetBytes(
                options.coreHwmBudgetBytes().getAsLong());
        }
        nativeContext.recalculateAutoHwm();
    }
    @Override
    public Optional<CoreHwmBudgetSnapshot> coreHwmBudgetSnapshot() {
        return Optional.of(nativeContext.coreHwmBudgetSnapshot());
    }
    @Override
    public void resetCoreHwmBudgetMetrics() {
        nativeContext.resetCoreHwmBudgetMetrics();
    }
    @Override public void shutdown() { nativeContext.shutdown(); }

    @Override
    public void close() {
        try {
            nativeContext.shutdown();
        } finally {
            nativeContext.close();
        }
    }
}
