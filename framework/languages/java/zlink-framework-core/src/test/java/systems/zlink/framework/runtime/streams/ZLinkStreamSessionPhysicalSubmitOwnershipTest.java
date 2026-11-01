package systems.zlink.framework.runtime.streams;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertInstanceOf;
import static org.junit.jupiter.api.Assertions.assertThrows;
import static org.junit.jupiter.api.Assertions.assertTrue;

import org.junit.jupiter.api.Test;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.framework.configuration.ZLinkMessageFlowLogMode;
import systems.zlink.framework.messaging.ZLinkMessage;
import systems.zlink.framework.runtime.configuration.ZLinkDispatchOptionsRegistration;
import systems.zlink.framework.runtime.diagnostics.ZLinkMessageFlowTracer;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendStreamSocket;
import systems.zlink.framework.runtime.internal.handlers.ZLinkHandlerActivator;
import systems.zlink.framework.runtime.internal.service.ZLinkFrameworkMultipartView;
import systems.zlink.framework.streams.ZLinkSession;
import systems.zlink.framework.streams.ZLinkSessionContext;
import systems.zlink.framework.streams.ZLinkSessionDispatchContext;
import systems.zlink.framework.streams.ZLinkStreamCodec;
import systems.zlink.framework.streams.ZLinkStreamCompressionCodec;
import systems.zlink.framework.streams.ZLinkStreamError;

import java.lang.reflect.Proxy;
import java.util.Map;
import java.util.Optional;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.concurrent.atomic.AtomicReference;

final class ZLinkStreamSessionPhysicalSubmitOwnershipTest {
    private static final RoutingId SESSION = RoutingId.from("session-a");

    @Test
    void sendDelegatesOnceToBackendAsyncAndCleansUpOnPhysicalTerminal() {
        CompletableFuture<Void> physicalTerminal = new CompletableFuture<>();
        AtomicInteger asyncSubmits = new AtomicInteger();
        AtomicInteger syncSubmits = new AtomicInteger();
        ZLinkBackendStreamSocket stream =
                stream(
                        (method, arguments) -> {
                            if (method.getName().equals("sendAsync")) {
                                asyncSubmits.incrementAndGet();
                                return physicalTerminal;
                            }
                            if (method.getName().equals("send")) {
                                syncSubmits.incrementAndGet();
                                return true;
                            }
                            return defaultValue(method.getReturnType());
                        });
        Message payload = Message.from("payload");
        ZLinkStreamSessionSendCall call =
                new ZLinkStreamSessionSendCall(
                        stream,
                        SESSION,
                        ZLinkFrameworkMultipartView.owned(payload),
                        "Packet",
                        Map.of(),
                        false,
                        ZLinkStreamCodec.JSON,
                        null);

        CompletionStage<Void> submission = call.submit();

        assertEquals(1, asyncSubmits.get());
        assertEquals(0, syncSubmits.get());
        assertFalse(submission.toCompletableFuture().isDone());
        assertEquals("payload".length(), payload.size());
        physicalTerminal.complete(null);
        submission.toCompletableFuture().join();
        assertEquals(0, payload.size());
    }

    @Test
    void callerCancellationKeepsSendPendingAndCleansUpOnPhysicalTerminal() {
        CompletableFuture<Void> physicalTerminal = new CompletableFuture<>();
        ZLinkBackendStreamSocket stream =
                stream(
                        (method, arguments) -> {
                            if (method.getName().equals("sendAsync")) {
                                return physicalTerminal;
                            }
                            if (method.getName().equals("send")) {
                                return true;
                            }
                            return defaultValue(method.getReturnType());
                        });
        Message payload = Message.from("payload");
        ZLinkStreamSessionSendCall call =
                new ZLinkStreamSessionSendCall(
                        stream,
                        SESSION,
                        ZLinkFrameworkMultipartView.owned(payload),
                        "Packet",
                        Map.of(),
                        false,
                        ZLinkStreamCodec.JSON,
                        null);

        CompletableFuture<Void> submission = call.submit().toCompletableFuture();

        assertTrue(submission.cancel(false));
        assertFalse(physicalTerminal.isDone());
        assertEquals("payload".length(), payload.size());
        physicalTerminal.complete(null);
        assertEquals(0, payload.size());
    }

