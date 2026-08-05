package systems.zlink.contract;

import systems.zlink.TestSupport;
import systems.zlink.contracts.core.Context;
import systems.zlink.contracts.core.Zlink;
import systems.zlink.contracts.sockets.PairSocket;
import org.junit.jupiter.api.Test;

import static org.junit.jupiter.api.Assertions.assertTrue;
import static org.junit.jupiter.api.Assertions.assertEquals;

public class MonitorContractTest {
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
            assertEquals(2, status.abiVersion());
            assertEquals(232, status.structSize());
            assertTrue(status.sndPendingMsgs() >= 0L);
            assertEquals(sendHwmBytes,
                status.autoHwmAppliedSendHwmBytes());
            assertEquals(recvHwmBytes,
                status.autoHwmAppliedRecvHwmBytes());
            assertTrue(status.minimumCoreMessageChargeBytes() > 0L);
        }
    }
}
