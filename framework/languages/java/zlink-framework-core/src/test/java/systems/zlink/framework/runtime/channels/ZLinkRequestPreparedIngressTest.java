package systems.zlink.framework.runtime.channels;

import static org.junit.jupiter.api.Assertions.*;

import org.junit.jupiter.api.RepeatedTest;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.framework.ZLinkMessageContext;
import systems.zlink.framework.channels.ZLinkRequestHandler;
import systems.zlink.framework.channels.ZLinkRouteMessageContext;
import systems.zlink.framework.channels.ZLinkRouteRequestHandler;
import systems.zlink.framework.runtime.configuration.ZLinkDispatchOptionsRegistration;
import systems.zlink.framework.runtime.configuration.ZLinkFrameworkRegistration;
import systems.zlink.framework.runtime.diagnostics.ZLinkDispatchErrorReporter;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendReceived;
import systems.zlink.framework.runtime.internal.configuration.ZLinkCodecRegistration;
import systems.zlink.framework.runtime.internal.handlers.ZLinkHandlerActivator;
import systems.zlink.framework.runtime.internal.messaging.ZLinkOwnedMessageSerializer;
import systems.zlink.framework.runtime.mesh.MeshNodeRegistration;
import systems.zlink.framework.runtime.messaging.ZLinkStringMessageSerializer;

import java.lang.foreign.MemorySegment;
import java.nio.ByteBuffer;
import java.util.AbstractList;
import java.util.List;
import java.util.Map;
import java.util.Optional;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.atomic.AtomicInteger;

final class ZLinkRequestPreparedIngressTest {
    @RepeatedTest(100)
    void clientRequestKeepsTheOriginalFrameThroughHandlerCompletion() {
        verifyRetainedFrame(false, false);
    }

    @RepeatedTest(100)
    void routeRequestKeepsTheOriginalFrameThroughHandlerCompletion() {
        verifyRetainedFrame(true, false);
    }

    @RepeatedTest(100)
    void rejectedClientRequestReleasesTheOriginalFrameOnce() {
        verifyRetainedFrame(false, true);
    }

    @RepeatedTest(100)
    void rejectedRouteRequestReleasesTheOriginalFrameOnce() {
        verifyRetainedFrame(true, true);
    }

    @RepeatedTest(100)
    void cancelledTimedOutAndShutdownClientHandlersReleaseTheFrameOnce() {
        for (int terminal = 1; terminal <= 3; terminal++)
            verifyRetainedFrame(false, false, terminal);
    }

    @RepeatedTest(100)
    void cancelledTimedOutAndShutdownRouteHandlersReleaseTheFrameOnce() {
        for (int terminal = 1; terminal <= 3; terminal++)
            verifyRetainedFrame(true, false, terminal);
    }

    private void verifyRetainedFrame(boolean route, boolean reject) {
        verifyRetainedFrame(route, reject, 0);
    }

