package systems.zlink.framework.runtime.channels;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertInstanceOf;
import static org.junit.jupiter.api.Assertions.assertThrows;
import static org.junit.jupiter.api.Assertions.assertTrue;

import java.lang.reflect.Proxy;
import java.time.Duration;
import java.time.Instant;
import java.util.ArrayList;
import java.util.Comparator;
import java.util.List;
import java.util.Optional;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionException;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.Delayed;
import java.util.concurrent.FutureTask;
import java.util.concurrent.ScheduledFuture;
import java.util.concurrent.ScheduledThreadPoolExecutor;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.concurrent.atomic.AtomicReference;
import org.junit.jupiter.api.Test;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.framework.channels.ZLinkRequestCall;
import systems.zlink.framework.errors.ZLinkFrameworkErrorKind;
import systems.zlink.framework.errors.ZLinkFrameworkException;
import systems.zlink.framework.runtime.configuration.DefaultZLinkFrameworkOptions;
import systems.zlink.framework.runtime.host.ZLinkFrameworkRuntimeState;
import systems.zlink.framework.runtime.internal.backend.*;
import systems.zlink.framework.runtime.internal.handlers.ZLinkHandlerActivator;
import systems.zlink.framework.runtime.internal.locations.ZLinkClientServerServerDescriptor;
import systems.zlink.framework.runtime.messaging.ZLinkJsonMessageSerializer;

final class ZLinkClientServerReadyWaitTest {
    @Test
    void shortCallTimeoutBoundsReadyWaitAtSubmit() throws Exception {
        try (Fixture fixture = new Fixture(Duration.ofSeconds(2))) {
            long building = fixture.time.nanoTime();
            ZLinkRequestCall call = fixture.runtime.requestToChannel("orders", new Request("short"))
                .timeout(Duration.ofMillis(150));
            assertEquals(building, fixture.time.nanoTime(),
                "creating the builder must not wait for service readiness");
            assertEquals(0, fixture.businessRequests.get());

            fixture.time.advanceBy(Duration.ofSeconds(1).toNanos());
            long started = fixture.time.nanoTime();
            assertUnavailable(call);
            assertEquals(started + Duration.ofMillis(150).toNanos(), fixture.time.nanoTime());
            assertEquals(0, fixture.businessRequests.get());
        }
    }

    @Test
    void lateReadyServerReceivesOnlyTheRemainingCallTimeout() throws Exception {
        try (Fixture fixture = new Fixture(Duration.ofSeconds(2))) {
            ZLinkRequestCall call = fixture.runtime.requestToChannel("orders", new Request("late"))
                .timeout(Duration.ofMillis(400));
            long started = fixture.time.nanoTime();
            fixture.time.schedule(fixture::admit, 150, TimeUnit.MILLISECONDS);

            assertEquals(new Reply("reply"), call.submit(Reply.class).toCompletableFuture().join());
            assertEquals(1, fixture.businessRequests.get());
            assertEquals(started + Duration.ofMillis(150).toNanos(), fixture.requestStarted);
            assertEquals(Duration.ofMillis(250), fixture.requestTimeout.get());
            assertEquals(started + Duration.ofMillis(400).toNanos(),
                fixture.requestStarted + fixture.requestTimeout.get().toNanos());
        }
    }

    @Test
    void lateReadyRequestReplyStillExpiresAtTheOriginalCallDeadline() throws Exception {
        try (Fixture fixture = new Fixture(Duration.ofSeconds(2))) {
            fixture.replyImmediately = false;
            ZLinkRequestCall call = fixture.runtime.requestToChannel("orders", new Request("pending"))
                .timeout(Duration.ofMillis(400));
            long started = fixture.time.nanoTime();
            fixture.time.schedule(fixture::admit, 150, TimeUnit.MILLISECONDS);

            CompletableFuture<Reply> reply = call.submit(Reply.class).toCompletableFuture();
            assertEquals(1, fixture.businessRequests.get());
            assertEquals(started + Duration.ofMillis(150).toNanos(), fixture.requestStarted);
            assertEquals(Duration.ofMillis(250), fixture.requestTimeout.get());
            assertFalse(reply.isDone());
            fixture.time.advanceBy(Duration.ofMillis(250).toNanos() - 1);
            assertFalse(reply.isDone(), "the request must remain pending before its original deadline");
            fixture.time.advanceBy(1);
            assertTrue(reply.isDone(), "the original call deadline must settle the request");
            CompletionException failure = assertThrows(CompletionException.class, reply::join);
            assertEquals(ZLinkFrameworkErrorKind.DEADLINE_EXCEEDED,
                assertInstanceOf(ZLinkFrameworkException.class, failure.getCause()).kind());
            assertEquals(started + Duration.ofMillis(400).toNanos(), fixture.time.nanoTime());
        }
    }

