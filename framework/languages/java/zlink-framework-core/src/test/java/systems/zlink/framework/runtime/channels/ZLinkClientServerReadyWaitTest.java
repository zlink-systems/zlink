package systems.zlink.framework.runtime.channels;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertInstanceOf;
import static org.junit.jupiter.api.Assertions.assertThrows;
import static org.junit.jupiter.api.Assertions.assertTrue;

import static systems.zlink.framework.runtime.channels.ZLinkChannelSubmissionAssertions.assertSubmitFailure;

import org.junit.jupiter.api.Test;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.errors.ZlinkRequestException;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.contracts.sockets.RequestResult;
import systems.zlink.framework.channels.ZLinkRequestCall;
import systems.zlink.framework.errors.ZLinkFrameworkErrorKind;
import systems.zlink.framework.errors.ZLinkFrameworkException;
import systems.zlink.framework.runtime.configuration.DefaultZLinkFrameworkOptions;
import systems.zlink.framework.runtime.host.ZLinkFrameworkRuntimeState;
import systems.zlink.framework.runtime.internal.backend.*;
import systems.zlink.framework.runtime.internal.handlers.ZLinkHandlerActivator;
import systems.zlink.framework.runtime.internal.locations.ZLinkClientServerServerDescriptor;
import systems.zlink.framework.runtime.messaging.ZLinkJsonMessageSerializer;

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
import java.util.concurrent.Semaphore;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.concurrent.atomic.AtomicReference;

final class ZLinkClientServerReadyWaitTest {
    @Test
    void publicPendingCapacitySendWaitsBeyondThreeSecondsAndCompletesOnce() throws Exception {
        try (Fixture fixture = new Fixture(Duration.ofSeconds(2))) {
            fixture.sendAdmission = new CompletableFuture<>();
            fixture.admit();
            var completed = new AtomicInteger();
            var pending =
                    fixture.runtime
                            .sendToChannel("orders", new Request("capacity"))
                            .submit()
                            .whenComplete((value, failure) -> completed.incrementAndGet())
                            .toCompletableFuture();
            Thread.sleep(3100);
            assertFalse(pending.isDone());
            fixture.sendAdmission.complete(null);
            pending.get(1, TimeUnit.SECONDS);
            assertEquals(1, completed.get());
            assertEquals(1, fixture.businessSends.get());
        }
    }

    @Test
    void publicPendingCapacitySendRouteRemovalIsUnavailable() throws Exception {
        try (Fixture fixture = new Fixture(Duration.ofSeconds(2))) {
            fixture.sendAdmission = new CompletableFuture<>();
            fixture.admit();
            var pending =
                    fixture.runtime
                            .sendToChannel("orders", new Request("removed"))
                            .submit()
                            .toCompletableFuture();
            assertFalse(pending.isDone());
            fixture.sendAdmission.completeExceptionally(
                    new systems.zlink.contracts.errors.ZlinkSubmitException(
                            systems.zlink.contracts.sockets.SubmitResult.NOT_FOUND));
            var failure = assertThrows(CompletionException.class, pending::join);
            assertEquals(
                    ZLinkFrameworkErrorKind.UNAVAILABLE,
                    assertInstanceOf(ZLinkFrameworkException.class, failure.getCause()).kind());
            fixture.runtime.channelSocketRegistry().signalTopologyChanged();
            assertEquals(1, fixture.businessSends.get());
        }
    }

    @Test
    void publicPendingCapacitySendSocketCloseIsShuttingDown() throws Exception {
        try (Fixture fixture = new Fixture(Duration.ofSeconds(2))) {
            fixture.sendAdmission = new CompletableFuture<>();
            fixture.admit();
            var pending =
                    fixture.runtime
                            .sendToChannel("orders", new Request("closed"))
                            .submit()
                            .toCompletableFuture();
            assertFalse(pending.isDone());
            fixture.runtime.close();
            var failure = assertThrows(CompletionException.class, pending::join);
            assertEquals(
                    ZLinkFrameworkErrorKind.SHUTTING_DOWN,
                    assertInstanceOf(ZLinkFrameworkException.class, failure.getCause()).kind());
            assertEquals(1, fixture.businessSends.get());
        }
    }

