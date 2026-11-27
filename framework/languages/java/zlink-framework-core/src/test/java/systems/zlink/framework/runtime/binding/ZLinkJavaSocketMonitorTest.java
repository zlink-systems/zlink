package systems.zlink.framework.runtime.binding;

import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertTrue;

import org.junit.jupiter.api.RepeatedTest;
import org.junit.jupiter.api.Test;

import systems.zlink.contracts.core.Zlink;
import systems.zlink.contracts.eventing.MonitorEventType;
import systems.zlink.contracts.eventing.PollEvents;
import systems.zlink.contracts.eventing.Poller;
import systems.zlink.contracts.eventing.SocketMonitor;
import systems.zlink.contracts.eventing.ZlinkTimer;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendSocketMonitor;

import java.lang.reflect.Proxy;
import java.time.Duration;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.FutureTask;
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

    @RepeatedTest(value = 200, failureThreshold = 1)
    void closeWakesPollWhenSourceCloseDoesNot() throws Exception {
        var entered = new CountDownLatch(1);
        try (var context = Zlink.createContext();
                var socket = context.createStreamSocket();
                var source = socket.monitorOpen(MonitorEventType.CONNECTION_READY);
                var cleanupWake = Zlink.createTimer();
                var poller = Zlink.createPoller()) {
            poller.add(cleanupWake, 3L);
            var pollWithoutSourceWake =
                    (Poller)
                            Proxy.newProxyInstance(
                                    Poller.class.getClassLoader(),
                                    new Class<?>[] {Poller.class},
                                    (proxy, method, args) ->
                                            switch (method.getName()) {
                                                case "add" -> {
                                                    // Keep timer readiness but omit monitor
                                                    // readiness.
                                                    if (args[0] instanceof ZlinkTimer timer) {
                                                        poller.add(timer, (long) args[1]);
                                                    }
                                                    yield null;
                                                }
                                                case "wait" -> {
                                                    entered.countDown();
                                                    yield poller.wait(
                                                            (PollEvents) args[0],
                                                            (Duration) args[1]);
                                                }
                                                case "close" -> {
                                                    poller.close();
                                                    yield null;
                                                }
                                                default -> throw new AssertionError(method);
                                            });
            try (var monitor = new ZLinkJavaSocketMonitor(source, pollWithoutSourceWake)) {
                var ready =
                        new FutureTask<>(
                                () ->
                                        monitor.waitForReadable(
                                                ZLinkBackendSocketMonitor.WAIT_UNTIL_EVENT));
                Thread.ofPlatform().daemon().start(ready);
                try {
                    assertTrue(entered.await(5, TimeUnit.SECONDS));
                    assertFalse(ready.isDone());
                    var close = new FutureTask<Void>(monitor::close, null);
                    Thread.ofPlatform().daemon().start(close);
                    close.get(1, TimeUnit.SECONDS);
                    assertFalse(ready.get(1, TimeUnit.SECONDS));
                } finally {
                    if (!ready.isDone()) {
                        cleanupWake.start(Duration.ofNanos(1), 1);
                    }
                }
            }
        }
    }

    @Test
    void sourceCloseFailureKeepsItsCauseAndAttemptsPollerClose() throws Exception {
        var sourceFailure = new IllegalStateException("source close failed");
        var pollerFailure = new IllegalStateException("poller close failed");
        var closeCalls = new java.util.concurrent.atomic.AtomicInteger();
        var source =
                (SocketMonitor)
                        java.lang.reflect.Proxy.newProxyInstance(
                                SocketMonitor.class.getClassLoader(),
                                new Class<?>[] {SocketMonitor.class},
                                (proxy, method, args) -> {
                                    throw sourceFailure;
                                });
        var poller =
                (Poller)
                        java.lang.reflect.Proxy.newProxyInstance(
                                Poller.class.getClassLoader(),
                                new Class<?>[] {Poller.class},
                                (proxy, method, args) -> {
                                    if (method.getName().equals("add")) {
                                        return null;
                                    }
                                    closeCalls.incrementAndGet();
                                    throw pollerFailure;
                                });
        var monitor = new ZLinkJavaSocketMonitor(source, poller);
        var actual =
                org.junit.jupiter.api.Assertions.assertThrows(
                        IllegalStateException.class, monitor::close);
        org.junit.jupiter.api.Assertions.assertSame(sourceFailure, actual);
        org.junit.jupiter.api.Assertions.assertEquals(1, closeCalls.get());
        org.junit.jupiter.api.Assertions.assertArrayEquals(
                new Throwable[] {pollerFailure}, actual.getSuppressed());
    }

    @org.junit.jupiter.params.ParameterizedTest
    @org.junit.jupiter.params.provider.ValueSource(longs = {0, 3, 4, 5})
    void preservesMonitorValueAcrossBackendAdapter(long reason) throws Exception {
        var rid = systems.zlink.contracts.core.RoutingId.from("peer");
        var event =
                new systems.zlink.contracts.eventing.MonitorEvent(
                        MonitorEventType.DISCONNECTED,
                        reason,
                        java.util.Optional.of(rid),
                        "local",
                        "remote");
        var source =
                (SocketMonitor)
                        java.lang.reflect.Proxy.newProxyInstance(
                                SocketMonitor.class.getClassLoader(),
                                new Class<?>[] {SocketMonitor.class},
                                (proxy, method, args) ->
                                        method.getName().equals("recv") ? event : null);
        var poller =
                (Poller)
                        java.lang.reflect.Proxy.newProxyInstance(
                                Poller.class.getClassLoader(),
                                new Class<?>[] {Poller.class},
                                (proxy, method, args) -> null);
        try (var monitor = new ZLinkJavaSocketMonitor(source, poller)) {
            var observed = monitor.recvDontWait();
            org.junit.jupiter.api.Assertions.assertEquals(reason, observed.value());
            org.junit.jupiter.api.Assertions.assertEquals(
                    java.util.Optional.of(rid), observed.routingId());
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
            long deadline = System.nanoTime() + TimeUnit.SECONDS.toNanos(5);
            boolean entered = false;
            while (!entered && System.nanoTime() < deadline) {
                synchronized (monitor) {
                    entered = monitor.waiting;
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