    @Test
    void replyDelegatesOnceToBackendAsyncAndWaitsForPhysicalTerminal() {
        CompletableFuture<Void> physicalTerminal = new CompletableFuture<>();
        AtomicInteger asyncSubmits = new AtomicInteger();
        AtomicInteger syncSubmits = new AtomicInteger();
        ZLinkBackendStreamSocket stream =
                stream(
                        (method, arguments) -> {
                            if (method.getName().equals("replyAsync")) {
                                asyncSubmits.incrementAndGet();
                                return physicalTerminal;
                            }
                            if (method.getName().equals("reply")) {
                                syncSubmits.incrementAndGet();
                                return true;
                            }
                            return defaultValue(method.getReturnType());
                        });
        ZLinkStreamSessionContextState context = context(stream);
        Message replyPayload = Message.from("reply");
        ZLinkStreamSessionReplyCall reply =
                new ZLinkStreamSessionReplyCall(
                        stream,
                        SESSION,
                        ZLinkFrameworkMultipartView.owned(replyPayload),
                        context,
                        "Reply",
                        false,
                        ZLinkStreamCodec.JSON,
                        null);
        ZLinkStreamHeader request = new ZLinkStreamHeader("Request", Map.of(), Optional.of(41L));

        CompletionStage<Void> dispatch =
                context.dispatchStage(
                        request, ZLinkMessage.empty(), new ReplyingSession(context, reply));

        assertEquals(1, asyncSubmits.get());
        assertEquals(0, syncSubmits.get());
        assertFalse(dispatch.toCompletableFuture().isDone());
        assertEquals("reply".length(), replyPayload.size());
        physicalTerminal.complete(null);
        dispatch.toCompletableFuture().join();
        assertEquals(0, replyPayload.size());
    }

    @Test
    void synchronousSendFailureReleasesPayloadAndPartsForOneHundredCalls() {
        for (int iteration = 0; iteration < 100; iteration++) {
            AtomicReference<java.util.List<Message>> submitted = new AtomicReference<>();
            ZLinkBackendStreamSocket stream =
                    stream(
                            (method, arguments) -> {
                                if (method.getName().equals("sendAsync")) {
                                    @SuppressWarnings("unchecked")
                                    java.util.List<Message> parts =
                                            (java.util.List<Message>) arguments[2];
                                    submitted.set(parts);
                                    throw new IllegalStateException("backend sync rejection");
                                }
                                return defaultValue(method.getReturnType());
                            });
            Message payload = Message.from("payload");
            ZLinkStreamSessionSendCall call =
                    new ZLinkStreamSessionSendCall(
                            stream,
                            SESSION,
                            ZLinkFrameworkMultipartView.owned(payload),
                            "Packet",
                            Map.of(),
                            false,
                            ZLinkStreamCodec.JSON,
                            null);

            assertThrows(IllegalStateException.class, call::submit);
            assertReleased(payload);
            assertReleased(submitted.get().getFirst());
        }
    }

    @Test
    void synchronousReplyFailureReleasesPayloadAndPartsForOneHundredCalls() {
        for (int iteration = 0; iteration < 100; iteration++) {
            AtomicReference<java.util.List<Message>> submitted = new AtomicReference<>();
            ZLinkBackendStreamSocket stream =
                    stream(
                            (method, arguments) -> {
                                if (method.getName().equals("replyAsync")) {
                                    @SuppressWarnings("unchecked")
                                    java.util.List<Message> parts =
                                            (java.util.List<Message>) arguments[2];
                                    submitted.set(parts);
                                    throw new IllegalStateException("backend sync rejection");
                                }
                                return defaultValue(method.getReturnType());
                            });
            ZLinkStreamSessionContextState context = context(stream);
            Message payload = Message.from("reply");
            AtomicReference<Throwable> synchronousFailure = new AtomicReference<>();
            ZLinkStreamSessionReplyCall call =
                    new ZLinkStreamSessionReplyCall(
                            stream,
                            SESSION,
                            ZLinkFrameworkMultipartView.owned(payload),
                            context,
                            "Reply",
                            false,
                            ZLinkStreamCodec.JSON,
                            null);
            CompletionStage<Void> dispatch =
                    context.dispatchStage(
                            new ZLinkStreamHeader("Request", Map.of(), Optional.of(41L)),
                            ZLinkMessage.empty(),
                            new ReplyingSession(context, call, synchronousFailure));

            dispatch.toCompletableFuture().join();
            assertInstanceOf(IllegalStateException.class, synchronousFailure.get());
            assertReleased(payload);
            assertReleased(submitted.get().getFirst());
        }
    }