    @Test
    void admissionTimeoutAfterBeginCloseDoesNotRestartHello() throws Exception {
        Fixture fixture = new Fixture(Duration.ofSeconds(1));
        fixture.runtime.beginClose();
        fixture.admission.completeExceptionally(new ZlinkRequestException(RequestResult.TIMED_OUT));
        fixture.close();
        assertEquals(1, fixture.helloRequests.get());
        assertEquals(1, fixture.connects.get());
    }

    @Test
    void admissionTimeoutRestartsHelloOnTheSamePhysicalConnection() throws Exception {
        try (Fixture fixture = new Fixture(Duration.ofSeconds(1))) {
            fixture.admission.completeExceptionally(
                    new CompletionException(new ZlinkRequestException(RequestResult.TIMED_OUT)));
            assertTrue(fixture.secondAdmissionStarted.await(1, TimeUnit.SECONDS));
            fixture.admit();

            assertEquals(
                    new Reply("reply"),
                    fixture.runtime
                            .requestToChannel("orders", new Request("recovered"))
                            .submit(Reply.class)
                            .toCompletableFuture()
                            .join());
            assertEquals(2, fixture.helloRequests.get());
            assertEquals(1, fixture.connects.get());
            assertEquals(0, fixture.disconnects.get());
        }
    }

    @Test
    void requestTimeoutIncludesClientServerReadiness() throws Exception {
        try (Fixture fixture = new Fixture(Duration.ofSeconds(2))) {
            ZLinkRequestCall call =
                    fixture.runtime
                            .requestToChannel("orders", new Request("short"))
                            .timeout(Duration.ofMillis(150));
            long started = fixture.time.nanoTime();
            assertEquals(0, fixture.businessRequests.get());

            CompletableFuture<Reply> reply = call.submit(Reply.class).toCompletableFuture();
            assertFalse(reply.isDone(), "admission must wait until its family deadline");
            assertDeadlineExceeded(reply);
            // 02-channel-transport/02-channel-messaging.ko.md:172: admission uses its own deadline.
            // No business request starts, so the independent call clock and its full timeout budget
            // are untouched.
            assertEquals(started, fixture.time.nanoTime());
            assertEquals(0, fixture.businessRequests.get());
        }
    }

    @Test
    void readinessConsumesRequestBudget() throws Exception {
        try (Fixture fixture = new Fixture(Duration.ofSeconds(2))) {
            ZLinkRequestCall call =
                    fixture.runtime
                            .requestToChannel("orders", new Request("late"))
                            .timeout(Duration.ofMillis(400));
            long started = fixture.time.nanoTime();
            // 02-channel-transport/02-channel-messaging.ko.md:172: readiness waits within a
            // separate admission deadline.
            CompletableFuture<Reply> reply = call.submit(Reply.class).toCompletableFuture();
            assertFalse(reply.isDone());
            assertEquals(0, fixture.businessRequests.get());
            fixture.time.advanceBy(Duration.ofMillis(150).toNanos());
            fixture.admit();

            assertEquals(new Reply("reply"), reply.join());
            assertEquals(1, fixture.businessRequests.get());
            assertEquals(started + Duration.ofMillis(150).toNanos(), fixture.requestStarted);
            assertEquals(Duration.ofMillis(250), fixture.requestTimeout.get());
            assertEquals(
                    started + Duration.ofMillis(400).toNanos(),
                    fixture.requestStarted + fixture.requestTimeout.get().toNanos());
        }
    }

