package systems.zlink.runtime.nativeapi;

import org.junit.jupiter.api.Test;

import static org.junit.jupiter.api.Assertions.assertEquals;

public class NativeLayoutsTest {
    @Test
    public void monitorEventAllocationMatchesCoreEventSize() {
        // Core writes the diagnostic tail even when Java projects only the
        // event, value, routing id, and address fields.
        assertEquals(816, NativeLayouts.MONITOR_EVENT_LAYOUT.byteSize());
    }
}