    @Test
    void compressionFailureReleasesStreamPayloadForSendAndReply() {
        ZLinkStreamCompressionCodec compressionFailure =
                new ZLinkStreamCompressionCodec() {
                    @Override
                    public byte[] compress(byte[] payload) {
                        throw new IllegalStateException("compression failed");
                    }

                    @Override
                    public byte[] decompress(byte[] payload, int maxDecompressedSize) {
                        return payload;
                    }
                };
        for (int iteration = 0; iteration < 100; iteration++) {
            Message sendPayload = Message.from("send");
            ZLinkStreamSessionSendCall send =
                    new ZLinkStreamSessionSendCall(
                            stream((method, arguments) -> defaultValue(method.getReturnType())),
                            SESSION,
                            ZLinkFrameworkMultipartView.owned(sendPayload),
                            "Packet",
                            Map.of(),
                            true,
                            ZLinkStreamCodec.JSON,
                            compressionFailure);

            assertThrows(IllegalStateException.class, send::submit);
            assertReleased(sendPayload);

            ZLinkBackendStreamSocket stream =
                    stream((method, arguments) -> defaultValue(method.getReturnType()));
            ZLinkStreamSessionContextState context = context(stream);
            Message replyPayload = Message.from("reply");
            AtomicReference<Throwable> synchronousFailure = new AtomicReference<>();
            ZLinkStreamSessionReplyCall reply =
                    new ZLinkStreamSessionReplyCall(
                            stream,
                            SESSION,
                            ZLinkFrameworkMultipartView.owned(replyPayload),
                            context,
                            "Reply",
                            true,
                            ZLinkStreamCodec.JSON,
                            compressionFailure);
            CompletionStage<Void> dispatch =
                    context.dispatchStage(
                            new ZLinkStreamHeader("Request", Map.of(), Optional.of(41L)),
                            ZLinkMessage.empty(),
                            new ReplyingSession(context, reply, synchronousFailure));

            dispatch.toCompletableFuture().join();
            assertInstanceOf(IllegalStateException.class, synchronousFailure.get());
            assertReleased(replyPayload);
        }
    }

    private static void assertReleased(Message message) {
        assertThrows(IllegalStateException.class, message::refCount);
    }

    private static ZLinkStreamSessionContextState context(ZLinkBackendStreamSocket stream) {
        return new ZLinkStreamSessionContextState(
                "stream-node",
                stream,
                SESSION,
                null,
                null,
                ZLinkStreamCodec.JSON,
                null,
                flow(),
                () -> CompletableFuture.completedFuture(null),
                null);
    }

    private static ZLinkMessageFlowTracer flow() {
        ZLinkDispatchOptionsRegistration options = new ZLinkDispatchOptionsRegistration();
        options.messageFlow(ZLinkMessageFlowLogMode.OFF);
        return new ZLinkMessageFlowTracer(
                options, ZLinkHandlerActivator.reflection(), Runnable::run);
    }

    private static ZLinkBackendStreamSocket stream(Invocation invocation) {
        return (ZLinkBackendStreamSocket)
                Proxy.newProxyInstance(
                        ZLinkBackendStreamSocket.class.getClassLoader(),
                        new Class<?>[] {ZLinkBackendStreamSocket.class},
                        (proxy, method, arguments) -> invocation.invoke(method, arguments));
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

    @FunctionalInterface
    private interface Invocation {
        Object invoke(java.lang.reflect.Method method, Object[] arguments) throws Throwable;
    }

    private static final class ReplyingSession implements ZLinkSession {
        private final ZLinkSessionContext context;
        private final ZLinkStreamSessionReplyCall reply;
        private final AtomicReference<Throwable> synchronousFailure;

        private ReplyingSession(ZLinkSessionContext context, ZLinkStreamSessionReplyCall reply) {
            this(context, reply, null);
        }

        private ReplyingSession(
                ZLinkSessionContext context,
                ZLinkStreamSessionReplyCall reply,
                AtomicReference<Throwable> synchronousFailure) {
            this.context = context;
            this.reply = reply;
            this.synchronousFailure = synchronousFailure;
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
            try {
                return reply.submit();
            } catch (RuntimeException | Error failure) {
                if (synchronousFailure == null) {
                    throw failure;
                }
                synchronousFailure.set(failure);
                return CompletableFuture.completedFuture(null);
            }
        }
    }
}