    @Test
    void replyUsesBudgetRemainingAfterReadiness() throws Exception {
        try (Fixture fixture = new Fixture(Duration.ofSeconds(2))) {
            fixture.replyImmediately = false;
            ZLinkRequestCall call =
                    fixture.runtime
                            .requestToChannel("orders", new Request("pending"))
                            .timeout(Duration.ofMillis(400));
            long started = fixture.time.nanoTime();

            // 02-channel-transport/02-channel-messaging.ko.md:172: readiness waits within a
            // separate admission deadline.
            CompletableFuture<Reply> reply = call.submit(Reply.class).toCompletableFuture();
            assertFalse(reply.isDone());
            assertEquals(0, fixture.businessRequests.get());
            fixture.time.advanceBy(Duration.ofMillis(150).toNanos());
            fixture.admit();
            assertTrue(fixture.businessRequestStarted.await(1, TimeUnit.SECONDS));
            assertEquals(1, fixture.businessRequests.get());
            assertEquals(started + Duration.ofMillis(150).toNanos(), fixture.requestStarted);
            assertEquals(Duration.ofMillis(250), fixture.requestTimeout.get());
            assertFalse(reply.isDone());
            fixture.time.advanceBy(Duration.ofMillis(250).toNanos() - 1);
            assertFalse(
                    reply.isDone(), "the request must remain pending before its reply deadline");
            fixture.time.advanceBy(1);
            // E5 publishes the due terminal on the shared completion dispatcher.
            // Keep virtual time at the exact deadline while that turn completes.
            reply.handle((value, failure) -> null).get(1, TimeUnit.SECONDS);
            assertTrue(reply.isDone(), "the original call deadline must settle the request");
            CompletionException failure = assertThrows(CompletionException.class, reply::join);
            assertEquals(
                    ZLinkFrameworkErrorKind.DEADLINE_EXCEEDED,
                    assertInstanceOf(ZLinkFrameworkException.class, failure.getCause()).kind());
            assertEquals(started + Duration.ofMillis(400).toNanos(), fixture.time.nanoTime());
        }
    }

    @Test
    void publicSendWaitsBeyondThreeSecondsThenAdmitsExactlyOnce() throws Exception {
        try (Fixture fixture = new Fixture(Duration.ofSeconds(2))) {
            var pending =
                    fixture.runtime
                            .sendToChannel("orders", new Request("held"))
                            .submit()
                            .toCompletableFuture();
            Thread.sleep(3100);
            assertFalse(pending.isDone());
            fixture.admit();
            pending.get(1, TimeUnit.SECONDS);
            assertEquals(1, fixture.businessSends.get());
            fixture.runtime.channelSocketRegistry().signalTopologyChanged();
            assertEquals(1, fixture.businessSends.get());
        }
    }

    @Test
    void publicSendCallerCancellationDoesNotCancelReadiness() throws Exception {
        try (Fixture fixture = new Fixture(Duration.ofSeconds(2))) {
            var pending =
                    fixture.runtime
                            .sendToChannel("orders", new Request("held"))
                            .submit()
                            .toCompletableFuture();
            assertTrue(pending.cancel(false));
            fixture.admit();
            assertEquals(1, fixture.businessSends.get());
        }
    }

    @Test
    void publicSendReadinessEndsOnShutdown() throws Exception {
        try (Fixture fixture = new Fixture(Duration.ofSeconds(2))) {
            var pending =
                    fixture.runtime
                            .sendToChannel("orders", new Request("held"))
                            .submit()
                            .toCompletableFuture();
            fixture.runtime.beginClose();
            var failure = assertThrows(CompletionException.class, pending::join);
            assertEquals(
                    ZLinkFrameworkErrorKind.SHUTTING_DOWN,
                    assertInstanceOf(ZLinkFrameworkException.class, failure.getCause()).kind());
            fixture.admit();
            assertEquals(0, fixture.businessSends.get());
        }
    }

