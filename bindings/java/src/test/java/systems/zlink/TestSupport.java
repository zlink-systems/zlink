package systems.zlink;

import systems.zlink.contracts.eventing.MonitorEvent;
import systems.zlink.contracts.eventing.MonitorEventType;
import systems.zlink.contracts.eventing.SocketMonitor;
import systems.zlink.contracts.core.Zlink;
import systems.zlink.contracts.errors.ZlinkRecvException;
import systems.zlink.contracts.sockets.RecvFlags;
import systems.zlink.contracts.sockets.RecvResult;
import org.junit.jupiter.api.Assertions;

import java.io.IOException;
import java.net.ServerSocket;
import java.util.Optional;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.function.BooleanSupplier;

public final class TestSupport {
    public static final int DEFAULT_TIMEOUT_MS = 5000;
    private static final int TCP_PORT_BASE =
        20_000 + (int) (ProcessHandle.current().pid() % 20_000);
    private static final AtomicInteger NEXT_TCP_PORT =
        new AtomicInteger(TCP_PORT_BASE);

    private TestSupport() {
    }

    public static void assumeNative() {
        try {
            Zlink.version();
        } catch (Throwable t) {
            Assertions.fail(
                "zlink native library not found: " + t.getMessage());
        }
    }

    public static String inprocEndpoint(String prefix) {
        return "inproc://" + prefix + "-" + System.nanoTime();
    }

    public static String tcpEndpoint() {
        return "tcp://127.0.0.1:" + nextTcpPort();
    }

    public static MonitorEvent awaitMonitorEvent(SocketMonitor monitor,
                                                 MonitorEventType eventType) {
        long deadline = System.nanoTime() + DEFAULT_TIMEOUT_MS * 1_000_000L;
        while (System.nanoTime() < deadline) {
            MonitorEvent event = null;
            try {
                event = monitor.recv(RecvFlags.DONT_WAIT);
            } catch (ZlinkRecvException ex) {
                if (ex.getResult() != RecvResult.NO_DATA) {
                    throw ex;
                }
            }
            if (event != null) {
                if (event.event() == eventType) {
                    return event;
                }
                continue;
            }

            if (eventType == MonitorEventType.CONNECTION_READY
                && monitor.status().isReady()) {
                return new MonitorEvent(eventType, 0L, Optional.empty(),
                    "", "");
            }

            try {
                Thread.sleep(1L);
            } catch (InterruptedException e) {
                Thread.currentThread().interrupt();
                throw new RuntimeException(e);
            }
        }
        throw new AssertionError("monitor event timed out: " + eventType);
    }

    public static void awaitCondition(BooleanSupplier condition) {
        awaitCondition(condition, DEFAULT_TIMEOUT_MS);
    }

    public static void awaitCondition(BooleanSupplier condition, long timeoutMs) {
        long deadline = System.nanoTime() + timeoutMs * 1_000_000L;
        while (System.nanoTime() < deadline) {
            if (condition.getAsBoolean()) {
                return;
            }
            try {
                Thread.sleep(10L);
            } catch (InterruptedException e) {
                Thread.currentThread().interrupt();
                throw new RuntimeException(e);
            }
        }
        throw new AssertionError("condition did not become true within " + timeoutMs + "ms");
    }

    public static void allowTcpRequestReplyCallbackHandshakeToSettle() {
        try {
            // CONNECTION_READY can arrive before the ROUTER request/reply
            // callback path finishes its peer routing-id handshake.
            Thread.sleep(100L);
        } catch (InterruptedException e) {
            Thread.currentThread().interrupt();
            throw new RuntimeException(e);
        }
    }

    private static int nextTcpPort() {
        for (int attempt = 0; attempt < 200; attempt++) {
            int port = NEXT_TCP_PORT.getAndIncrement();
            if (isBindable(port)) {
                return port;
            }
        }
        throw new IllegalStateException("failed to allocate tcp port");
    }

    private static boolean isBindable(int port) {
        try (ServerSocket server = new ServerSocket(port)) {
            server.setReuseAddress(false);
            return true;
        } catch (IOException e) {
            return false;
        }
    }
}
