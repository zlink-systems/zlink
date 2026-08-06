package systems.zlink.framework.runtime.binding;

import java.time.Duration;
import systems.zlink.contracts.sockets.RouterSocket;
import systems.zlink.contracts.sockets.Socket;

final class ZLinkJavaSocketOptions {
    /**
     * The default shutdown deadline is 30 seconds. Keep transport output
     * alive for that bounded window so a reply accepted by a draining
     * Framework socket is not discarded by a zero-linger close.
     */
    private static final Duration FRAMEWORK_LINGER = Duration.ofSeconds(30);

    private ZLinkJavaSocketOptions() {
    }

    static <T extends Socket> T configureFrameworkSocket(T socket) {
        socket.options().linger(FRAMEWORK_LINGER);
        return socket;
    }

    static RouterSocket configureFrameworkRouterSocket(RouterSocket socket) {
        configureFrameworkSocket(socket);
        socket.options().handover(true);
        return socket;
    }
}
