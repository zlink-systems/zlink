package systems.zlink.framework.runtime.channels;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertInstanceOf;
import static org.junit.jupiter.api.Assertions.assertTrue;

import java.time.Duration;
import java.util.List;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionException;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.Executor;
import java.util.concurrent.Executors;
import java.util.concurrent.RejectedExecutionException;
import java.util.concurrent.ScheduledExecutorService;
import java.util.concurrent.ScheduledFuture;
import java.util.concurrent.ScheduledThreadPoolExecutor;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.concurrent.atomic.AtomicReference;
import org.junit.jupiter.api.Test;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.contracts.sockets.SendFlags;
import systems.zlink.framework.errors.ZLinkConfigurationException;
import systems.zlink.framework.errors.ZLinkFrameworkErrorKind;
import systems.zlink.framework.errors.ZLinkFrameworkException;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendRequestCallback;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendRouterSocket;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendSpotRouteBridge;

final class ZLinkSpotRouteBridgeDispatcherTest {
    @Test
    void acceptedRetryDoesNotAttemptAfterLifecycleClose() throws Exception {
        CapturingScheduler scheduler = new CapturingScheduler();
        ScheduledExecutorService deadlines =
            Executors.newSingleThreadScheduledExecutor();
        ZLinkChannelCallRuntime calls = new ZLinkChannelCallRuntime(
            null,
            deadlines,
            Runnable::run,
            Duration.ofSeconds(1),
            null,
            null,
            null,
            null);
        RetryBridge bridge = new RetryBridge();
        CompletableFuture<Void> result = new CompletableFuture<>();
        try {
            calls.track(result, Duration.ofSeconds(1));
            ZLinkSpotRouteBridgeDispatcher.submitSendWithRetry(
                bridge,
                "play.route",
                RoutingId.from("play-node"),
                "room-spot",
                List.of("payload".getBytes()),
                Duration.ofSeconds(1),
                scheduler,
                Runnable::run,
                result);

            assertEquals(1, bridge.attempts.get());
            assertTrue(bridge.lastAttempt.get().stream().allMatch(Message::empty));
            scheduler.awaitAccepted();
            calls.beginClose();

            scheduler.fireAccepted();

            assertEquals(1, bridge.attempts.get());
            ZLinkConfigurationException closeFailure = assertInstanceOf(
                ZLinkConfigurationException.class,
                completionFailure(result));
            assertEquals(ZLinkFrameworkErrorKind.NOT_CONFIGURED, closeFailure.kind());
            assertEquals("channel runtime is closed", closeFailure.getMessage());
        } finally {
            calls.beginClose();
            deadlines.shutdownNow();
            scheduler.shutdownNow();
        }
    }

    @Test
    void rejectedRetryTimerSettlesAsShutdownFailure() {
        ScheduledThreadPoolExecutor scheduler = new ScheduledThreadPoolExecutor(1);
        scheduler.shutdownNow();
        RetryBridge bridge = new RetryBridge();
        CompletableFuture<Void> result = new CompletableFuture<>();

        ZLinkSpotRouteBridgeDispatcher.submitSendWithRetry(
            bridge,
            "play.route",
            RoutingId.from("play-node"),
            "room-spot",
            List.of("payload".getBytes()),
            Duration.ofSeconds(1),
            scheduler,
            Runnable::run,
            result);

        assertShutdownFailure(result);
        assertEquals(1, bridge.attempts.get());
        assertTrue(bridge.lastAttempt.get().stream().allMatch(Message::empty));
    }

