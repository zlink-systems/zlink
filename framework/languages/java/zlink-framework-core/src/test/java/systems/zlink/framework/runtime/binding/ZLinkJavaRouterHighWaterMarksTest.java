package systems.zlink.framework.runtime.binding;

import static org.junit.jupiter.api.Assertions.assertEquals;

import org.junit.jupiter.api.Test;

import systems.zlink.contracts.core.Zlink;
import systems.zlink.framework.runtime.mesh.MeshNodeRegistration;

final class ZLinkJavaRouterHighWaterMarksTest {
    @Test
    void unconfiguredValuesLeaveNativeOptionsUntouched() {
        try (var context = Zlink.createContext();
                var socket = context.createRouterSocket()) {
            socket.options().sendHwm(12345L);
            socket.options().recvHwm(23456L);

            ZLinkJavaSocketOptions.applyRouterHighWaterMarks(
                    socket, new MeshNodeRegistration.RouterSocketConfig());

            assertEquals(12345L, socket.options().sendHwm());
            assertEquals(23456L, socket.options().recvHwm());
        }
    }

    @Test
    void explicitZeroIsPassedToCoreAsManualUnlimited() {
        try (var context = Zlink.createContext();
                var socket = context.createRouterSocket()) {
            socket.options().sendHwm(12345L);
            socket.options().recvHwm(23456L);
            var config = new MeshNodeRegistration.RouterSocketConfig();
            config.setSendHighWaterMark(0L);
            config.setReceiveHighWaterMark(0L);

            ZLinkJavaSocketOptions.applyRouterHighWaterMarks(socket, config);

            assertEquals(0L, socket.options().sendHwm());
            assertEquals(0L, socket.options().recvHwm());
        }
    }
}
