package systems.zlink.framework.runtime.binding;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertTrue;

import org.junit.jupiter.api.Test;

import systems.zlink.contracts.core.Zlink;
import systems.zlink.framework.runtime.configuration.ZLinkInboundDispatchRegistration;

final class ZLinkJavaContextMemoryInputTest {
    @Test
    void omittedMemoryLimitUsesCoreDetection() {
        try (var context = Zlink.createContext()) {
            var adapter = new ZLinkJavaContext(context);
            adapter.configureCoreHwm(new ZLinkInboundDispatchRegistration());
            var snapshot = adapter.coreHwmBudgetSnapshot().orElseThrow();
            assertEquals(0L, snapshot.configuredMemoryLimitBytes());
            assertTrue(snapshot.resolvedMemoryLimitBytes() > 0L);
        }
    }
}
