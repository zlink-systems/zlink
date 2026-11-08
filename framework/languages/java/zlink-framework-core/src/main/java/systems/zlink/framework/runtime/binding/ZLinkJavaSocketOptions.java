package systems.zlink.framework.runtime.binding;

import static systems.zlink.framework.runtime.host.ZLinkFrameworkRuntime.DEFAULT_TERMINATION_DEADLINE;

import systems.zlink.contracts.sockets.RouterSocket;
import systems.zlink.contracts.sockets.Socket;
import systems.zlink.framework.runtime.mesh.MeshNodeRegistration.RouterSocketConfig;

final class ZLinkJavaSocketOptions {
    private ZLinkJavaSocketOptions() {}

    /**
     * The default shutdown deadline is 30 seconds. Keep transport output alive for that bounded
     * window so a reply accepted by a draining Framework socket is not discarded by a zero-linger
     * close.
     */
    static <T extends Socket> T configureFrameworkSocket(T socket) {
        socket.options().linger(DEFAULT_TERMINATION_DEADLINE);
        return socket;
    }

    static RouterSocket configureFrameworkRouterSocket(RouterSocket socket) {
        configureFrameworkSocket(socket);
        socket.options().handover(true);
        return socket;
    }

    static void applyRouterHighWaterMarks(RouterSocket socket, RouterSocketConfig config) {
        if (config.configuredSendHighWaterMark() != null) {
            socket.options().sendHwm(config.configuredSendHighWaterMark());
        }
        if (config.configuredReceiveHighWaterMark() != null) {
            socket.options().recvHwm(config.configuredReceiveHighWaterMark());
        }
    }
}
