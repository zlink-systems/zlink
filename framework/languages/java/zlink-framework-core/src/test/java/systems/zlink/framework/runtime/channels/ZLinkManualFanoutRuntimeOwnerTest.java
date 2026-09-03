package systems.zlink.framework.runtime.channels;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertTrue;

import java.time.Duration;
import java.util.List;
import java.util.Optional;
import java.util.concurrent.CopyOnWriteArrayList;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.Future;
import java.util.concurrent.LinkedBlockingQueue;
import java.util.concurrent.ScheduledFuture;
import java.util.concurrent.ScheduledThreadPoolExecutor;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.concurrent.atomic.AtomicReference;
import org.junit.jupiter.api.Test;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendContext;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendDealerSocket;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendPublisherSocket;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendRecvMode;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendRouterSocket;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendSocketMonitor;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendSocketMonitorEvent;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendSubscriberSocket;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendTopicMessage;
import systems.zlink.framework.runtime.internal.backend.ZLinkChannelBackendAdapter;

final class ZLinkManualFanoutRuntimeOwnerTest {
    private static final String ENDPOINT = "tcp://127.0.0.1:7001";

    @Test
    void connectionIsNotReceivableBeforeConnectCommit() throws Exception {
        try (ExecutorService lifecycle = Executors.newSingleThreadExecutor();
             Fixture fixture = new Fixture(true, false, false)) {
            fixture.runtime.start();
            Future<?> connect = lifecycle.submit(() ->
                fixture.runtime.connections("events").connect(ENDPOINT));
            ControlledSubscriber subscriber = fixture.awaitSubscriber();
            assertTrue(subscriber.connectEntered.await(1, TimeUnit.SECONDS));

            try {
                int tickCount = fixture.scheduler.tickCount.get();
                awaitCondition(() ->
                    fixture.scheduler.tickCount.get() >= tickCount + 2);
                fixture.awaitInfrastructureIdle();
                assertEquals(0, subscriber.readinessWaits.get());
                assertEquals(0, subscriber.subscribeCalls.get());
            } finally {
                subscriber.releaseConnect.countDown();
            }

            connect.get(1, TimeUnit.SECONDS);
            awaitCondition(() -> subscriber.subscribeCalls.get() > 0);
        }
    }

    @Test
    void monitorAndRuntimeCloseJoinTheAdmittedSubscriberReceive()
        throws Exception {
        try (ExecutorService lifecycle = Executors.newFixedThreadPool(2);
             Fixture fixture = new Fixture(false, true, false)) {
            fixture.runtime.start();
            fixture.runtime.connections("events").connect(ENDPOINT);
            ControlledSubscriber subscriber = fixture.awaitSubscriber();
            assertTrue(subscriber.subscribeEntered.await(1, TimeUnit.SECONDS));

            Future<?> close;
            try {
                Future<?> monitor = lifecycle.submit(() ->
                    subscriber.monitor.emit("DISCONNECTED"));
                monitor.get(1, TimeUnit.SECONDS);
                AtomicReference<Thread> closeOwner = new AtomicReference<>();
                close = lifecycle.submit(() -> {
                    closeOwner.set(Thread.currentThread());
                    fixture.runtime.close();
                });
                awaitStackFrame(
                    closeOwner, ZLinkManualFanoutRuntime.class, "awaitClose");
                assertFalse(close.isDone());
                assertEquals(0, subscriber.disconnectCalls.get());
                assertEquals(0, subscriber.monitor.closeCalls.get());
                assertEquals(0, subscriber.closeCalls.get());
            } finally {
                subscriber.releaseReceive.countDown();
            }

            close.get(2, TimeUnit.SECONDS);
            assertEquals(1, subscriber.disconnectCalls.get());
            assertEquals(1, subscriber.monitor.closeCalls.get());
            assertEquals(1, subscriber.closeCalls.get());
            assertEquals(List.of(
                "subscribe-enter", "subscribe-exit", "disconnect", "close"),
                subscriber.events);
        }
    }

    @Test
    void publicDisconnectJoinsTheAdmittedSubscriberReceive()
        throws Exception {
        try (ExecutorService lifecycle = Executors.newSingleThreadExecutor();
             Fixture fixture = new Fixture(false, true, false)) {
            fixture.runtime.start();
            fixture.runtime.connections("events").connect(ENDPOINT);
            ControlledSubscriber subscriber = fixture.awaitSubscriber();
            assertTrue(subscriber.subscribeEntered.await(1, TimeUnit.SECONDS));

            AtomicReference<Thread> disconnectOwner = new AtomicReference<>();
            Future<?> disconnect = lifecycle.submit(() -> {
                disconnectOwner.set(Thread.currentThread());
                fixture.runtime.connections("events").disconnect(ENDPOINT);
            });
            try {
                awaitStackFrame(
                    disconnectOwner, ZLinkManualFanoutRuntime.class, "awaitClose");
                assertFalse(disconnect.isDone());
                assertEquals(0, subscriber.disconnectCalls.get());
                assertEquals(0, subscriber.closeCalls.get());
            } finally {
                subscriber.releaseReceive.countDown();
            }

            disconnect.get(2, TimeUnit.SECONDS);
            assertEquals(1, subscriber.disconnectCalls.get());
            assertEquals(1, subscriber.monitor.closeCalls.get());
            assertEquals(1, subscriber.closeCalls.get());
            assertEquals(List.of(
                "subscribe-enter", "subscribe-exit", "disconnect", "close"),
                subscriber.events);
        }
    }

