package systems.zlink.framework.runtime.internal.backend;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertSame;
import static org.junit.jupiter.api.Assertions.assertTrue;

import org.junit.jupiter.api.Test;

import java.lang.foreign.Arena;
import java.lang.foreign.FunctionDescriptor;
import java.lang.foreign.Linker;
import java.lang.foreign.MemorySegment;
import java.lang.foreign.SymbolLookup;
import java.lang.foreign.ValueLayout;
import java.lang.invoke.MethodHandle;
import java.time.Duration;
import java.util.ArrayList;
import java.util.List;
import java.util.Optional;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CopyOnWriteArrayList;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.Semaphore;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.concurrent.atomic.AtomicReference;

final class ZLinkSocketMonitorDrainLoopTest {
    private static final Duration NATIVE_WAIT = Duration.ofSeconds(2);
    private static final Duration PROGRESS_TIMEOUT = Duration.ofMillis(500);
    private static final Duration LOOP_JOIN_TIMEOUT = Duration.ofSeconds(3);

    @Test
    void nativeMonitorWaitsLeaveVirtualTasksAbleToProgress() throws Exception {
        int carriers =
                Integer.getInteger(
                        "jdk.virtualThreadScheduler.parallelism",
                        Runtime.getRuntime().availableProcessors());
        List<TestMonitor> monitors = new ArrayList<>();
        List<Thread> loops = new ArrayList<>();
        Runnable nativeWait = nativeWait();
        try {
            for (int index = 0; index < carriers; index++) {
                TestMonitor monitor = new TestMonitor();
                monitor.nativeWait = nativeWait;
                monitors.add(monitor);
                loops.add(
                        ZLinkSocketMonitorDrainLoop.start(
                                "native-monitor-progress-" + index, monitor, event -> {}));
                assertTrue(monitor.waitEntered.await(1, TimeUnit.SECONDS));
            }
            CompletableFuture<Void> progress = new CompletableFuture<>();
            Thread.ofVirtual().start(() -> progress.complete(null));
            progress.get(PROGRESS_TIMEOUT.toMillis(), TimeUnit.MILLISECONDS);
        } finally {
            monitors.forEach(TestMonitor::close);
            for (Thread loop : loops) {
                loop.join(LOOP_JOIN_TIMEOUT.toMillis());
                assertFalse(loop.isAlive());
            }
        }
    }

    private static Runnable nativeWait() {
        boolean windows = System.getProperty("os.name").startsWith("Windows");
        SymbolLookup lookup =
                windows
                        ? SymbolLookup.libraryLookup("kernel32", Arena.global())
                        : Linker.nativeLinker().defaultLookup();
        MethodHandle wait =
                Linker.nativeLinker()
                        .downcallHandle(
                                lookup.find(windows ? "Sleep" : "poll").orElseThrow(),
                                windows
                                        ? FunctionDescriptor.ofVoid(ValueLayout.JAVA_INT)
                                        : FunctionDescriptor.of(
                                                ValueLayout.JAVA_INT,
                                                ValueLayout.ADDRESS,
                                                ValueLayout.JAVA_LONG,
                                                ValueLayout.JAVA_INT));
        int nativeWaitMillis = Math.toIntExact(NATIVE_WAIT.toMillis());
        return () -> {
            try {
                if (windows) {
                    wait.invokeExact(nativeWaitMillis);
                } else {
                    int ignored = (int) wait.invokeExact(MemorySegment.NULL, 0L, nativeWaitMillis);
                }
            } catch (Throwable failure) {
                throw new AssertionError("native monitor wait failed", failure);
            }
        };
    }

    @Test
    void waitsForReadinessAndDrainsEveryReadyEventWithoutExitingOnNoData() throws Exception {
        TestMonitor monitor = new TestMonitor();
        List<String> dispatched = new CopyOnWriteArrayList<>();
        Thread loop =
                ZLinkSocketMonitorDrainLoop.start(
                        "monitor-drain-contract", monitor, event -> dispatched.add(event.event()));

        assertTrue(monitor.waitEntered.await(1, TimeUnit.SECONDS));
        assertEquals(0, monitor.recvCalls.get());
        assertEquals(1, monitor.waitCalls.get());

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
        monitor.close();
        loop.interrupt();
        loop.join(1_000);

        assertFalse(loop.isAlive());
        assertTrue(monitor.isClosed());
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
        private final AtomicInteger waitCalls = new AtomicInteger();
        private volatile RuntimeException receiveFailure;
        private Runnable nativeWait;
        private volatile boolean closed;

        private void emit(String... names) {
            for (String name : names) {
                events.add(new ZLinkBackendSocketMonitorEvent(name, Optional.empty(), "", ""));
            }
            readable.release();
        }

        @Override
        public boolean waitForReadable(Duration timeout) {
            assertEquals(ZLinkBackendSocketMonitor.WAIT_UNTIL_EVENT, timeout);
            waitCalls.incrementAndGet();
            waitEntered.countDown();
            if (nativeWait != null) {
                nativeWait.run();
                return !closed;
            }
            try {
                readable.acquire();
                return !closed;
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
