package systems.zlink.framework.runtime.binding;

import static org.junit.jupiter.api.Assertions.*;

import org.junit.jupiter.api.Test;

import systems.zlink.contracts.core.Context;
import systems.zlink.contracts.core.Zlink;
import systems.zlink.contracts.eventing.*;
import systems.zlink.contracts.sockets.RouterSocket;
import systems.zlink.contracts.sockets.Socket;

import java.time.Duration;
import java.util.List;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CopyOnWriteArrayList;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.Executors;
import java.util.concurrent.ForkJoinPool;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.concurrent.atomic.AtomicInteger;

final class ZLinkJavaMeshReceiveWakeTest {
    private static final Duration INFINITE = Duration.ofMillis(-1);

    @Test
    void timerCreationFailureClosesTheExistingPoller() {
        try (var context = Zlink.createContext();
                var socket = context.createRouterSocket()) {
            var order = new CopyOnWriteArrayList<String>();
            var poller = new ProbePoller(order);
            var failure = new IllegalStateException("timer creation failed");
            assertSame(
                    failure,
                    assertThrows(
                            IllegalStateException.class,
                            () ->
                                    new ZLinkJavaSocketReceivePoller(
                                            socket,
                                            false,
                                            true,
                                            () -> poller,
                                            () -> {
                                                throw failure;
                                            })));
            assertEquals(List.of("poller-close"), order);
        }
    }

    @Test
    void absentDeadlineDoesNotArmAnyTimer() {
        try (var fixture = new Fixture()) {
            fixture.receive.prepareManagementWait(Long.MAX_VALUE, 0);
            assertEquals(0, fixture.receive.waitForReady(Duration.ZERO));
            assertEquals(0, fixture.timer.starts.get());
            assertEquals(0, fixture.timer.stops.get());
        }
    }

    @Test
    void unchangedLongDeadlineArmsTheExistingTimerOnce() {
        try (var fixture = new Fixture()) {
            long deadline = Duration.ofSeconds(30).toNanos();
            fixture.receive.prepareManagementWait(deadline, 0);
            fixture.receive.prepareManagementWait(deadline, Duration.ofSeconds(1).toNanos());
            assertEquals(List.of(Duration.ofSeconds(30)), fixture.timer.intervals);
            assertEquals(1, fixture.timer.starts.get());
            fixture.receive.prepareManagementWait(Long.MAX_VALUE, 0);
            assertEquals(1, fixture.timer.stops.get());
        }
    }

    @Test
    void deadlineRegistrationCannotOverwriteAPendingNotification() {
        try (var fixture = new Fixture()) {
            fixture.receive.signal();
            long deadline = Duration.ofSeconds(30).toNanos();
            fixture.receive.prepareManagementWait(deadline, 0);
            assertEquals(List.of(Duration.ofNanos(1)), fixture.timer.intervals);
            assertEquals(0, fixture.receive.waitForReady(INFINITE));
            fixture.receive.prepareManagementWait(deadline, Duration.ofSeconds(1).toNanos());
            assertEquals(
                    List.of(Duration.ofNanos(1), Duration.ofSeconds(29)), fixture.timer.intervals);
        }
    }

    @Test
    void queuedWakeUsesOneShotAndIsConsumedWithoutApplicationReadiness() {
        try (var fixture = new Fixture()) {
            fixture.receive.signal();
            assertEquals(0, fixture.receive.waitForReady(INFINITE));
            assertEquals(1, fixture.timer.starts.get());
            assertEquals(1, fixture.timer.receives.get());
            assertEquals(2, fixture.poller.size());
            assertEquals(0, fixture.receive.waitForReady(Duration.ZERO));
            assertEquals(1, fixture.timer.receives.get());
        }
    }

    @Test
    void signalReleasesTheExistingBlockedReceiveMonitor() throws Exception {
        try (var workers = Executors.newFixedThreadPool(2);
                var fixture = new Fixture()) {
            var pending =
                    CompletableFuture.supplyAsync(
                            () -> fixture.receive.waitForReady(INFINITE), workers);
            fixture.poller.entered.get(5, TimeUnit.SECONDS);
            fixture.receive.signal();
            assertEquals(0, pending.get(5, TimeUnit.SECONDS));
            assertEquals(1, fixture.timer.starts.get());
            assertEquals(1, fixture.timer.receives.get());
        }
    }

    @Test
    void closeWakesBeforeTakingTheReceiveMonitorAndDisposesAfterWait() throws Exception {
        // Native wait occupies a worker until close signals it. Both operations need workers
        // owned by this test, even when unrelated work occupies the common pool.
        try (var workers = Executors.newFixedThreadPool(2);
                var fixture = new Fixture()) {
            var pending =
                    CompletableFuture.supplyAsync(
                            () -> fixture.receive.waitForReady(INFINITE), workers);
            fixture.poller.entered.get(5, TimeUnit.SECONDS);
            var closing = CompletableFuture.runAsync(fixture.receive::close, workers);
            closing.get(5, TimeUnit.SECONDS);
            assertEquals(0, pending.get(5, TimeUnit.SECONDS));
            assertEquals(1, fixture.timer.starts.get());
            assertEquals(1, fixture.timer.receives.get());
            assertEquals(List.of("wait-exit", "poller-close", "timer-close"), fixture.order);
            fixture.receive.signal();
            assertEquals(1, fixture.timer.starts.get(), "closed owner cannot start another wake");
        }
    }