    private static void awaitCondition(java.util.function.BooleanSupplier condition)
        throws Exception {
        long deadline = System.nanoTime() + TimeUnit.SECONDS.toNanos(1);
        while (!condition.getAsBoolean()) {
            if (System.nanoTime() >= deadline) {
                throw new AssertionError("condition timed out");
            }
            Thread.sleep(1);
        }
    }

    private static void awaitStackFrame(
        AtomicReference<Thread> owner,
        Class<?> type,
        String methodName) throws Exception {
        long deadline = System.nanoTime() + TimeUnit.SECONDS.toNanos(1);
        while (true) {
            Thread thread = owner.get();
            if (thread != null) {
                for (StackTraceElement frame : thread.getStackTrace()) {
                    if (frame.getClassName().equals(type.getName())
                        && frame.getMethodName().equals(methodName)) {
                        return;
                    }
                }
            }
            if (System.nanoTime() >= deadline) {
                throw new AssertionError(
                    type.getSimpleName() + "." + methodName
                        + " did not block on the admitted receive");
            }
            Thread.sleep(1);
        }
    }

    private static final class Fixture implements AutoCloseable {
        private final Backend backend;
        private final TickScheduler scheduler = new TickScheduler();
        private final ExecutorService infrastructure =
            Executors.newSingleThreadExecutor();
        private final ZLinkManualFanoutRuntime runtime;

        private Fixture(
            boolean blockConnect,
            boolean blockReceive,
            boolean blockMonitorRegistration) {
            backend = new Backend(
                blockConnect, blockReceive, blockMonitorRegistration);
            runtime = new ZLinkManualFanoutRuntime(
                backend,
                socket -> ((ControlledSubscriber) socket).monitor,
                new Context(),
                scheduler,
                infrastructure,
                (channel, message) -> message.parts().forEach(Message::close));
        }

        private ControlledSubscriber awaitSubscriber() throws Exception {
            ControlledSubscriber subscriber = backend.created.poll(
                1, TimeUnit.SECONDS);
            if (subscriber == null) {
                throw new AssertionError("manual fanout subscriber was not created");
            }
            return subscriber;
        }

        private void awaitInfrastructureIdle() throws Exception {
            infrastructure.submit(() -> { }).get(1, TimeUnit.SECONDS);
        }

        @Override
        public void close() {
            backend.subscribers.forEach(subscriber -> {
                subscriber.releaseConnect.countDown();
                subscriber.releaseReceive.countDown();
                subscriber.monitor.releaseRegistration.countDown();
            });
            runtime.close();
            scheduler.shutdownNow();
            infrastructure.shutdownNow();
        }
    }

    private static final class TickScheduler extends ScheduledThreadPoolExecutor {
        private final AtomicInteger tickCount = new AtomicInteger();

        private TickScheduler() {
            super(1);
        }

        @Override
        public ScheduledFuture<?> scheduleAtFixedRate(
            Runnable command,
            long initialDelay,
            long period,
            TimeUnit unit) {
            return super.scheduleAtFixedRate(() -> {
                try {
                    command.run();
                } finally {
                    tickCount.incrementAndGet();
                }
            }, initialDelay, period, unit);
        }
    }

    private static final class Backend implements ZLinkChannelBackendAdapter {
        private final boolean blockConnect;
        private final boolean blockReceive;
        private final boolean blockMonitorRegistration;
        private final List<ControlledSubscriber> subscribers =
            new CopyOnWriteArrayList<>();
        private final LinkedBlockingQueue<ControlledSubscriber> created =
            new LinkedBlockingQueue<>();

        private Backend(
            boolean blockConnect,
            boolean blockReceive,
            boolean blockMonitorRegistration) {
            this.blockConnect = blockConnect;
            this.blockReceive = blockReceive;
            this.blockMonitorRegistration = blockMonitorRegistration;
        }

        @Override
        public ZLinkBackendSubscriberSocket createSubscriberSocket(
            ZLinkBackendContext context) {
            ControlledSubscriber subscriber = new ControlledSubscriber(
                blockConnect, blockReceive, blockMonitorRegistration);
            subscribers.add(subscriber);
            created.add(subscriber);
            return subscriber;
        }