    private void verifyRetainedFrame(boolean route, boolean reject, int terminal) {
        var workers = new ZLinkManualDispatchExecutor();
        var registry = new ZLinkChannelDispatchRegistry(workers);
        if (route) {
            registry.registerRoute(
                    "orders",
                    Map.of(),
                    Map.of(
                            "Request",
                            new ChannelRouteRequestHandlerRegistration(
                                    RouteHandler.class, String.class, String.class, "Request")));
        } else {
            registry.registerClientServer(
                    "orders",
                    Map.of(),
                    Map.of(
                            "Request",
                            new ChannelRequestHandlerRegistration(
                                    ClientHandler.class, String.class, String.class, "Request")));
        }
        var pending = new CompletableFuture<String>();
        var handlerCalls = new AtomicInteger();
        var codec = new BorrowedCodec();
        var invoker =
                new ZLinkChannelHandlerInvoker(
                        codec,
                        new ZLinkCodecRegistration(),
                        type ->
                                route
                                        ? new RouteHandler(pending, handlerCalls)
                                        : new ClientHandler(pending, handlerCalls),
                        Runnable::run,
                        List.of());
        var reporter =
                new ZLinkDispatchErrorReporter(
                        new ZLinkDispatchOptionsRegistration(),
                        ZLinkHandlerActivator.reflection(),
                        Runnable::run);
        var errors = new ZLinkChannelDispatchReporter(reporter, new ZLinkCodecRegistration());
        var parts = new OwnedParts(List.of(Message.from("Request"), Message.from("request")));
        codec.expectedAddress = MemorySegment.ofBuffer(parts.get(1).dataBuffer()).address();
        var replies = new AtomicInteger();
        var received =
                new ZLinkBackendReceived(
                        Optional.of(RoutingId.from("source")),
                        Optional.empty(),
                        Optional.of(7L),
                        parts,
                        reply -> {
                            replies.incrementAndGet();
                            Message.closeAll(reply);
                        });
        if (reject) registry.sealClosingAdmission();
        if (route) {
            new ZLinkChannelRouteDispatcher(
                            new ZLinkChannelSocketRegistry(),
                            registry,
                            invoker,
                            errors,
                            reporter.flow(),
                            null,
                            ignored -> null)
                    .dispatch("orders", null, received);
        } else {
            new ZLinkChannelMessageDispatcher(registry, invoker, errors, reporter.flow())
                    .dispatchRequest("orders", null, received);
        }
        try {
            if (!reject) {
                assertEquals(0, parts.closes, "the queued record owns the frame");
                assertEquals(0, codec.decodes, "decode requires the record execution right");
            }
            workers.drain();
            if (!reject) {
                assertEquals(1, codec.decodes);
                assertEquals(1, handlerCalls.get());
                assertEquals(0, parts.closes, "a suspended handler still owns its frame");
                assertEquals("request", parts.get(1).toUtf8String());
                switch (terminal) {
                    case 1 -> pending.cancel(false);
                    case 2 ->
                            pending.completeExceptionally(
                                    new java.util.concurrent.TimeoutException("expired"));
                    case 3 ->
                            pending.completeExceptionally(
                                    new systems.zlink.framework.errors.ZLinkFrameworkException(
                                            systems.zlink.framework.errors.ZLinkFrameworkErrorKind
                                                    .SHUTTING_DOWN,
                                            "shutdown"));
                    default -> pending.complete("reply");
                }
                workers.drain();
            }
            assertEquals(1, replies.get());
            assertEquals(1, parts.closes, "one terminal cleanup for the original frame");
        } finally {
            pending.complete("reply");
            workers.drain();
            parts.release();
        }
    }

