package systems.zlink.framework.runtime.streams;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertTrue;

import java.util.Map;
import java.util.Optional;
import java.lang.reflect.Proxy;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.Executors;
import java.util.concurrent.ScheduledExecutorService;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicInteger;
import org.junit.jupiter.api.Test;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.streams.ZLinkStreamCodec;
import systems.zlink.framework.configuration.ZLinkMessageFlowLogMode;
import systems.zlink.framework.runtime.configuration.ZLinkDispatchOptionsRegistration;
import systems.zlink.framework.runtime.diagnostics.ZLinkMessageFlowTracer;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendStreamSocket;
import systems.zlink.framework.runtime.internal.handlers.ZLinkHandlerActivator;
import systems.zlink.framework.messaging.ZLinkMessage;
import systems.zlink.framework.streams.ZLinkSession;
import systems.zlink.framework.streams.ZLinkSessionContext;
import systems.zlink.framework.streams.ZLinkSessionDispatchContext;
import systems.zlink.framework.streams.ZLinkStreamError;
import java.util.concurrent.CompletionStage;

final class ZLinkStreamSessionContextStateTest {
    @Test
    void replyHeaderCanBeClaimedOnlyOnce() {
        ZLinkStreamSessionContextState context = context(new AtomicInteger());
        ZLinkStreamHeader request = new ZLinkStreamHeader(
            "Request",
            Map.of(),
            Optional.of(7L));

        assertTrue(context.claimReplyHeader(request));
        assertFalse(context.claimReplyHeader(request));
    }

    @Test
    void concurrentReplyClaimsHaveExactlyOneWinner() throws Exception {
        for (int iteration = 0; iteration < 100; iteration++) {
            ZLinkStreamSessionContextState context = context(new AtomicInteger());
            ZLinkStreamHeader request = new ZLinkStreamHeader(
                "Request",
                Map.of(),
                Optional.of((long) iteration + 1L));
            CountDownLatch start = new CountDownLatch(1);
            AtomicInteger winners = new AtomicInteger();
            Thread first = Thread.ofVirtual().start(() -> claim(context, request, start, winners));
            Thread second = Thread.ofVirtual().start(() -> claim(context, request, start, winners));

            start.countDown();
            first.join();
            second.join();

            assertEquals(1, winners.get());
            assertFalse(context.claimReplyHeader(request));
        }
    }

    @Test
    void closeExecutesTheRuntimeOwnedSessionCloseAction() {
        AtomicInteger closes = new AtomicInteger();
        ZLinkStreamSessionContextState context = context(closes);

        context.close().toCompletableFuture().join();

        assertEquals(1, closes.get());
    }

    @Test
    void errorReplyStageCompletesOnlyAfterAdmission() throws Exception {
        ScheduledExecutorService retries = Executors.newSingleThreadScheduledExecutor();
        AtomicInteger attempts = new AtomicInteger();
        ZLinkBackendStreamSocket stream = stream(attempts, 2);
        ZLinkStreamSessionContextState context = context(
            new AtomicInteger(),
            stream,
            retries);
        ZLinkStreamHeader request = new ZLinkStreamHeader(
            "Request",
            Map.of(),
            Optional.of(9L));

        try {
            CompletionStage<Void> dispatch = context.dispatchStage(
                request,
                ZLinkMessage.empty(),
                new FailedSession(context));

            dispatch.toCompletableFuture().get(1, TimeUnit.SECONDS);
            assertEquals(2, attempts.get());
        } finally {
            context.closeReplyRetries();
            retries.shutdownNow();
        }
    }

    @Test
    void closingContextCancelsDelayedErrorReply() throws Exception {
        ScheduledExecutorService retries = Executors.newSingleThreadScheduledExecutor();
        AtomicInteger attempts = new AtomicInteger();
        ZLinkBackendStreamSocket stream = stream(attempts, Integer.MAX_VALUE);
        ZLinkStreamSessionContextState context = context(
            new AtomicInteger(),
            stream,
            retries);
        ZLinkStreamHeader request = new ZLinkStreamHeader(
            "Request",
            Map.of(),
            Optional.of(10L));

        try {
            CompletionStage<Void> dispatch = context.dispatchStage(
                request,
                ZLinkMessage.empty(),
                new FailedSession(context));
            context.closeReplyRetries();
            assertTrue(dispatch.toCompletableFuture().isCompletedExceptionally());
            int attemptsAtClose = attempts.get();
            Thread.sleep(50);
            assertEquals(attemptsAtClose, attempts.get());
        } finally {
            retries.shutdownNow();
        }
    }