        @Override public ZLinkBackendContext createContext() {
            throw new UnsupportedOperationException();
        }
        @Override public ZLinkBackendDealerSocket createDealerSocket(
            ZLinkBackendContext context) {
            throw new UnsupportedOperationException();
        }
        @Override public ZLinkBackendRouterSocket createRouterSocket(
            ZLinkBackendContext context) {
            throw new UnsupportedOperationException();
        }
        @Override public ZLinkBackendPublisherSocket createPublisherSocket(
            ZLinkBackendContext context) {
            throw new UnsupportedOperationException();
        }
    }

    private static final class ControlledSubscriber
        implements ZLinkBackendSubscriberSocket {
        private final boolean blockConnect;
        private final boolean blockReceive;
        private final Monitor monitor;
        private final CountDownLatch connectEntered = new CountDownLatch(1);
        private final CountDownLatch releaseConnect = new CountDownLatch(1);
        private final CountDownLatch subscribeEntered = new CountDownLatch(1);
        private final CountDownLatch releaseReceive = new CountDownLatch(1);
        private final AtomicInteger readinessWaits = new AtomicInteger();
        private final AtomicInteger subscribeCalls = new AtomicInteger();
        private final AtomicInteger connectCalls = new AtomicInteger();
        private final AtomicInteger disconnectCalls = new AtomicInteger();
        private final AtomicInteger closeCalls = new AtomicInteger();
        private final List<String> events = new CopyOnWriteArrayList<>();

        private ControlledSubscriber(
            boolean blockConnect,
            boolean blockReceive,
            boolean blockMonitorRegistration) {
            this.blockConnect = blockConnect;
            this.blockReceive = blockReceive;
            monitor = new Monitor(blockMonitorRegistration);
        }

        @Override public void setChannelName(String channelName) { }
        @Override public void setSubscription(String topic) { }

        @Override
        public ZLinkBackendTopicMessage subscribe(ZLinkBackendRecvMode mode) {
            subscribeCalls.incrementAndGet();
            if (!blockReceive) {
                return null;
            }
            events.add("subscribe-enter");
            subscribeEntered.countDown();
            awaitUninterruptibly(releaseReceive);
            events.add("subscribe-exit");
            return null;
        }

        @Override
        public boolean waitForReadable(Duration timeout) {
            readinessWaits.incrementAndGet();
            return true;
        }

        @Override
        public void connect(String endpoint) {
            connectCalls.incrementAndGet();
            connectEntered.countDown();
            if (blockConnect) {
                awaitUninterruptibly(releaseConnect);
            }
        }

        @Override
        public void disconnect(String endpoint) {
            disconnectCalls.incrementAndGet();
            events.add("disconnect");
        }

        @Override public void bind(String endpoint) {
            throw new UnsupportedOperationException();
        }
        @Override public String name() { return "manual-subscriber"; }

        @Override
        public void close() {
            closeCalls.incrementAndGet();
            events.add("close");
        }
    }

    private static void awaitUninterruptibly(CountDownLatch latch) {
        boolean interrupted = false;
        while (true) {
            try {
                latch.await();
                break;
            } catch (InterruptedException ignored) {
                interrupted = true;
            }
        }
        if (interrupted) {
            Thread.currentThread().interrupt();
        }
    }

    private static final class Monitor implements ZLinkBackendSocketMonitor {
        private final boolean blockRegistration;
        private final LinkedBlockingQueue<ZLinkBackendSocketMonitorEvent> events =
            new LinkedBlockingQueue<>();
        private final AtomicInteger closeCalls = new AtomicInteger();
        private final CountDownLatch registrationEntered = new CountDownLatch(1);
        private final CountDownLatch releaseRegistration = new CountDownLatch(1);

        private Monitor(boolean blockRegistration) {
            this.blockRegistration = blockRegistration;
        }

        private void emit(String event) {
            events.add(new ZLinkBackendSocketMonitorEvent(
                event, Optional.empty(), "", ""));
        }

        @Override public ZLinkBackendSocketMonitorEvent recv() {
            registrationEntered.countDown();
            if (blockRegistration) {
                awaitUninterruptibly(releaseRegistration);
            }
            try {
                return events.take();
            } catch (InterruptedException interrupted) {
                Thread.currentThread().interrupt();
                throw new IllegalStateException("monitor receive interrupted", interrupted);
            }
        }
        @Override public String name() { return "manual-monitor"; }
        @Override public void close() { closeCalls.incrementAndGet(); }
    }

    private static final class Context implements ZLinkBackendContext {
        @Override public void shutdown() { }
        @Override public String name() { return "manual-context"; }
        @Override public void close() { }
    }
}
