package systems.zlink.framework.runtime.host;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertThrows;
import static org.junit.jupiter.api.Assertions.assertTrue;

import java.util.concurrent.TimeUnit;
import org.junit.jupiter.api.Test;
import systems.zlink.framework.runtime.binding.ZLinkJavaBackendAdapterFactory;
import systems.zlink.framework.runtime.configuration.DefaultZLinkFrameworkOptions;

final class ZLinkFrameworkRuntimeCoreHwmDiagnosticsTest {
    @Test
    void activeContextProjectsAndResetsBindingSnapshotUntilTeardown()
        throws Exception {
        long budgetBytes = 4L * 1024 * 1024;
        DefaultZLinkFrameworkOptions options =
            new DefaultZLinkFrameworkOptions();
        options.configureInboundDispatch().setCoreHwmBudgetBytes(budgetBytes);
        ZLinkFrameworkRuntime runtime = ZLinkFrameworkRuntimeTestAccess.start(
            options,
            new ZLinkJavaBackendAdapterFactory());
        try {
            var before = runtime.status().capacity();
            assertEquals(
                budgetBytes,
                before.coreHwm().configuredBudgetBytes().orElseThrow());

            runtime.resetCapacityMetrics();
            var after = runtime.status().capacity();

            assertTrue(after.measurementEpoch() > before.measurementEpoch());
            assertEquals(
                before.coreHwm().effectiveBudgetBytes(),
                after.coreHwm().effectiveBudgetBytes());

            runtime.closeAsync().toCompletableFuture().get(5, TimeUnit.SECONDS);
            assertEquals(
                after.coreHwm().effectiveBudgetBytes(),
                runtime.status().capacity().coreHwm().effectiveBudgetBytes());
            var inactive = assertThrows(
                IllegalStateException.class,
                runtime::resetCapacityMetrics);
            assertEquals(
                "Capacity metrics reset requires an active Framework runtime context",
                inactive.getMessage());
        } finally {
            runtime.close();
        }
    }
}