    @Test
    void readyWaitHasFiveSecondCapEvenWhenCallTimeoutExceedsChannelDefault() throws Exception {
        try (Fixture fixture = new Fixture(Duration.ofMillis(150))) {
            ZLinkRequestCall call = fixture.runtime.requestToChannel("orders", new Request("cap"))
                .timeout(Duration.ofSeconds(8));
            long started = fixture.time.nanoTime();
            assertUnavailable(call);
            assertEquals(started + Duration.ofSeconds(5).toNanos(), fixture.time.nanoTime());
            assertEquals(0, fixture.businessRequests.get());
        }
    }

    @Test
    void serverReadyAtFiveSecondCapReceivesTheRemainingCallTimeout() throws Exception {
        try (Fixture fixture = new Fixture(Duration.ofMillis(150))) {
            ZLinkRequestCall call = fixture.runtime.requestToChannel("orders", new Request("cap-ready"))
                .timeout(Duration.ofSeconds(8));
            long started = fixture.time.nanoTime();
            fixture.time.schedule(fixture::admit, 5, TimeUnit.SECONDS);

            assertEquals(new Reply("reply"), call.submit(Reply.class).toCompletableFuture().join());
            assertEquals(1, fixture.businessRequests.get());
            assertEquals(started + Duration.ofSeconds(5).toNanos(), fixture.requestStarted);
            assertEquals(Duration.ofSeconds(3), fixture.requestTimeout.get());
            assertEquals(started + Duration.ofSeconds(8).toNanos(),
                fixture.requestStarted + fixture.requestTimeout.get().toNanos());
        }
    }

    private static void assertUnavailable(ZLinkRequestCall call) {
        CompletionException failure = assertThrows(CompletionException.class,
            () -> call.submit(Reply.class).toCompletableFuture().join());
        assertEquals(ZLinkFrameworkErrorKind.UNAVAILABLE,
            assertInstanceOf(ZLinkFrameworkException.class, failure.getCause()).kind());
    }

    private record Request(String value) { }
    private record Reply(String value) { }

    private static final class Fixture implements AutoCloseable {
        private final ManualTime time = new ManualTime();
        private final CountDownLatch admissionListening = new CountDownLatch(1);
        private final CompletableFuture<ZLinkBackendReceived> admission = new CompletableFuture<>();
        private final CountDownLatch admissionStarted = new CountDownLatch(1);
        private final AtomicInteger businessRequests = new AtomicInteger();
        private final AtomicReference<Duration> requestTimeout = new AtomicReference<>();
        private volatile long requestStarted;
        private volatile boolean replyImmediately = true;
        private final ZLinkChannelRuntime runtime;