    @RepeatedTest(100)
    void meshRequestCodecsAreSelectedAndPreparedBeforeTheFirstRequest() throws Exception {
        var selections = new AtomicInteger();
        var preparations = new AtomicInteger();
        var framework = new ZLinkFrameworkRegistration();
        var serializer =
                new TestSerializer() {
                    @Override
                    public void prepare(Class<?> type) {
                        preparations.incrementAndGet();
                    }
                };
        framework
                .codecs()
                .addSerializer(
                        "application/x-request-test",
                        serializer,
                        type -> {
                            selections.incrementAndGet();
                            return type == String.class;
                        });
        var mesh = new MeshNodeRegistration("mesh");
        mesh.addRouteRequestHandler(RouteHandler.class, String.class, String.class);
        var activations = new AtomicInteger();
        var calls = new AtomicInteger();
        var dispatcher =
                new ZLinkMeshApplicationDispatcher(
                        mesh,
                        framework.codecs().serializerWithFallback(serializer),
                        framework,
                        type -> {
                            activations.incrementAndGet();
                            return new RouteHandler(
                                    CompletableFuture.completedFuture("reply"), calls);
                        },
                        (token, parts) -> {
                            throw new AssertionError("startup must not reply");
                        });
        assertEquals(
                0, activations.get(), "startup prepares plans without creating handler instances");
        assertTrue(preparations.get() > 0, "serializer plans belong to startup");
        assertTrue(selections.get() > 0, "codec choice belongs to startup");
        int preparedSelections = selections.get();
        int preparedSerializers = preparations.get();
        for (int request = 0; request < 5; request++) {
            assertEquals(
                    "application/x-request-test", framework.codecs().contentTypeFor(String.class));
            var completed = new CompletableFuture<Void>();
            var receive =
                    new systems.zlink.framework.runtime.internal.binding.spot.ReceiveRecord(
                            systems.zlink.framework.runtime.internal.binding.spot.RecordKind
                                    .NODE_REQUEST,
                            1,
                            RoutingId.from("source"),
                            null,
                            null,
                            null,
                            null,
                            null,
                            null,
                            null,
                            "application/x-request-test",
                            new byte[0],
                            0,
                            0,
                            0,
                            2);
            dispatcher.accept(
                    new systems.zlink.framework.runtime.internal.backend.ZLinkMeshDispatchRecord(
                            new systems.zlink.framework.runtime.internal.binding.spot.ReadyRecord(
                                    systems.zlink.framework.runtime.internal.binding.spot.OwnerKind
                                            .NODE,
                                    1,
                                    null,
                                    null),
                            receive,
                            List.of(Message.from("String"), Message.from("request")),
                            reply -> {
                                try {
                                    assertEquals("reply", reply.getFirst().toUtf8String());
                                } finally {
                                    Message.closeAll(reply);
                                }
                            },
                            () -> completed.complete(null)));
            completed.get(5, java.util.concurrent.TimeUnit.SECONDS);
        }
        assertEquals(preparedSelections, selections.get(), "requests only look up prepared codecs");
        assertEquals(
                preparedSerializers,
                preparations.get(),
                "serializer preparation never runs on a request");
        assertEquals(5, activations.get(), "each request still creates its own handler instance");
        assertEquals(5, calls.get());
    }

    private static final class OwnedParts extends AbstractList<Message> implements AutoCloseable {
        private final List<Message> parts;
        int closes;

        OwnedParts(List<Message> parts) {
            this.parts = parts;
        }

        @Override
        public Message get(int index) {
            return parts.get(index);
        }

        @Override
        public int size() {
            return parts.size();
        }

        @Override
        public void close() {
            closes++;
            release();
        }

        void release() {
            Message.closeAll(parts);
        }
    }

    private static class TestSerializer implements systems.zlink.framework.ZLinkMessageSerializer {
        private final ZLinkStringMessageSerializer delegate = new ZLinkStringMessageSerializer();

        @Override
        public <T> systems.zlink.framework.ZLinkEncodedPayload serialize(T value) {
            return delegate.serialize(value);
        }

        @Override
        public <T> T deserialize(
                systems.zlink.framework.ZLinkEncodedPayload payload, Class<T> type) {
            return delegate.deserialize(payload, type);
        }
    }

    private static final class BorrowedCodec extends TestSerializer
            implements ZLinkOwnedMessageSerializer {
        long expectedAddress;
        int decodes;

        @Override
        public Message serializeOwned(Object value) {
            return Message.from((String) value);
        }

        @Override
        public <T> T deserializeBorrowed(ByteBuffer payload, Class<T> type) {
            assertEquals(
                    expectedAddress,
                    MemorySegment.ofBuffer(payload).address(),
                    "typed decode must read the received binding frame without copying");
            decodes++;
            return type.cast("request");
        }
    }

    public record ClientHandler(CompletableFuture<String> pending, AtomicInteger calls)
            implements ZLinkRequestHandler<String, String> {
        @Override
        public CompletionStage<String> handle(String request, ZLinkMessageContext context) {
            calls.incrementAndGet();
            return pending;
        }
    }

    public record RouteHandler(CompletableFuture<String> pending, AtomicInteger calls)
            implements ZLinkRouteRequestHandler<String, String> {
        @Override
        public CompletionStage<String> handle(String request, ZLinkRouteMessageContext context) {
            calls.incrementAndGet();
            return pending;
        }
    }
}
