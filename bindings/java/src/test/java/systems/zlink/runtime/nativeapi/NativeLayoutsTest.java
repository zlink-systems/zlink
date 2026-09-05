package systems.zlink.runtime.nativeapi;

import org.junit.jupiter.api.Test;

import static org.junit.jupiter.api.Assertions.assertEquals;

public class NativeLayoutsTest {
    @Test
    public void monitorEventAllocationMatchesCoreEventSize() {
        assertEquals(800, NativeLayouts.MONITOR_EVENT_LAYOUT.byteSize());
        assertEquals(784, NativeLayouts.MONITOR_CONNECTION_ID_OFFSET);
        assertEquals(792, NativeLayouts.MONITOR_TRANSPORT_LANE_OFFSET);
        assertEquals(796, NativeLayouts.MONITOR_FLAGS_OFFSET);
    }
}