    private static ZLinkStreamSessionContextState context(AtomicInteger closes) {
        return context(closes, null, null);
    }

    private static ZLinkStreamSessionContextState context(
        AtomicInteger closes,
        ZLinkBackendStreamSocket stream,
        ScheduledExecutorService retries) {
        return new ZLinkStreamSessionContextState(
            "session",
            stream,
            RoutingId.from("client-a"),
            null,
            null,
            ZLinkStreamCodec.JSON,
            null,
            flow(),
            () -> {
                closes.incrementAndGet();
                return CompletableFuture.completedFuture(null);
            },
            new systems.zlink.framework.runtime.internal.calls.ZLinkOneWayCalls(
                (backend, key) -> (submission, cleanup) -> {
                    try {
                        return submission.get()
                            ? CompletableFuture.completedFuture(null)
                            : CompletableFuture.failedFuture(
                                new IllegalStateException("submission rejected"));
                    } finally {
                        cleanup.run();
                    }
                }),
            retries);
    }

    private static ZLinkMessageFlowTracer flow() {
        ZLinkDispatchOptionsRegistration options =
            new ZLinkDispatchOptionsRegistration();
        options.messageFlow(ZLinkMessageFlowLogMode.OFF);
        return new ZLinkMessageFlowTracer(
            options,
            ZLinkHandlerActivator.reflection(),
            Runnable::run);
    }

    private static ZLinkBackendStreamSocket stream(
        AtomicInteger attempts,
        int successfulAttempt) {
        return (ZLinkBackendStreamSocket) Proxy.newProxyInstance(
            ZLinkBackendStreamSocket.class.getClassLoader(),
            new Class<?>[] {ZLinkBackendStreamSocket.class},
            (proxy, method, arguments) -> switch (method.getName()) {
                case "name" -> "test-stream";
                case "reply" -> attempts.incrementAndGet() >= successfulAttempt;
                case "close", "bind", "setTlsServer", "setMaxMessageSize",
                    "enableNotifications", "onTransportError", "startSessionService" -> null;
                default -> defaultValue(method.getReturnType());
            });
    }

    private static Object defaultValue(Class<?> type) {
        if (!type.isPrimitive()) {
            return null;
        }
        if (type == boolean.class) {
            return false;
        }
        if (type == char.class) {
            return '\0';
        }
        return 0;
    }

    private static final class FailedSession implements ZLinkSession {
        private final ZLinkSessionContext context;

        private FailedSession(ZLinkSessionContext context) {
            this.context = context;
        }

        @Override
        public ZLinkSessionContext context() {
            return context;
        }

        @Override
        public CompletionStage<Void> onConnected() {
            return CompletableFuture.completedFuture(null);
        }

        @Override
        public CompletionStage<Void> onDisconnected() {
            return CompletableFuture.completedFuture(null);
        }

        @Override
        public CompletionStage<Void> onError(ZLinkStreamError error) {
            return CompletableFuture.completedFuture(null);
        }

        @Override
        public CompletionStage<Void> onDispatch(
            ZLinkSessionDispatchContext dispatch,
            ZLinkMessage payload) {
            return CompletableFuture.failedFuture(
                new IllegalStateException("handler failure"));
        }
    }

    private static void claim(
        ZLinkStreamSessionContextState context,
        ZLinkStreamHeader request,
        CountDownLatch start,
        AtomicInteger winners) {
        try {
            start.await();
        } catch (InterruptedException error) {
            Thread.currentThread().interrupt();
            throw new AssertionError(error);
        }
        if (context.claimReplyHeader(request)) {
            winners.incrementAndGet();
        }
    }
}
