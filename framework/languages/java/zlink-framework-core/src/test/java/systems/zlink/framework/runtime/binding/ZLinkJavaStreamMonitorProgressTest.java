package systems.zlink.framework.runtime.binding;

import static org.junit.jupiter.api.Assertions.assertTrue;

import org.junit.jupiter.api.Test;

import systems.zlink.contracts.core.Zlink;

import java.lang.reflect.Field;
import java.time.Duration;
import java.util.ArrayList;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.TimeUnit;

final class ZLinkJavaStreamMonitorProgressTest {
    private static final Duration TEST_TIMEOUT = Duration.ofSeconds(5);

    @Test
    void idleNativeMonitorsDoNotPreventAnotherSocketFromBinding() throws Exception {
        int carriers =
                Integer.getInteger(
                        "jdk.virtualThreadScheduler.parallelism",
                        Runtime.getRuntime().availableProcessors());
        var streams = new ArrayList<ZLinkJavaStreamSocket>();
        var monitors = new ArrayList<ZLinkJavaSocketMonitor>();
        Field monitorField = ZLinkJavaStreamSocket.class.getDeclaredField("monitor");
        monitorField.setAccessible(true);
        Field waitingField = ZLinkJavaSocketMonitor.class.getDeclaredField("waiting");
        waitingField.setAccessible(true);
        try (var context = Zlink.createContext()) {
            var candidate = new ZLinkJavaStreamSocket(context.createStreamSocket(), null);
            try {
                for (int index = 0; index < carriers; index++) {
                    var stream = new ZLinkJavaStreamSocket(context.createStreamSocket(), null);
                    streams.add(stream);
                    stream.onTransportError((rid, event, code, message) -> {});
                    var monitor = (ZLinkJavaSocketMonitor) monitorField.get(stream);
                    monitors.add(monitor);
                    long deadline = System.nanoTime() + TEST_TIMEOUT.toNanos();
                    boolean waiting = false;
                    while (!waiting && System.nanoTime() < deadline) {
                        synchronized (monitor) {
                            waiting = waitingField.getBoolean(monitor);
                        }
                        if (!waiting) Thread.sleep(1);
                    }
                    assertTrue(waiting, "native monitor receiver did not start");
                }
                var bound = new CompletableFuture<Void>();
                Thread.ofPlatform()
                        .daemon()
                        .start(
                                () -> {
                                    try {
                                        candidate.bind("tcp://127.0.0.1:0");
                                        bound.complete(null);
                                    } catch (RuntimeException failure) {
                                        bound.completeExceptionally(failure);
                                    }
                                });
                bound.get(TEST_TIMEOUT.toMillis(), TimeUnit.MILLISECONDS);
            } finally {
                // Closing the native sources releases blocked carriers even after a failed
                // assertion.
                for (var monitor : monitors) monitor.close();
                candidate.close();
                for (var stream : streams) stream.close();
            }
        }
    }
}