    @Test
    void readyRequestHasAtMostOriginalBudget() throws Exception {
        try (Fixture fixture = new Fixture(Duration.ofSeconds(2))) {
            ZLinkRequestCall call =
                    fixture.runtime
                            .requestToChannel("orders", new Request("cap-ready"))
                            .timeout(Duration.ofSeconds(8));
            long started = fixture.time.nanoTime();
            // 02-channel-transport/02-channel-messaging.ko.md:172: readiness waits within a
            // separate admission deadline.
            CompletableFuture<Reply> reply = call.submit(Reply.class).toCompletableFuture();
            assertFalse(reply.isDone());
            assertEquals(0, fixture.businessRequests.get());
            fixture.time.advanceBy(Duration.ofMillis(500).toNanos());
            fixture.admit();

            assertEquals(new Reply("reply"), reply.join());
            assertEquals(1, fixture.businessRequests.get());
            assertEquals(started + Duration.ofMillis(500).toNanos(), fixture.requestStarted);
            assertTrue(fixture.requestTimeout.get().compareTo(Duration.ofSeconds(8)) <= 0);
            assertEquals(
                    started + Duration.ofMillis(8_000).toNanos(),
                    fixture.requestStarted + fixture.requestTimeout.get().toNanos());
        }
    }

    @Test
    void unregisteredChannelEndsAsNotFoundWithoutAdmissionWait() throws Exception {
        try (Fixture fixture = new Fixture(Duration.ofSeconds(2))) {
            long started = fixture.time.nanoTime();
            var requestFailure =
                    assertSubmitFailure(
                            systems.zlink.framework.errors.ZLinkConfigurationException.class,
                            () ->
                                    fixture.runtime
                                            .requestToChannel("missing", new Request("missing"))
                                            .submit(Reply.class));
            assertEquals(ZLinkFrameworkErrorKind.NOT_FOUND, requestFailure.kind());
            var sendFailure =
                    assertSubmitFailure(
                            systems.zlink.framework.errors.ZLinkConfigurationException.class,
                            () ->
                                    fixture.runtime
                                            .sendToChannel("missing", new Request("missing"))
                                            .submit());
            assertEquals(ZLinkFrameworkErrorKind.NOT_FOUND, sendFailure.kind());
            assertEquals(started, fixture.time.nanoTime());
            assertEquals(0, fixture.businessRequests.get());
        }
    }

    @Test
    void readyWeightZeroAndDrainingFailWithoutAdmissionWait() throws Exception {
        for (var state :
                List.of(ZLinkFrameworkRuntimeState.SERVING, ZLinkFrameworkRuntimeState.DRAINING)) {
            for (boolean pending : List.of(false, true)) {
                try (Fixture fixture = new Fixture(Duration.ofSeconds(2), pending)) {
                    fixture.admitWithEligibility(
                            state == ZLinkFrameworkRuntimeState.SERVING ? 0 : 100, state);
                    long started = fixture.time.nanoTime();
                    var failure =
                            assertThrows(
                                    CompletionException.class,
                                    () ->
                                            fixture.runtime
                                                    .requestToChannel(
                                                            "orders", new Request("ineligible"))
                                                    .submit(Reply.class)
                                                    .toCompletableFuture()
                                                    .join());
                    assertEquals(
                            ZLinkFrameworkErrorKind.UNAVAILABLE,
                            assertInstanceOf(ZLinkFrameworkException.class, failure.getCause())
                                    .kind());
                    var sendFailure =
                            assertSubmitFailure(
                                    ZLinkFrameworkException.class,
                                    () ->
                                            fixture.runtime
                                                    .sendToChannel(
                                                            "orders", new Request("ineligible"))
                                                    .submit());
                    assertEquals(ZLinkFrameworkErrorKind.UNAVAILABLE, sendFailure.kind());
                    assertEquals(started, fixture.time.nanoTime());
                    assertEquals(0, fixture.businessRequests.get());
                }
            }
        }
    }

