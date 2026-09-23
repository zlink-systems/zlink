package systems.zlink.framework.runtime.streams;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertNull;
import static org.junit.jupiter.api.Assertions.assertTrue;

import org.junit.jupiter.api.Test;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.actors.ActorRef;
import systems.zlink.framework.configuration.ZLinkMessageFlowLogMode;
import systems.zlink.framework.errors.ZLinkFrameworkErrorKind;
import systems.zlink.framework.errors.ZLinkFrameworkException;
import systems.zlink.framework.messaging.ZLinkMessage;
import systems.zlink.framework.runtime.actors.ZLinkSessionActorsRuntime;
import systems.zlink.framework.runtime.configuration.ZLinkDispatchOptionsRegistration;
import systems.zlink.framework.runtime.diagnostics.ZLinkMessageFlowTracer;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendActorBindOperation;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendStreamSocket;
import systems.zlink.framework.runtime.internal.handlers.ZLinkHandlerActivator;
import systems.zlink.framework.runtime.internal.streams.ZLinkStreamErrorPayload;
import systems.zlink.framework.runtime.messaging.ZLinkJsonMessageSerializer;
import systems.zlink.framework.streams.ZLinkSession;
import systems.zlink.framework.streams.ZLinkSessionActor;
import systems.zlink.framework.streams.ZLinkSessionContext;
import systems.zlink.framework.streams.ZLinkSessionDispatchContext;
import systems.zlink.framework.streams.ZLinkStreamCodec;
import systems.zlink.framework.streams.ZLinkStreamError;
import systems.zlink.framework.streams.ZLinkStreamMessageKind;

import java.lang.reflect.Proxy;
import java.util.List;
import java.util.Map;
import java.util.Optional;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.CopyOnWriteArrayList;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.concurrent.atomic.AtomicReference;
import java.util.logging.Handler;
import java.util.logging.Level;
import java.util.logging.LogRecord;
import java.util.logging.Logger;

final class ZLinkStreamSessionContextStateTest {
    @Test
    void streamErrorPayloadUsesCanonicalErrorKinds() {
        ZLinkStreamErrorPayload.Decoded rejected =
                ZLinkStreamErrorPayload.decode(
                        ZLinkStreamErrorPayload.encode(
                                new ZLinkFrameworkException(
                                        ZLinkFrameworkErrorKind.REJECTED, "request rejected")));
        assertEquals("rejected", rejected.code());
        assertEquals(ZLinkFrameworkErrorKind.REJECTED, rejected.frameworkKind());

        ZLinkStreamErrorPayload.Decoded unexpected =
                ZLinkStreamErrorPayload.decode(
                        ZLinkStreamErrorPayload.encode(
                                new IllegalStateException("handler failed")));
        assertEquals("internal_failure", unexpected.code());
        assertEquals(ZLinkFrameworkErrorKind.INTERNAL_FAILURE, unexpected.frameworkKind());
    }

    @Test
    void replyHeaderCanBeClaimedOnlyOnce() {
        ZLinkStreamSessionContextState context = context(new AtomicInteger());
        ZLinkStreamHeader request = new ZLinkStreamHeader("Request", Map.of(), Optional.of(7L));

        assertTrue(context.claimReplyHeader(request));
        assertFalse(context.claimReplyHeader(request));
    }

