package systems.zlink.runtime.nativeapi;

import static org.junit.jupiter.api.Assertions.assertEquals;

import org.junit.jupiter.api.Test;

public class NativeLayoutsTest {
    @Test
    public void monitorEventAllocationMatchesCoreEventSize() {
        assertEquals(800, NativeLayouts.MONITOR_EVENT_LAYOUT.byteSize());
        assertEquals(784, NativeLayouts.MONITOR_CONNECTION_ID_OFFSET);
        assertEquals(792, NativeLayouts.MONITOR_TRANSPORT_LANE_OFFSET);
        assertEquals(796, NativeLayouts.MONITOR_FLAGS_OFFSET);
    }

    @Test
    void connectionSnapshotMatchesCore64BitAbi() {
        assertEquals(16, NativeLayouts.CONNECTION_FAILURE_LAYOUT.byteSize());
        assertEquals(4, NativeLayouts.CONNECTION_FAILURE_LAYOUT.byteAlignment());
        assertEquals(64, NativeLayouts.CONNECTION_ATTEMPT_LAYOUT.byteSize());
        assertEquals(8, NativeLayouts.CONNECTION_ATTEMPT_LAYOUT.byteAlignment());
        assertEquals(0, NativeLayouts.CONNECTION_ATTEMPT_ENDPOINT_OFFSET_OFFSET);
        assertEquals(8, NativeLayouts.CONNECTION_ATTEMPT_ENDPOINT_LENGTH_OFFSET);
        assertEquals(16, NativeLayouts.CONNECTION_ATTEMPT_INTENT_ID_OFFSET);
        assertEquals(24, NativeLayouts.CONNECTION_ATTEMPT_REVISION_OFFSET);
        assertEquals(32, NativeLayouts.CONNECTION_ATTEMPT_LAST_FAILURE_ID_OFFSET);
        assertEquals(40, NativeLayouts.CONNECTION_ATTEMPT_STATE_OFFSET);
        assertEquals(44, NativeLayouts.CONNECTION_ATTEMPT_LAST_FAILURE_OFFSET);
        assertEquals(0, NativeLayouts.CONNECTION_FAILURE_KIND_OFFSET);
        assertEquals(4, NativeLayouts.CONNECTION_FAILURE_STAGE_OFFSET);
        assertEquals(8, NativeLayouts.CONNECTION_FAILURE_NORMALIZED_ERRNO_OFFSET);
        assertEquals(12, NativeLayouts.CONNECTION_FAILURE_DISCONNECT_REASON_OFFSET);
    }
}
