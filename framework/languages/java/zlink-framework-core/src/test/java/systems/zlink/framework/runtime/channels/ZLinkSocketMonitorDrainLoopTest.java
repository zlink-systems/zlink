package systems.zlink.framework.runtime.channels;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertSame;
import static org.junit.jupiter.api.Assertions.assertTrue;

import org.junit.jupiter.api.Test;

import systems.zlink.framework.runtime.internal.backend.ZLinkBackendSocketMonitor;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendSocketMonitorEvent;

import java.time.Duration;
import java.util.List;
import java.util.Optional;
import java.util.concurrent.CopyOnWriteArrayList;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.Semaphore;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.concurrent.atomic.AtomicReference;

final class ZLinkSocketMonitorDrainLoopTest {
    @Test
    void waitsForReadinessAndDrainsEveryReadyEventWithoutExitingOnNoData() throws Exception {
        TestMonitor monitor = new TestMonitor();
        List<String> dispatched = new CopyOnWriteArrayList<>();
        Thread loop =
                ZLinkSocketMonitorDrainLoop.start(
                        "monitor-drain-contract", monitor, event -> dispatched.add(event.event()));

        assertTrue(monitor.waitEntered.await(1, TimeUnit.SECONDS));
        assertEquals(0, monitor.recvCalls.get());

        monitor.emit("CONNECT_DELAYED", "CONNECTION_READY");
        awaitCondition(() -> dispatched.size() == 2);
        assertEquals(List.of("CONNECT_DELAYED", "CONNECTION_READY"), dispatched);

        monitor.emit("DISCONNECTED");
        awaitCondition(() -> dispatched.size() == 3);
        assertEquals(List.of("CONNECT_DELAYED", "CONNECTION_READY", "DISCONNECTED"), dispatched);
        assertTrue(loop.isAlive());

        monitor.close();
        loop.join(1_000);
        assertFalse(loop.isAlive());
    }

    @Test
    void interruptionCancelsTheLoop() throws Exception {
        TestMonitor monitor = new TestMonitor();
        Thread loop =
                ZLinkSocketMonitorDrainLoop.start(
                        "monitor-drain-cancellation", monitor, event -> {});

        assertTrue(monitor.waitEntered.await(1, TimeUnit.SECONDS));
        loop.interrupt();
        loop.join(1_000);

        assertFalse(loop.isAlive());
        assertFalse(monitor.isClosed());
    }

    @Test
    void receiveFailureIsNotSwallowed() throws Exception {
        TestMonitor monitor = new TestMonitor();
        IllegalStateException failure = new IllegalStateException("receive failed");
        monitor.receiveFailure = failure;
        AtomicReference<Throwable> uncaught = new AtomicReference<>();
        CountDownLatch failed = new CountDownLatch(1);
        Thread loop =
                ZLinkSocketMonitorDrainLoop.start("monitor-drain-failure", monitor, event -> {});
        assertTrue(monitor.waitEntered.await(1, TimeUnit.SECONDS));
        loop.setUncaughtExceptionHandler(
                (thread, thrown) -> {
                    uncaught.set(thrown);
                    failed.countDown();
                });

        monitor.emit("CONNECTION_READY");

        assertTrue(failed.await(1, TimeUnit.SECONDS));
        assertSame(failure, uncaught.get());
        loop.join(1_000);
        assertFalse(loop.isAlive());
    }

    private static void awaitCondition(java.util.function.BooleanSupplier condition)
            throws Exception {
        long deadline = System.nanoTime() + TimeUnit.SECONDS.toNanos(1);
        while (!condition.getAsBoolean() && System.nanoTime() < deadline) {
            Thread.yield();
        }
        assertTrue(condition.getAsBoolean());
    }

    private static final class TestMonitor implements ZLinkBackendSocketMonitor {
        private final java.util.concurrent.ConcurrentLinkedQueue<ZLinkBackendSocketMonitorEvent>
                events = new java.util.concurrent.ConcurrentLinkedQueue<>();
        private final Semaphore readable = new Semaphore(0);
        private final CountDownLatch waitEntered = new CountDownLatch(1);
        private final AtomicInteger recvCalls = new AtomicInteger();
        private volatile RuntimeException receiveFailure;
        private volatile boolean closed;

        private void emit(String... names) {
            for (String name : names) {
                events.add(new ZLinkBackendSocketMonitorEvent(name, Optional.empty(), "", ""));
            }
            readable.release();
        }

        @Override
        public boolean waitForReadable(Duration timeout) {
            waitEntered.countDown();
            try {
                return readable.tryAcquire(timeout.toMillis(), TimeUnit.MILLISECONDS) && !closed;
            } catch (InterruptedException interrupted) {
                Thread.currentThread().interrupt();
                return false;
            }
        }

        @Override
        public ZLinkBackendSocketMonitorEvent recvDontWait() {
            recvCalls.incrementAndGet();
            RuntimeException failure = receiveFailure;
            if (failure != null) {
                throw failure;
            }
            return events.poll();
        }

        @Override
        public boolean isClosed() {
            return closed;
        }

        @Override
        public String name() {
            return "test-monitor";
        }

        @Override
        public void close() {
            closed = true;
            readable.release();
        }
    }
}