        private Fixture(Duration channelTimeout) throws Exception {
            ZLinkBackendDealerSocket dealer = (ZLinkBackendDealerSocket) Proxy.newProxyInstance(
                getClass().getClassLoader(), new Class<?>[] {ZLinkBackendDealerSocket.class},
                (proxy, method, args) -> switch (method.getName()) {
                    case "request" -> {
                        if (admissionStarted.getCount() != 0) {
                            admissionStarted.countDown();
                            yield admission;
                        }
                        requestStarted = time.nanoTime();
                        requestTimeout.set((Duration) args[1]);
                        businessRequests.incrementAndGet();
                        yield replyImmediately
                            ? CompletableFuture.completedFuture(received(Message.from("{\"value\":\"reply\"}")))
                            : new CompletableFuture<ZLinkBackendReceived>();
                    }
                    case "send" -> CompletableFuture.completedFuture(null);
                    case "recv" -> null;
                    case "waitForReadable" -> false;
                    case "name" -> "ready-wait-dealer";
                    case "hashCode" -> System.identityHashCode(proxy);
                    case "equals" -> proxy == args[0];
                    case "setReceiveFlowState", "setChannelName", "connect", "disconnect", "close" -> null;
                    default -> throw new UnsupportedOperationException(method.toString());
                });
            ZLinkBackendContext context = new ZLinkBackendContext() {
                @Override public String name() { return "ready-wait-context"; }
                @Override public void shutdown() { }
                @Override public void close() { }
            };
            ZLinkChannelBackendAdapter backend = (ZLinkChannelBackendAdapter) Proxy.newProxyInstance(
                getClass().getClassLoader(), new Class<?>[] {ZLinkChannelBackendAdapter.class},
                (proxy, method, args) -> switch (method.getName()) {
                    case "createContext" -> context;
                    case "createDealerSocket" -> dealer;
                    default -> throw new UnsupportedOperationException(method.toString());
                });
            ZLinkMonitoringBackendAdapter monitoring = socket -> new ZLinkBackendSocketMonitor() {
                private boolean emitted;
                @Override public ZLinkBackendSocketMonitorEvent recv() {
                    if (emitted) {
                        admissionListening.countDown();
                        return null;
                    }
                    emitted = true;
                    return new ZLinkBackendSocketMonitorEvent("CONNECTION_READY", Optional.empty(), "", "");
                }
                @Override public String name() { return "ready-wait-monitor"; }
                @Override public void close() { }
            };
            ZLinkBackendAdapterProvider provider = (ZLinkBackendAdapterProvider) Proxy.newProxyInstance(
                getClass().getClassLoader(), new Class<?>[] {ZLinkBackendAdapterProvider.class},
                (proxy, method, args) -> switch (method.getName()) {
                    case "createMonitoringAdapter" -> monitoring;
                    default -> throw new UnsupportedOperationException(method.toString());
                });
            DefaultZLinkFrameworkOptions options = new DefaultZLinkFrameworkOptions();
            options.setDefaultRequestTimeout(channelTimeout);
            options.addClientServerChannel("orders").client().connect("inproc://ready-wait");
            runtime = new ZLinkChannelRuntime(backend, context, true, provider,
                new ZLinkBackendAdapterOptions(channelTimeout), options.registration(),
                new ZLinkJsonMessageSerializer(), ZLinkHandlerActivator.reflection(), null,
                (ignoredBackend, ignoredKey) -> (ignoredSubmission, ignoredCleanup) -> {
                    throw new AssertionError("one-way admission is not used by this fixture");
                }, time::nanoTime, time::advanceBy, time);
            // The next monitor receive follows registration of the admission callback.
            assertTrue(admissionListening.await(1, TimeUnit.SECONDS));
        }

        private void admit() {
            admission.complete(received(Message.from(ZLinkClientServerServiceWire.encodeAdmit(
                new ZLinkClientServerServerDescriptor("orders", RoutingId.from("server"), 1, 1,
                    "inproc://ready-wait", 100, ZLinkFrameworkRuntimeState.SERVING,
                    "default", "server", 1, Instant.EPOCH), Integer.MAX_VALUE))));
        }

        private static ZLinkBackendReceived received(Message message) {
            return new ZLinkBackendReceived(Optional.empty(), Optional.empty(), Optional.empty(), List.of(message));
        }

        @Override public void close() { runtime.close(); }
    }

    private static final class ManualTime extends ScheduledThreadPoolExecutor {
        private final List<TimedTask> tasks = new ArrayList<>();
        private long nowNanos = Duration.ofSeconds(42).toNanos();

        private ManualTime() {
            super(1);
        }

        private long nanoTime() {
            return nowNanos;
        }

        @Override
        public ScheduledFuture<?> schedule(Runnable command, long delay, TimeUnit unit) {
            TimedTask task = new TimedTask(command, nowNanos + unit.toNanos(delay));
            tasks.add(task);
            return task;
        }

        private void advanceBy(long nanos) {
            assertTrue(nanos >= 0, "the monotonic clock must not move backwards");
            long target = nowNanos + nanos;
            while (true) {
                TimedTask next = tasks.stream()
                    .filter(task -> !task.isDone() && task.deadlineNanos <= target)
                    .min(Comparator.comparingLong(task -> task.deadlineNanos))
                    .orElse(null);
                if (next == null) {
                    nowNanos = target;
                    return;
                }
                tasks.remove(next);
                nowNanos = next.deadlineNanos;
                next.run();
            }
        }

        private final class TimedTask extends FutureTask<Void> implements ScheduledFuture<Void> {
            private final long deadlineNanos;

            private TimedTask(Runnable command, long deadlineNanos) {
                super(command, null);
                this.deadlineNanos = deadlineNanos;
            }

            @Override
            public long getDelay(TimeUnit unit) {
                return unit.convert(deadlineNanos - nowNanos, TimeUnit.NANOSECONDS);
            }

            @Override
            public int compareTo(Delayed other) {
                return Long.compare(getDelay(TimeUnit.NANOSECONDS), other.getDelay(TimeUnit.NANOSECONDS));
            }
        }
    }
}