    @Test
    void closeWakesWhenTheCommonPoolHasOnlyOneAvailableWorker() throws Exception {
        int blockers = ForkJoinPool.getCommonPoolParallelism() - 1;
        var started = new CountDownLatch(blockers);
        var release = new CountDownLatch(1);
        var finished = new CountDownLatch(blockers);
        try {
            for (int index = 0; index < blockers; index++) {
                ForkJoinPool.commonPool()
                        .execute(
                                () -> {
                                    started.countDown();
                                    try {
                                        release.await();
                                    } catch (InterruptedException error) {
                                        Thread.currentThread().interrupt();
                                    } finally {
                                        finished.countDown();
                                    }
                                });
            }
            assertTrue(started.await(5, TimeUnit.SECONDS));
            closeWakesBeforeTakingTheReceiveMonitorAndDisposesAfterWait();
        } finally {
            release.countDown();
            assertTrue(finished.await(5, TimeUnit.SECONDS));
        }
    }

    private static final class Fixture implements AutoCloseable {
        final Context context = Zlink.createContext();
        final RouterSocket socket = context.createRouterSocket();
        final List<String> order = new CopyOnWriteArrayList<>();
        final CountingTimer timer = new CountingTimer(order);
        final ProbePoller poller = new ProbePoller(order);
        final ZLinkJavaSocketReceivePoller receive =
                new ZLinkJavaSocketReceivePoller(socket, false, false, () -> poller, () -> timer);

        @Override
        public void close() {
            receive.close();
            socket.close();
            context.close();
        }
    }

    private static final class CountingTimer implements ZlinkTimer {
        final ZlinkTimer delegate = Zlink.createTimer();
        final AtomicInteger starts = new AtomicInteger();
        final AtomicInteger receives = new AtomicInteger();
        final AtomicInteger stops = new AtomicInteger();
        final List<Duration> intervals = new CopyOnWriteArrayList<>();
        final List<String> order;

        CountingTimer(List<String> order) {
            this.order = order;
        }

        @Override
        public void start(Duration interval, long repeatCount) {
            assertEquals(1, repeatCount);
            intervals.add(interval);
            starts.incrementAndGet();
            delegate.start(interval, repeatCount);
        }

        @Override
        public void stop() {
            stops.incrementAndGet();
            delegate.stop();
        }

        @Override
        public long recv() {
            receives.incrementAndGet();
            return delegate.recv();
        }

        @Override
        public void close() {
            order.add("timer-close");
            delegate.close();
        }
    }

    private static final class ProbePoller implements Poller {
        final Poller delegate = Zlink.createPoller();
        final CompletableFuture<Void> entered = new CompletableFuture<>();
        final AtomicBoolean waiting = new AtomicBoolean();
        final List<String> order;

        ProbePoller(List<String> order) {
            this.order = order;
        }

        @Override
        public void add(Socket socket, long slot, PollEventFlags... events) {
            delegate.add(socket, slot, events);
        }

        @Override
        public void add(ZlinkTimer timer, long slot) {
            delegate.add(((CountingTimer) timer).delegate, slot);
        }

        @Override
        public int wait(PollEvents events, Duration timeout) {
            waiting.set(true);
            entered.complete(null);
            try {
                return delegate.wait(events, timeout);
            } finally {
                waiting.set(false);
                order.add("wait-exit");
            }
        }

        @Override
        public int size() {
            return delegate.size();
        }

        @Override
        public void close() {
            assertFalse(waiting.get());
            order.add("poller-close");
            delegate.close();
        }

        @Override
        public void add(SocketMonitor monitor, long slot, PollEventFlags... events) {
            throw new UnsupportedOperationException();
        }

        @Override
        public void addFd(int fd, long slot, PollEventFlags... events) {
            throw new UnsupportedOperationException();
        }

        @Override
        public void modify(Socket socket, PollEventFlags... events) {
            throw new UnsupportedOperationException();
        }

        @Override
        public void modify(SocketMonitor monitor, PollEventFlags... events) {
            throw new UnsupportedOperationException();
        }

        @Override
        public void modifyFd(int fd, PollEventFlags... events) {
            throw new UnsupportedOperationException();
        }

        @Override
        public boolean remove(Socket socket) {
            throw new UnsupportedOperationException();
        }

        @Override
        public boolean remove(SocketMonitor monitor) {
            throw new UnsupportedOperationException();
        }

        @Override
        public boolean remove(int fd) {
            throw new UnsupportedOperationException();
        }

        @Override
        public boolean remove(ZlinkTimer timer) {
            throw new UnsupportedOperationException();
        }

        @Override
        public void clear() {
            throw new UnsupportedOperationException();
        }
    }
}
