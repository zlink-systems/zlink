package systems.zlink.framework.runtime.binding;

import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertTrue;

import org.junit.jupiter.api.Test;

import systems.zlink.contracts.core.Zlink;
import systems.zlink.contracts.eventing.MonitorEventType;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendSocketMonitor;

import java.lang.reflect.Field;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.TimeUnit;

final class ZLinkJavaSocketMonitorTest {
    @Test
    void closeWakesAnInfiniteWaitAndRepeatedCloseIsSafe() throws Exception {
        verifyCloseWakesWait(false);
    }

    @Test
    void closeRestoresInterruptionAfterNativeWaitCompletes() throws Exception {
        verifyCloseWakesWait(true);
    }

    @Test
    void sourceCloseFailureKeepsItsCauseAndAttemptsPollerClose() throws Exception {
        var sourceFailure = new IllegalStateException("source close failed");
        var pollerFailure = new IllegalStateException("poller close failed");
        var closeCalls = new java.util.concurrent.atomic.AtomicInteger();
        try (var context = Zlink.createContext();
                var socket = context.createStreamSocket();
                var source = socket.monitorOpen(MonitorEventType.CONNECTION_READY)) {
            var monitor = new ZLinkJavaSocketMonitor(source);
            Field pollerField = ZLinkJavaSocketMonitor.class.getDeclaredField("poller");
            Field sourceField = ZLinkJavaSocketMonitor.class.getDeclaredField("monitor");
            pollerField.setAccessible(true);
            sourceField.setAccessible(true);
            var originalPoller = (systems.zlink.contracts.eventing.Poller) pollerField.get(monitor);
            try {
                sourceField.set(
                        monitor,
                        java.lang.reflect.Proxy.newProxyInstance(
                                systems.zlink.contracts.eventing.SocketMonitor.class
                                        .getClassLoader(),
                                new Class<?>[] {
                                    systems.zlink.contracts.eventing.SocketMonitor.class
                                },
                                (proxy, method, args) -> {
                                    throw sourceFailure;
                                }));
                pollerField.set(
                        monitor,
                        java.lang.reflect.Proxy.newProxyInstance(
                                systems.zlink.contracts.eventing.Poller.class.getClassLoader(),
                                new Class<?>[] {systems.zlink.contracts.eventing.Poller.class},
                                (proxy, method, args) -> {
                                    closeCalls.incrementAndGet();
                                    throw pollerFailure;
                                }));
                var actual =
                        org.junit.jupiter.api.Assertions.assertThrows(
                                IllegalStateException.class, monitor::close);
                org.junit.jupiter.api.Assertions.assertSame(sourceFailure, actual);
                org.junit.jupiter.api.Assertions.assertEquals(1, closeCalls.get());
                org.junit.jupiter.api.Assertions.assertArrayEquals(
                        new Throwable[] {pollerFailure}, actual.getSuppressed());
            } finally {
                originalPoller.close();
            }
        }
    }

    private static void verifyCloseWakesWait(boolean interruptCloser) throws Exception {
        try (var context = Zlink.createContext();
                var socket = context.createStreamSocket();
                var monitor =
                        new ZLinkJavaSocketMonitor(
                                socket.monitorOpen(MonitorEventType.CONNECTION_READY))) {
            CompletableFuture<Boolean> result = new CompletableFuture<>();
            Thread waiter =
                    Thread.ofVirtual()
                            .start(
                                    () -> {
                                        try {
                                            result.complete(
                                                    monitor.waitForReadable(
                                                            ZLinkBackendSocketMonitor
                                                                    .WAIT_UNTIL_EVENT));
                                        } catch (Throwable failure) {
                                            result.completeExceptionally(failure);
                                        }
                                    });
            Field waiting = ZLinkJavaSocketMonitor.class.getDeclaredField("waiting");
            waiting.setAccessible(true);
            long deadline = System.nanoTime() + TimeUnit.SECONDS.toNanos(5);
            boolean entered = false;
            while (!entered && System.nanoTime() < deadline) {
                synchronized (monitor) {
                    entered = waiting.getBoolean(monitor);
                }
                Thread.yield();
            }
            assertTrue(entered);
            assertFalse(result.isDone());
            var closed = new CompletableFuture<Boolean>();
            Thread.ofVirtual()
                    .start(
                            () -> {
                                try {
                                    if (interruptCloser) {
                                        Thread.currentThread().interrupt();
                                    }
                                    monitor.close();
                                    closed.complete(Thread.currentThread().isInterrupted());
                                } catch (Throwable failure) {
                                    closed.completeExceptionally(failure);
                                }
                            });
            org.junit.jupiter.api.Assertions.assertEquals(
                    interruptCloser, closed.get(5, TimeUnit.SECONDS));
            assertTrue(monitor.isClosed());
            assertFalse(result.get(5, TimeUnit.SECONDS));
            waiter.join();
            monitor.close();
            assertFalse(monitor.waitForReadable(ZLinkBackendSocketMonitor.WAIT_UNTIL_EVENT));
        }
    }
}