    @Test
    void rejectedInfrastructureHandoffSettlesExactlyOnceAsShutdownFailure()
        throws Exception {
        CapturingScheduler scheduler = new CapturingScheduler();
        AtomicInteger handoffs = new AtomicInteger();
        Executor infrastructure = command -> {
            handoffs.incrementAndGet();
            throw new RejectedExecutionException("infrastructure lane is closed");
        };
        RetryBridge bridge = new RetryBridge();
        CompletableFuture<Void> result = new CompletableFuture<>();
        AtomicInteger completions = new AtomicInteger();
        result.whenComplete((ignored, failure) -> completions.incrementAndGet());
        try {
            ZLinkSpotRouteBridgeDispatcher.submitSendWithRetry(
                bridge,
                "play.route",
                RoutingId.from("play-node"),
                "room-spot",
                List.of("payload".getBytes()),
                Duration.ofSeconds(1),
                scheduler,
                infrastructure,
                result);

            scheduler.awaitAccepted();
            scheduler.fireAccepted();

            assertShutdownFailure(result);
            assertEquals(1, bridge.attempts.get());
            assertEquals(1, handoffs.get());
            assertEquals(1, completions.get());
        } finally {
            scheduler.shutdownNow();
        }
    }

    private static void assertShutdownFailure(CompletableFuture<Void> result) {
        Throwable failure = completionFailure(result);
        ZLinkFrameworkException frameworkFailure = assertInstanceOf(
            ZLinkFrameworkException.class,
            failure);
        assertEquals(ZLinkFrameworkErrorKind.SHUTTING_DOWN, frameworkFailure.kind());
        assertInstanceOf(RejectedExecutionException.class, frameworkFailure.getCause());
    }

    private static Throwable completionFailure(CompletableFuture<Void> result) {
        try {
            result.join();
            throw new AssertionError("result completed successfully");
        } catch (CompletionException failure) {
            return failure.getCause();
        }
    }

    private static final class CapturingScheduler extends ScheduledThreadPoolExecutor {
        private final AtomicReference<Runnable> accepted = new AtomicReference<>();
        private final CountDownLatch acceptedSignal = new CountDownLatch(1);

        private CapturingScheduler() {
            super(1, runnable -> {
                Thread thread = new Thread(runnable, "captured-route-retry");
                thread.setDaemon(true);
                return thread;
            });
        }

        @Override
        public ScheduledFuture<?> schedule(
            Runnable command,
            long delay,
            TimeUnit unit) {
            if (!accepted.compareAndSet(null, command)) {
                throw new AssertionError("only one retry may be pending");
            }
            acceptedSignal.countDown();
            return super.schedule(() -> { }, 1, TimeUnit.DAYS);
        }

        private void awaitAccepted() throws InterruptedException {
            assertTrue(
                acceptedSignal.await(1, TimeUnit.SECONDS),
                "retry timer was not accepted");
        }

        private void fireAccepted() {
            Runnable command = accepted.getAndSet(null);
            if (command == null) {
                throw new AssertionError("retry was not accepted");
            }
            command.run();
        }
    }

    private static final class RetryBridge implements ZLinkBackendSpotRouteBridge {
        private final AtomicInteger attempts = new AtomicInteger();
        private final AtomicReference<List<Message>> lastAttempt =
            new AtomicReference<>(List.of());

        @Override
        public void attachRouterChannel(
            String channelName,
            ZLinkBackendRouterSocket router) {
        }

        @Override
        public boolean send(
            String channelName,
            RoutingId targetNodeRid,
            String targetSpotId,
            List<Message> parts,
            SendFlags flags) {
            attempts.incrementAndGet();
            lastAttempt.set(List.copyOf(parts));
            return false;
        }

        @Override
        public boolean request(
            String channelName,
            RoutingId targetNodeRid,
            String targetSpotId,
            List<Message> parts,
            ZLinkBackendRequestCallback callback,
            SendFlags flags,
            Duration timeout) {
            throw new UnsupportedOperationException();
        }

        @Override
        public boolean handleRouterReceived(
            String channelName,
            RoutingId sourceNodeRid,
            long requestSeq,
            List<Message> parts) {
            return false;
        }

        @Override
        public int drain() {
            return 0;
        }

        @Override
        public String name() {
            return "retry-bridge";
        }

        @Override
        public void close() {
        }
    }
}