    private static void assertDeadlineExceeded(CompletableFuture<Reply> reply) {
        CompletionException failure = assertThrows(CompletionException.class, reply::join);
        assertEquals(
                ZLinkFrameworkErrorKind.DEADLINE_EXCEEDED,
                assertInstanceOf(ZLinkFrameworkException.class, failure.getCause()).kind());
    }

    private record Request(String value) {}

    private record Reply(String value) {}

    private static final class Fixture implements AutoCloseable {
        private Runnable receiveAdmission;
        private final ManualTime time = new ManualTime();
        private final CountDownLatch admissionListening = new CountDownLatch(1);
        private final CompletableFuture<ZLinkBackendReceived> admission =
                new CompletableFuture<>() {
                    @Override
                    public CompletableFuture<ZLinkBackendReceived> whenComplete(
                            java.util.function.BiConsumer<
                                            ? super ZLinkBackendReceived, ? super Throwable>
                                    action) {
                        var result = super.whenComplete(action);
                        admissionListening.countDown();
                        return result;
                    }
                };
        private final CompletableFuture<ZLinkBackendReceived> secondAdmission =
                new CompletableFuture<>() {
                    @Override
                    public CompletableFuture<ZLinkBackendReceived> whenComplete(
                            java.util.function.BiConsumer<
                                            ? super ZLinkBackendReceived, ? super Throwable>
                                    action) {
                        var result = super.whenComplete(action);
                        secondAdmissionStarted.countDown();
                        return result;
                    }
                };
        private final CountDownLatch secondAdmissionStarted = new CountDownLatch(1);
        private final AtomicInteger helloRequests = new AtomicInteger();
        private final AtomicInteger connects = new AtomicInteger();
        private final AtomicInteger disconnects = new AtomicInteger();
        private final AtomicInteger businessRequests = new AtomicInteger();
        private final AtomicInteger businessSends = new AtomicInteger();
        private CompletableFuture<Void> sendAdmission = CompletableFuture.completedFuture(null);
        private final CountDownLatch businessRequestStarted = new CountDownLatch(1);
        private final AtomicReference<Duration> requestTimeout = new AtomicReference<>();
        private volatile long requestStarted;
        private volatile boolean replyImmediately = true;
        private final ZLinkChannelRuntime runtime;

        private Fixture(Duration channelTimeout) throws Exception {
            this(channelTimeout, false);
        }

