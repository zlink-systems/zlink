package systems.zlink.framework.runtime.channels;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertInstanceOf;
import static org.junit.jupiter.api.Assertions.assertThrows;
import static org.junit.jupiter.api.Assertions.assertTrue;

import java.lang.reflect.Proxy;
import java.time.Duration;
import java.time.Instant;
import java.util.List;
import java.util.Optional;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionException;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.Executors;
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
            long building = System.nanoTime();
            ZLinkRequestCall call = fixture.runtime.requestToChannel("orders", new Request("short"))
                .timeout(Duration.ofMillis(150));
            assertTrue(System.nanoTime() - building < TimeUnit.MILLISECONDS.toNanos(150),
                "creating the builder must not wait for service readiness");

            long started = System.nanoTime();
            assertUnavailable(call);
            assertElapsed(started, 145, 250);
            assertEquals(0, fixture.businessRequests.get());
        }
    }

    @Test
    void lateReadyServerReceivesOnlyTheRemainingCallTimeout() throws Exception {
        try (Fixture fixture = new Fixture(Duration.ofSeconds(2));
             var scheduler = Executors.newSingleThreadScheduledExecutor()) {
            ZLinkRequestCall call = fixture.runtime.requestToChannel("orders", new Request("late"))
                .timeout(Duration.ofMillis(400));
            long started = System.nanoTime();
            scheduler.schedule(fixture::admit, 150, TimeUnit.MILLISECONDS);

            assertEquals(new Reply("reply"), call.submit(Reply.class).toCompletableFuture().join());
            assertElapsed(started, 145, 400);
            assertEquals(1, fixture.businessRequests.get());
            long forwarded = fixture.requestTimeout.get().toNanos();
            assertTrue(forwarded > 0 && forwarded <= TimeUnit.MILLISECONDS.toNanos(250),
                "readiness must consume the operation's timeout: " + fixture.requestTimeout.get());
            long totalBudget = fixture.requestStarted - started + forwarded;
            assertTrue(totalBudget <= TimeUnit.MILLISECONDS.toNanos(410),
                "the transport deadline must stay within the original call deadline");
        }
    }

    @Test
    void lateReadyRequestReplyStillExpiresAtTheOriginalCallDeadline() throws Exception {
        try (Fixture fixture = new Fixture(Duration.ofSeconds(2));
             var scheduler = Executors.newSingleThreadScheduledExecutor()) {
            fixture.replyImmediately = false;
            ZLinkRequestCall call = fixture.runtime.requestToChannel("orders", new Request("pending"))
                .timeout(Duration.ofMillis(400));
            long started = System.nanoTime();
            scheduler.schedule(fixture::admit, 150, TimeUnit.MILLISECONDS);

            CompletionException failure = assertThrows(CompletionException.class,
                () -> call.submit(Reply.class).toCompletableFuture().join());
            assertEquals(ZLinkFrameworkErrorKind.DEADLINE_EXCEEDED,
                assertInstanceOf(ZLinkFrameworkException.class, failure.getCause()).kind());
            assertElapsed(started, 395, 500);
            assertEquals(1, fixture.businessRequests.get());
            assertTrue(fixture.requestTimeout.get().compareTo(Duration.ofMillis(250)) <= 0);
        }
    }

    @Test
    void readyWaitHasFiveSecondCapEvenWhenCallTimeoutExceedsChannelDefault() throws Exception {
        try (Fixture fixture = new Fixture(Duration.ofMillis(150))) {
            ZLinkRequestCall call = fixture.runtime.requestToChannel("orders", new Request("cap"))
                .timeout(Duration.ofSeconds(8));
            long started = System.nanoTime();
            assertUnavailable(call);
            assertElapsed(started, 4_995, 5_200);
            assertEquals(0, fixture.businessRequests.get());
        }
    }

    private static void assertUnavailable(ZLinkRequestCall call) {
        CompletionException failure = assertThrows(CompletionException.class,
            () -> call.submit(Reply.class).toCompletableFuture().join());
        assertEquals(ZLinkFrameworkErrorKind.UNAVAILABLE,
            assertInstanceOf(ZLinkFrameworkException.class, failure.getCause()).kind());
    }

    private static void assertElapsed(long started, long minimumMillis, long maximumMillis) {
        Duration elapsed = Duration.ofNanos(System.nanoTime() - started);
        assertTrue(elapsed.compareTo(Duration.ofMillis(minimumMillis)) >= 0
                && elapsed.compareTo(Duration.ofMillis(maximumMillis)) < 0,
            "elapsed=" + elapsed + ", expected [" + minimumMillis + ", " + maximumMillis + ") ms");
    }

    private record Request(String value) { }
    private record Reply(String value) { }

    private static final class Fixture implements AutoCloseable {
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
                        requestStarted = System.nanoTime();
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
                    if (emitted) return null;
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
            runtime = new ZLinkChannelRuntime(backend, provider,
                new ZLinkBackendAdapterOptions(channelTimeout), options.registration(),
                new ZLinkJsonMessageSerializer(), ZLinkHandlerActivator.reflection());
            assertTrue(admissionStarted.await(1, TimeUnit.SECONDS));
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
}