    @Test
    void concurrentReplyClaimsHaveExactlyOneWinner() throws Exception {
        for (int iteration = 0; iteration < 100; iteration++) {
            ZLinkStreamSessionContextState context = context(new AtomicInteger());
            ZLinkStreamHeader request =
                    new ZLinkStreamHeader("Request", Map.of(), Optional.of((long) iteration + 1L));
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
    void errorReplyDelegatesOnceToBackendAsyncAndWaitsForPhysicalTerminal() throws Exception {
        CompletableFuture<Void> physicalTerminal = new CompletableFuture<>();
        AtomicInteger asyncSubmits = new AtomicInteger();
        AtomicInteger syncSubmits = new AtomicInteger();
        AtomicReference<systems.zlink.contracts.messaging.Message> submitted =
                new AtomicReference<>();
        ZLinkBackendStreamSocket stream =
                stream(asyncSubmits, syncSubmits, physicalTerminal, submitted);
        ZLinkStreamSessionContextState context = context(new AtomicInteger(), stream);
        ZLinkStreamHeader request = new ZLinkStreamHeader("Request", Map.of(), Optional.of(9L));

        try {
            CompletionStage<Void> dispatch =
                    context.dispatchStage(
                            request, ZLinkMessage.empty(), new FailedSession(context));

            assertEquals(1, asyncSubmits.get());
            assertEquals(0, syncSubmits.get());
            assertFalse(dispatch.toCompletableFuture().isDone());
            assertTrue(submitted.get().size() > 0);
            physicalTerminal.complete(null);
            dispatch.toCompletableFuture().get(1, TimeUnit.SECONDS);
            assertEquals(0, submitted.get().size());
        } finally {
            context.closeReplyRetries();
        }
    }

    @Test
    void closingContextCancelsBackendErrorReplyTerminal() {
        CompletableFuture<Void> physicalTerminal = new CompletableFuture<>();
        AtomicInteger asyncSubmits = new AtomicInteger();
        AtomicInteger syncSubmits = new AtomicInteger();
        AtomicReference<systems.zlink.contracts.messaging.Message> submitted =
                new AtomicReference<>();
        ZLinkBackendStreamSocket stream =
                stream(asyncSubmits, syncSubmits, physicalTerminal, submitted);
        ZLinkStreamSessionContextState context = context(new AtomicInteger(), stream);
        ZLinkStreamHeader request = new ZLinkStreamHeader("Request", Map.of(), Optional.of(10L));

        CompletionStage<Void> dispatch =
                context.dispatchStage(request, ZLinkMessage.empty(), new FailedSession(context));
        context.closeReplyRetries();

        assertEquals(1, asyncSubmits.get());
        assertEquals(0, syncSubmits.get());
        assertTrue(physicalTerminal.isCancelled());
        assertTrue(dispatch.toCompletableFuture().isCompletedExceptionally());
        assertEquals(0, submitted.get().size());
    }

    @Test
    void staleActorSlotRequestDoesNotReachSessionHandler() throws Exception {
        CompletableFuture<Void> physicalTerminal = CompletableFuture.completedFuture(null);
        AtomicInteger asyncSubmits = new AtomicInteger();
        AtomicReference<systems.zlink.contracts.messaging.Message> submitted =
                new AtomicReference<>();
        AtomicReference<ZLinkStreamHeader> replyHeader = new AtomicReference<>();
        AtomicReference<byte[]> replyPayload = new AtomicReference<>();
        ZLinkBackendStreamSocket stream =
                stream(
                        asyncSubmits,
                        new AtomicInteger(),
                        physicalTerminal,
                        submitted,
                        replyHeader,
                        replyPayload);
        ZLinkSessionActorsRuntime actors =
                new ZLinkSessionActorsRuntime(stream, RoutingId.from("client-a"), null, null);
        ZLinkStreamSessionContextState context =
                context(new AtomicInteger(), stream, actors, tracedFlow());
        AtomicInteger dispatches = new AtomicInteger();
        ZLinkStreamHeader request =
                new ZLinkStreamHeader("Request", Map.of(), Optional.of(11L)).withActorSlot(1);

        List<String> records =
                captureFlow(
                        () ->
                                context.dispatchStage(
                                                request,
                                                ZLinkMessage.empty(),
                                                new RecordingSession(context, dispatches))
                                        .toCompletableFuture()
                                        .join());

        assertEquals(0, dispatches.get());
        assertEquals(1, asyncSubmits.get());
        assertEquals(Optional.of(11L), replyHeader.get().requestSequence());
        assertEquals(ZLinkStreamMessageKind.ERROR, replyHeader.get().kind());
        assertEquals(
                "invalid_operation", ZLinkStreamErrorPayload.decode(replyPayload.get()).code());
        assertTrue(
                records.stream()
                        .anyMatch(
                                record ->
                                        record.contains("event_id=zlink.dispatch_error")
                                                && record.contains("surface=stream")
                                                && record.contains("kind=request")
                                                && record.contains("outcome=failed")
                                                && record.contains("reason=stale_target")
                                                && record.contains("action=reply_error")));
    }

    @Test
    void staleActorSlotSendIsDroppedAndRecorded() {
        AtomicInteger dispatches = new AtomicInteger();
        AtomicInteger replies = new AtomicInteger();
        ZLinkBackendStreamSocket stream =
                stream(
                        replies,
                        new AtomicInteger(),
                        CompletableFuture.completedFuture(null),
                        new AtomicReference<>());
        ZLinkSessionActorsRuntime actors =
                new ZLinkSessionActorsRuntime(stream, RoutingId.from("client-a"), null, null);
        ZLinkStreamSessionContextState context =
                context(new AtomicInteger(), stream, actors, tracedFlow());
        ZLinkStreamHeader send =
                new ZLinkStreamHeader("Send", Map.of(), Optional.empty()).withActorSlot(1);

        List<String> records =
                captureFlow(
                        () ->
                                context.dispatchStage(
                                                send,
                                                ZLinkMessage.empty(),
                                                new RecordingSession(context, dispatches))
                                        .toCompletableFuture()
                                        .join());

        assertEquals(0, dispatches.get());
        assertEquals(0, replies.get());
        assertTrue(
                records.stream()
                        .anyMatch(
                                record ->
                                        record.contains("phase=dropped")
                                                && record.contains("surface=stream")
                                                && record.contains("kind=send")
                                                && record.contains("outcome=dropped")
                                                && record.contains("reason=stale_target")));
    }

    @Test
    void packetWithoutActorSlotStillReachesSessionHandler() {
        ZLinkBackendStreamSocket stream =
                stream(
                        new AtomicInteger(),
                        new AtomicInteger(),
                        CompletableFuture.completedFuture(null),
                        new AtomicReference<>());
        ZLinkSessionActorsRuntime actors =
                new ZLinkSessionActorsRuntime(stream, RoutingId.from("client-a"), null, null);
        ZLinkStreamSessionContextState context =
                context(new AtomicInteger(), stream, actors, flow());
        AtomicInteger dispatches = new AtomicInteger();
        AtomicReference<ZLinkSessionActor> dispatchedActor = new AtomicReference<>();
        ZLinkStreamHeader send = new ZLinkStreamHeader("Send", Map.of(), Optional.empty());

        context.dispatchStage(
                        send,
                        ZLinkMessage.empty(),
                        new RecordingSession(context, dispatches, dispatchedActor))
                .toCompletableFuture()
                .join();

        assertEquals(1, dispatches.get());
        assertNull(dispatchedActor.get());
    }

    @Test
    void currentActorSlotStillReachesSessionHandlerWithBoundActor() {
        ZLinkBackendStreamSocket stream =
                stream(
                        new AtomicInteger(),
                        new AtomicInteger(),
                        CompletableFuture.completedFuture(null),
                        new AtomicReference<>());
        ZLinkSessionActorsRuntime actors =
                new ZLinkSessionActorsRuntime(
                        stream, RoutingId.from("client-a"), null, new ZLinkJsonMessageSerializer());
        ZLinkSessionActor bound =
                actors.bind(new ActorRef("actor-1", 1, "mesh", RoutingId.from("node-a")))
                        .toCompletableFuture()
                        .join();
        ZLinkStreamSessionContextState context =
                context(new AtomicInteger(), stream, actors, flow());
        AtomicInteger dispatches = new AtomicInteger();
        AtomicReference<ZLinkSessionActor> dispatchedActor = new AtomicReference<>();
        ZLinkStreamHeader send =
                new ZLinkStreamHeader("Send", Map.of(), Optional.empty()).withActorSlot(1);

        context.dispatchStage(
                        send,
                        ZLinkMessage.empty(),
                        new RecordingSession(context, dispatches, dispatchedActor))
                .toCompletableFuture()
                .join();

        assertEquals(1, dispatches.get());
        assertEquals(bound, dispatchedActor.get());
    }

    private static ZLinkStreamSessionContextState context(AtomicInteger closes) {
        return context(closes, null);
    }

    private static ZLinkStreamSessionContextState context(
            AtomicInteger closes, ZLinkBackendStreamSocket stream) {
        return context(closes, stream, null, flow());
    }

    private static ZLinkStreamSessionContextState context(
            AtomicInteger closes,
            ZLinkBackendStreamSocket stream,
            ZLinkSessionActorsRuntime actors,
            ZLinkMessageFlowTracer flow) {
        return new ZLinkStreamSessionContextState(
                "session",
                stream,
                RoutingId.from("client-a"),
                actors,
                null,
                ZLinkStreamCodec.JSON,
                null,
                flow,
                () -> {
                    closes.incrementAndGet();
                    return CompletableFuture.completedFuture(null);
                },
                null);
    }

    private static ZLinkMessageFlowTracer flow() {
        return flow(ZLinkMessageFlowLogMode.OFF);
    }

    private static ZLinkMessageFlowTracer tracedFlow() {
        return flow(ZLinkMessageFlowLogMode.NORMAL);
    }

    private static ZLinkMessageFlowTracer flow(ZLinkMessageFlowLogMode mode) {
        ZLinkDispatchOptionsRegistration options = new ZLinkDispatchOptionsRegistration();
        options.messageFlow(mode);
        return new ZLinkMessageFlowTracer(
                options, ZLinkHandlerActivator.reflection(), Runnable::run);
    }

    private static ZLinkBackendStreamSocket stream(
            AtomicInteger asyncSubmits,
            AtomicInteger syncSubmits,
            CompletableFuture<Void> physicalTerminal,
            AtomicReference<systems.zlink.contracts.messaging.Message> submitted) {
        return stream(asyncSubmits, syncSubmits, physicalTerminal, submitted, null, null);
    }

    private static ZLinkBackendStreamSocket stream(
            AtomicInteger asyncSubmits,
            AtomicInteger syncSubmits,
            CompletableFuture<Void> physicalTerminal,
            AtomicReference<systems.zlink.contracts.messaging.Message> submitted,
            AtomicReference<ZLinkStreamHeader> replyHeader,
            AtomicReference<byte[]> replyPayload) {
        return (ZLinkBackendStreamSocket)
                Proxy.newProxyInstance(
                        ZLinkBackendStreamSocket.class.getClassLoader(),
                        new Class<?>[] {ZLinkBackendStreamSocket.class},
                        (proxy, method, arguments) ->
                                switch (method.getName()) {
                                    case "name" -> "test-stream";
                                    case "bindActor" ->
                                            (ZLinkBackendActorBindOperation)
                                                    timeout ->
                                                            CompletableFuture.completedFuture(null);
                                    case "admitSessionControl" ->
                                            CompletableFuture.completedFuture(null);
                                    case "requestBoundActor" ->
                                            CompletableFuture.completedFuture(List.of());
                                    case "boundActorBindingGeneration" -> 1L;
                                    case "replyAsync" -> {
                                        asyncSubmits.incrementAndGet();
                                        if (replyHeader != null) {
                                            replyHeader.set((ZLinkStreamHeader) arguments[1]);
                                        }
                                        @SuppressWarnings("unchecked")
                                        java.util.List<systems.zlink.contracts.messaging.Message>
                                                parts =
                                                        (java.util.List<
                                                                        systems.zlink.contracts
                                                                                .messaging.Message>)
                                                                arguments[2];
                                        submitted.set(parts.getFirst());
                                        if (replyPayload != null) {
                                            replyPayload.set(parts.getFirst().toByteArray());
                                        }
                                        yield physicalTerminal;
                                    }
                                    case "reply" -> {
                                        syncSubmits.incrementAndGet();
                                        yield false;
                                    }
                                    case "close",
                                            "bind",
                                            "setTlsServer",
                                            "setMaxMessageSize",
                                            "enableNotifications",
                                            "onTransportError",
                                            "startSessionService" ->
                                            null;
                                    default -> defaultValue(method.getReturnType());
                                });
    }

    private static List<String> captureFlow(Runnable action) {
        Logger logger = Logger.getLogger(ZLinkMessageFlowTracer.class.getName());
        List<String> records = new CopyOnWriteArrayList<>();
        Handler handler =
                new Handler() {
                    @Override
                    public void publish(LogRecord record) {
                        records.add(record.getMessage());
                    }

                    @Override
                    public void flush() {}

                    @Override
                    public void close() {}
                };
        Level level = logger.getLevel();
        logger.addHandler(handler);
        logger.setLevel(Level.ALL);
        try {
            action.run();
        } finally {
            logger.removeHandler(handler);
            logger.setLevel(level);
        }
        return records;
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
                ZLinkSessionDispatchContext dispatch, ZLinkMessage payload) {
            return CompletableFuture.failedFuture(new IllegalStateException("handler failure"));
        }
    }

    private static final class RecordingSession implements ZLinkSession {
        private final ZLinkSessionContext context;
        private final AtomicInteger dispatches;
        private final AtomicReference<ZLinkSessionActor> dispatchedActor;

        private RecordingSession(ZLinkSessionContext context, AtomicInteger dispatches) {
            this(context, dispatches, new AtomicReference<>());
        }

        private RecordingSession(
                ZLinkSessionContext context,
                AtomicInteger dispatches,
                AtomicReference<ZLinkSessionActor> dispatchedActor) {
            this.context = context;
            this.dispatches = dispatches;
            this.dispatchedActor = dispatchedActor;
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
                ZLinkSessionDispatchContext dispatch, ZLinkMessage payload) {
            dispatches.incrementAndGet();
            dispatchedActor.set(dispatch.actor());
            return CompletableFuture.completedFuture(null);
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