        private Fixture(Duration channelTimeout, boolean pending) throws Exception {
            ZLinkBackendDealerSocket dealer =
                    (ZLinkBackendDealerSocket)
                            Proxy.newProxyInstance(
                                    getClass().getClassLoader(),
                                    new Class<?>[] {ZLinkBackendDealerSocket.class},
                                    (proxy, method, args) ->
                                            switch (method.getName()) {
                                                case "request" -> {
                                                    @SuppressWarnings("unchecked")
                                                    List<Message> parts = (List<Message>) args[0];
                                                    if (ZLinkClientServerServiceWire.isControlFrame(
                                                            parts.getFirst().toByteArray())) {
                                                        assertInstanceOf(
                                                                ZLinkClientServerServiceWire.Hello
                                                                        .class,
                                                                ZLinkClientServerServiceWire.decode(
                                                                        parts.getFirst()
                                                                                .toByteArray()));
                                                        assertEquals(channelTimeout, args[1]);
                                                        if (helloRequests.incrementAndGet() == 1) {
                                                            yield admission;
                                                        }
                                                        yield secondAdmission;
                                                    }
                                                    requestStarted = time.nanoTime();
                                                    requestTimeout.set((Duration) args[1]);
                                                    businessRequests.incrementAndGet();
                                                    businessRequestStarted.countDown();
                                                    yield replyImmediately
                                                            ? CompletableFuture.completedFuture(
                                                                    received(
                                                                            Message.from(
                                                                                    "{\"value\":\"reply\"}")))
                                                            : new CompletableFuture<
                                                                    ZLinkBackendReceived>();
                                                }
                                                case "recv" -> null;
                                                case "waitForReadable" -> false;
                                                case "name" -> "ready-wait-dealer";
                                                case "hashCode" -> System.identityHashCode(proxy);
                                                case "equals" -> proxy == args[0];
                                                case "send" -> {
                                                    businessSends.incrementAndGet();
                                                    yield sendAdmission;
                                                }
                                                case "close" -> {
                                                    sendAdmission.completeExceptionally(
                                                            new systems.zlink.contracts.errors
                                                                    .ZlinkSubmitException(
                                                                    systems.zlink.contracts.sockets
                                                                            .SubmitResult
                                                                            .TERMINATED));
                                                    yield null;
                                                }
                                                case "connect" -> {
                                                    connects.incrementAndGet();
                                                    yield null;
                                                }
                                                case "disconnect" -> {
                                                    disconnects.incrementAndGet();
                                                    yield null;
                                                }
                                                case "receiveAdmission" -> receiveAdmission;
                                                case "setReceiveAdmission" -> {
                                                    receiveAdmission = (Runnable) args[0];
                                                    yield null;
                                                }
                                                case "setReceiveFlowState", "setChannelName" ->
                                                        null;
                                                default ->
                                                        throw new UnsupportedOperationException(
                                                                method.toString());
                                            });
            ZLinkBackendContext context =
                    new ZLinkBackendContext() {
                        @Override
                        public String name() {
                            return "ready-wait-context";
                        }

                        @Override
                        public void shutdown() {}

                        @Override
                        public void close() {}
                    };
            AtomicInteger createdDealers = new AtomicInteger();
            ZLinkChannelBackendAdapter backend =
                    (ZLinkChannelBackendAdapter)
                            Proxy.newProxyInstance(
                                    getClass().getClassLoader(),
                                    new Class<?>[] {ZLinkChannelBackendAdapter.class},
                                    (proxy, method, args) ->
                                            switch (method.getName()) {
                                                case "createContext" -> context;
                                                case "createDealerSocket" -> {
                                                    if (createdDealers.getAndIncrement() == 0)
                                                        yield dealer;
                                                    yield Proxy.newProxyInstance(
                                                            getClass().getClassLoader(),
                                                            new Class<?>[] {
                                                                ZLinkBackendDealerSocket.class
                                                            },
                                                            (ignoredProxy,
                                                                    socketMethod,
                                                                    socketArgs) ->
                                                                    socketMethod
                                                                                    .getName()
                                                                                    .equals(
                                                                                            "request")
                                                                            ? new CompletableFuture<
                                                                                    ZLinkBackendReceived>()
                                                                            : socketMethod.invoke(
                                                                                    dealer,
                                                                                    socketArgs));
                                                }
                                                default ->
                                                        throw new UnsupportedOperationException(
                                                                method.toString());
                                            });
            ZLinkMonitoringBackendAdapter monitoring =
                    socket ->
                            new ZLinkBackendSocketMonitor() {
                                private final Semaphore readable = new Semaphore(1);
                                private boolean emitted;

                                private volatile boolean closed;

                                @Override
                                public boolean waitForReadable(Duration timeout) {
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
                                    if (emitted) {
                                        return null;
                                    }
                                    emitted = true;
                                    return new ZLinkBackendSocketMonitorEvent(
                                            "CONNECTION_READY", Optional.empty(), "", "");
                                }

                                @Override
                                public boolean isClosed() {
                                    return closed;
                                }

                                @Override
                                public String name() {
                                    return "ready-wait-monitor";
                                }

                                @Override
                                public void close() {
                                    closed = true;
                                    readable.release();
                                }
                            };
            ZLinkBackendAdapterProvider provider =
                    (ZLinkBackendAdapterProvider)
                            Proxy.newProxyInstance(
                                    getClass().getClassLoader(),
                                    new Class<?>[] {ZLinkBackendAdapterProvider.class},
                                    (proxy, method, args) ->
                                            switch (method.getName()) {
                                                case "createMonitoringAdapter" -> monitoring;
                                                default ->
                                                        throw new UnsupportedOperationException(
                                                                method.toString());
                                            });
            DefaultZLinkFrameworkOptions options = new DefaultZLinkFrameworkOptions();
            options.setDefaultRequestTimeout(channelTimeout);
            var client =
                    options.addClientServerChannel("orders")
                            .client()
                            .connect("inproc://ready-wait");
            if (pending) client.connect("inproc://pending");
            runtime =
                    new ZLinkChannelRuntime(
                            backend,
                            context,
                            true,
                            provider,
                            new ZLinkBackendAdapterOptions(channelTimeout),
                            options.registration(),
                            new ZLinkJsonMessageSerializer(),
                            ZLinkHandlerActivator.reflection(),
                            null,
                            (ignoredBackend, ignoredKey) ->
                                    (ignoredSubmission, ignoredCleanup) -> {
                                        throw new AssertionError(
                                                "one-way admission is not used by this fixture");
                                    },
                            time::nanoTime,
                            time::advanceBy,
                            time);
            // The first admission callback is installed before the fixture exposes its runtime.
            assertTrue(admissionListening.await(1, TimeUnit.SECONDS));
        }

