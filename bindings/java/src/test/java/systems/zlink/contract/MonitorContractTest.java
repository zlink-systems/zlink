package systems.zlink.contract;

import systems.zlink.TestSupport;
import systems.zlink.contracts.core.Context;
import systems.zlink.contracts.core.Zlink;
import systems.zlink.contracts.eventing.MonitorEventFlags;
import systems.zlink.contracts.eventing.MonitorStatusDetailFlags;
import systems.zlink.contracts.sockets.PairSocket;
import org.junit.jupiter.api.Test;

import static org.junit.jupiter.api.Assertions.assertTrue;
import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertThrows;

public class MonitorContractTest {
    @Test
    public void monitorHwmBytesAreForwardedExactly() {
        TestSupport.assumeNative();

        try (Context ctx = Zlink.createContext();
             PairSocket socket = ctx.createPairSocket()) {
            long monitorHwmBytes = 12_345L;
            try (var monitor = socket.monitorOpen(monitorHwmBytes)) {
                assertEquals(monitorHwmBytes * 2L,
                    ctx.coreHwmBudgetSnapshot()
                        .monitorQueueAppliedHwmBytes());
            }
            assertThrows(IllegalArgumentException.class,
                () -> socket.monitorOpen(-1L));
        }
    }

    @Test
    public void monitorStatusUsesCanonicalMonitorSurface() {
        TestSupport.assumeNative();

        try (Context ctx = Zlink.createContext();
             PairSocket socket = ctx.createPairSocket();
             var monitor = socket.monitorOpen()) {
            long sendHwmBytes = (long) Integer.MAX_VALUE + 4096L;
            long recvHwmBytes = (long) Integer.MAX_VALUE + 8192L;
            socket.options().sendHwm(sendHwmBytes);
            socket.options().recvHwm(recvHwmBytes);

            var status = monitor.status();
            assertEquals(4, status.abiVersion());
            assertEquals(232, status.structSize());
            assertTrue(status.sndPendingMsgs() >= 0L);
            assertTrue(status.sndPendingBytes() >= 0L);
            assertTrue(status.rcvPendingBytes() >= 0L);
            assertEquals(sendHwmBytes,
                status.autoHwmAppliedSendHwmBytes());
            assertEquals(recvHwmBytes,
                status.autoHwmAppliedRecvHwmBytes());
            assertTrue(status.minimumCoreMessageChargeBytes() > 0L);
            assertTrue(status.flowPausedConnections() >= 0L);
            assertTrue(status.flowPauseAppliedTotal() >= 0L);
            assertTrue(status.flowResumeAppliedTotal() >= 0L);
            assertTrue(status.flowStateStaleTotal() >= 0L);
            assertTrue(status.flowPauseDurationMs() >= 0L);
        }
    }

    @Test
    public void statusDetailAndEventFlagConstantsMatchCAbi() {
        // core/include/zlink_enum.h:
        //   ZLINK_MONITOR_STATUS_DETAIL_FLOW_STATE = 1u << 5
        assertEquals(1 << 5, MonitorStatusDetailFlags.FLOW_STATE.mask());

        // core/include/zlink/eventing/api.h:
        //   ZLINK_MONITOR_EVENT_FLAG_CONNECTION_READY_EDGE       = 1u << 0
        //   ZLINK_MONITOR_EVENT_FLAG_SEND_FLOW_WRITABLE          = 1u << 1
        //   ZLINK_MONITOR_EVENT_FLAG_FLOW_STATE_STALE_GENERATION = 1u << 2
        //   ZLINK_MONITOR_EVENT_FLAG_FLOW_STATE_STALE_EPOCH      = 1u << 3
        assertEquals(1 << 0, MonitorEventFlags.CONNECTION_READY_EDGE.mask());
        assertEquals(1 << 1, MonitorEventFlags.SEND_FLOW_WRITABLE.mask());
        assertEquals(1 << 2, MonitorEventFlags.FLOW_STATE_STALE_GENERATION.mask());
        assertEquals(1 << 3, MonitorEventFlags.FLOW_STATE_STALE_EPOCH.mask());

        var combined = MonitorEventFlags.fromMask(
            MonitorEventFlags.SEND_FLOW_WRITABLE.mask()
                | MonitorEventFlags.FLOW_STATE_STALE_EPOCH.mask());
        assertTrue(combined.contains(MonitorEventFlags.SEND_FLOW_WRITABLE));
        assertTrue(combined.contains(MonitorEventFlags.FLOW_STATE_STALE_EPOCH));
        assertTrue(!combined.contains(MonitorEventFlags.CONNECTION_READY_EDGE));
    }
}
