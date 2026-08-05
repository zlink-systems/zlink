/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.perf;

import systems.zlink.contracts.sockets.AutoHwmProfile;
import systems.zlink.contracts.core.Context;
import systems.zlink.contracts.core.Zlink;
import systems.zlink.contracts.eventing.MonitorEventType;
import systems.zlink.contracts.eventing.SocketMonitor;
import systems.zlink.contracts.sockets.Socket;
import java.nio.file.Files;
import java.nio.file.Path;
import java.time.Duration;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicReference;

final class PerfTransport {
    private PerfTransport() {
    }

    static Context newContext(PerfUtil.Config config) {
        Context ctx = Zlink.createContext();
        if (config.ioThreads() > 0) {
            ctx.options().ioThreads(config.ioThreads());
        }
        ctx.options().blocky(benchCtxBlocky());
        ctx.options().autoHwmEnabled(benchCtxAutoHwmEnabled());
        ctx.options().autoHwmProfile(benchCtxAutoHwmProfile());
        if (config.size() > 0) {
            ctx.options().autoHwmMessageUnitBytes(config.size());
        }
        return ctx;
    }

    static boolean manualSocketOverridesEnabled(String suite) {
        String suiteEnv = "multi".equals(suite)
            ? "PERF_MULTI_ALLOW_MANUAL_SOCKET_OVERRIDES"
            : "PERF_SINGLE_ALLOW_MANUAL_SOCKET_OVERRIDES";
        return PerfUtil.intEnv(suiteEnv, 0) > 0
            || PerfUtil.intEnv("PERF_ALLOW_MANUAL_SOCKET_OVERRIDES", 0) > 0;
    }

    private static boolean benchCtxBlocky() {
        return PerfUtil.intEnv("PERF_CTX_BLOCKY", 0) != 0;
    }

    private static boolean benchCtxAutoHwmEnabled() {
        return PerfUtil.intEnv("PERF_CTX_AUTO_HWM_ENABLE", 1) != 0;
    }

    private static AutoHwmProfile benchCtxAutoHwmProfile() {
        String value = System.getenv("PERF_CTX_AUTO_HWM_PROFILE");
        if (value == null || value.isBlank()) {
            value = System.getenv("PERF_AUTO_HWM_PROFILE");
        }
        if (value == null || value.isBlank()) {
            return AutoHwmProfile.BALANCED;
        }
        return switch (value.toLowerCase(java.util.Locale.ROOT)) {
            case "compact" -> AutoHwmProfile.COMPACT;
            case "low_latency", "low-latency" -> AutoHwmProfile.LOW_LATENCY;
            case "throughput" -> AutoHwmProfile.THROUGHPUT;
            default -> AutoHwmProfile.BALANCED;
        };
    }

    static void configureServerTls(Socket socket, String transport) {
        if (!isTlsTransport(transport)) {
            return;
        }
        socket.setTlsServer(cert("server.crt"), cert("server.key"), false);
    }

    static void configureClientTls(Socket socket, String transport) {
        if (!isTlsTransport(transport)) {
            return;
        }
        socket.setTlsClient(cert("ca.crt"), "localhost", true);
    }

    static void applySocketOptions(Socket socket, PerfUtil.Config config) {
        socket.options().linger(Duration.ZERO);
        // C parity: the C perf reference relies on the core default
        // TCP_NODELAY=1 for tcp sockets (and the multi suite sets it
        // explicitly in apply_benchmark_socket_options). The Java binding
        // does not apply that default, so small-message tcp publishes were
        // Nagle-batched, collapsing PUBSUB/tcp (single and multi) throughput
        // ~4x vs C while inproc/tls/ws were unaffected. Set it explicitly for
        // tcp to match the C reference.
        if ("tcp".equals(config.transport())) {
            socket.options().tcpNoDelay(true);
        }
        if ("multi".equals(config.suite())) {
            if (manualSocketOverridesEnabled(config.suite())) {
                applyManualSocketSizing(socket, config);
            }
        } else {
            if (manualSocketOverridesEnabled(config.suite())) {
                applyManualSocketSizing(socket, config);
            }
        }
        if (config.sendTimeoutMs() >= 0) {
            socket.options().sendTimeout(Duration.ofMillis(config.sendTimeoutMs()));
        }
        if (config.recvTimeoutMs() >= 0) {
            socket.options().recvTimeout(Duration.ofMillis(config.recvTimeoutMs()));
        }
    }

    private static void applyManualSocketSizing(Socket socket, PerfUtil.Config config) {
        if (config.sendHwm() != 0L) {
            socket.options().sendHwm(config.sendHwm());
        }
        if (config.recvHwm() != 0L) {
            socket.options().recvHwm(config.recvHwm());
        }
        if (config.sendBuffer() > 0) {
            socket.options().sendBuffer(config.sendBuffer());
        }
        if (config.recvBuffer() > 0) {
            socket.options().recvBuffer(config.recvBuffer());
        }
    }

    static void recalculateAutoHwm(Context ctx) {
        ctx.recalculateAutoHwm();
    }

    static void applyMonitorOptions(SocketMonitor monitor, PerfUtil.Config config) {
        // The aligned SocketMonitor surface does not accept generic HWM tuning.
        // Perf runners keep the hook for parity, but unsupported monitor options
        // must degrade to a no-op instead of failing startup.
    }

    static void await(CountDownLatch latch, String label, Duration timeout) {
        try {
            if (!latch.await(timeout.toMillis(), TimeUnit.MILLISECONDS)) {
                throw new IllegalStateException(label + " timed out");
            }
        } catch (InterruptedException ex) {
            Thread.currentThread().interrupt();
            throw new IllegalStateException(label + " interrupted", ex);
        }
    }

    static void join(Thread thread, String label, Duration timeout) {
        try {
            thread.join(timeout.toMillis());
            if (thread.isAlive()) {
                throw new IllegalStateException(label + " timed out");
            }
        } catch (InterruptedException ex) {
            Thread.currentThread().interrupt();
            throw new IllegalStateException(label + " interrupted", ex);
        }
    }

    static void waitForMonitorEvent(SocketMonitor monitor,
                                    MonitorEventType expectedEvent,
                                    int expectedCount, Duration timeout,
                                    String label) {
        CountDownLatch done = new CountDownLatch(1);
        AtomicReference<Throwable> failure = new AtomicReference<>();
        Thread waiter = new Thread(() -> {
            try {
                int seen = 0;
                while (seen < expectedCount) {
                    var event = monitor.recv();
                    if (event.event() != expectedEvent) {
                        continue;
                    }
                    seen++;
                }
            } catch (Throwable ex) {
                failure.compareAndSet(null, ex);
            } finally {
                done.countDown();
            }
        }, "perf-monitor-wait");
        waiter.setDaemon(true);
        waiter.start();
        await(done, label, timeout);
        Throwable error = failure.get();
        if (error != null) {
            throw new IllegalStateException(label + " failed", error);
        }
    }

    private static boolean isTlsTransport(String transport) {
        return "tls".equals(transport) || "wss".equals(transport);
    }

    private static String cert(String name) {
        Path cwd = Path.of("").toAbsolutePath().normalize();
        Path current = cwd;
        while (current != null) {
            Path candidate = current.resolve("tests").resolve("certs").resolve(name);
            if (Files.exists(candidate)) {
                return candidate.toAbsolutePath().toString();
            }
            current = current.getParent();
        }
        return cwd.resolve("tests").resolve("certs").resolve(name)
            .toAbsolutePath().toString();
    }
}