        private void admit() {
            admitWithEligibility(100, ZLinkFrameworkRuntimeState.SERVING);
        }

        private void admitWithEligibility(int weight, ZLinkFrameworkRuntimeState state) {
            (admission.isCompletedExceptionally() ? secondAdmission : admission)
                    .complete(
                            received(
                                    Message.from(
                                            ZLinkClientServerServiceWire.encodeAdmit(
                                                    new ZLinkClientServerServerDescriptor(
                                                            "orders",
                                                            RoutingId.from("server"),
                                                            1,
                                                            1,
                                                            "inproc://ready-wait",
                                                            weight,
                                                            state,
                                                            "default",
                                                            "server",
                                                            1,
                                                            Instant.EPOCH),
                                                    Integer.MAX_VALUE))));
        }

        private static ZLinkBackendReceived received(Message message) {
            return new ZLinkBackendReceived(
                    Optional.empty(), Optional.empty(), Optional.empty(), List.of(message));
        }

        @Override
        public void close() {
            runtime.close();
        }
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

        @Override
        public ScheduledFuture<?> scheduleAtFixedRate(
                Runnable command, long initialDelay, long period, TimeUnit unit) {
            TimedTask task =
                    new TimedTask(
                            command, nowNanos + unit.toNanos(initialDelay), unit.toNanos(period));
            tasks.add(task);
            return task;
        }

        private void advanceBy(long nanos) {
            assertTrue(nanos >= 0, "the monotonic clock must not move backwards");
            long target = nowNanos + nanos;
            while (true) {
                TimedTask next =
                        tasks.stream()
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
            private long deadlineNanos;
            private final long periodNanos;

            private TimedTask(Runnable command, long deadlineNanos) {
                this(command, deadlineNanos, 0);
            }

            private TimedTask(Runnable command, long deadlineNanos, long periodNanos) {
                super(command, null);
                this.deadlineNanos = deadlineNanos;
                this.periodNanos = periodNanos;
            }

            @Override
            public void run() {
                if (periodNanos == 0) {
                    super.run();
                } else if (runAndReset()) {
                    deadlineNanos += periodNanos;
                    tasks.add(this);
                }
            }

            @Override
            public long getDelay(TimeUnit unit) {
                return unit.convert(deadlineNanos - nowNanos, TimeUnit.NANOSECONDS);
            }

            @Override
            public int compareTo(Delayed other) {
                return Long.compare(
                        getDelay(TimeUnit.NANOSECONDS), other.getDelay(TimeUnit.NANOSECONDS));
            }
        }
    }
}
