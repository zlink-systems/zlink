package systems.zlink.framework.runtime.internal.service;

import static org.junit.jupiter.api.Assertions.*;

import org.junit.jupiter.api.Test;

import systems.zlink.framework.runtime.internal.execution.ZLinkStateLane;

import java.util.function.Supplier;

final class ZLinkServiceStateOwnershipTest {
    @Test
    void livenessStateWorkHasAnActualLaneOwnerAndRejectsReentry() throws Exception {
        var registry = new ZLinkServiceLivenessRegistry();
        var entry =
                ZLinkServiceLivenessRegistry.class.getDeclaredMethod("inStateLane", Supplier.class);
        entry.setAccessible(true);
        entry.invoke(
                registry,
                (Supplier<Void>)
                        () -> {
                            assertNotNull(ZLinkStateLane.current());
                            assertThrows(IllegalStateException.class, registry::size);
                            return null;
                        });
        assertEquals(0, registry.size());
    }
}
