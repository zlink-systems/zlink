package systems.zlink.framework.runtime.binding;

import static org.junit.jupiter.api.Assertions.assertAll;
import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;

import org.junit.jupiter.api.Test;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.core.Zlink;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.contracts.sockets.SendFlags;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendActorRef;
import systems.zlink.framework.runtime.streams.ZLinkStreamFrameCodec;
import systems.zlink.framework.runtime.streams.ZLinkStreamHeader;
import systems.zlink.framework.runtime.streams.ZLinkStreamHeaderCodec;
import systems.zlink.framework.runtime.streams.ZLinkStreamHeaderFlag;
import systems.zlink.framework.streams.ZLinkStreamCodec;
import systems.zlink.framework.streams.ZLinkStreamMessageKind;

import java.lang.reflect.InvocationTargetException;
import java.lang.reflect.Method;
import java.time.Duration;
import java.util.EnumSet;
import java.util.List;
import java.util.Map;
import java.util.Optional;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicInteger;

final class ZLinkJavaRawSpotNodeBoundSessionAdmissionOwnershipTest {
    @Test
    void localActorAsyncPushCarriesItsPublishedSlot() throws Exception {
        AdmissionSink sink = new AdmissionSink();
        RoutingId sessionRid = RoutingId.from("local-actor-push-session");
        try (var context = Zlink.createContext();
                var node = new ZLinkJavaRawMeshNode(context, "mesh");
                var stream = new ZLinkJavaStreamSocket(context.createStreamSocket(), node, sink);
                var frame =
                        Message.from(
                                ZLinkStreamFrameCodec.encode(
                                        new ZLinkStreamHeader(
                                                ZLinkStreamMessageKind.SEND,
                                                ZLinkStreamCodec.RAW,
                                                EnumSet.noneOf(ZLinkStreamHeaderFlag.class),
                                                Optional.empty(),
                                                "Push",
                                                Map.of()),
                                        new byte[] {1}))) {
            node.setRoutingId(RoutingId.from("local-actor-push-node"));
            ZLinkBackendActorRef actor;
            try (Message create = Message.from("create")) {
                actor = node.spotNode().createActor("local-actor-push", create);
            }
            stream.startSessionService();
            stream.bindActor(sessionRid, actor, 11)
                    .submit(Duration.ofSeconds(1))
                    .toCompletableFuture()
                    .get(1, TimeUnit.SECONDS);
            stream.publishBoundActor(sessionRid, actor.actorId());

            node.spotNode()
                    .sendLocalActorBoundSessionAsync(actor, List.of(frame), Duration.ofSeconds(1));

            assertEquals(11, sink.actorSlot);
            sink.terminal.complete(null);
        }
    }

    @Test
    void boundSessionReplyDelegatesOnceToTheStreamAdmissionTerminal() throws Exception {
        AdmissionSink sink = new AdmissionSink();
        RoutingId sessionRid = RoutingId.from("bound-session-reply");
        try (var context = Zlink.createContext();
                var node = new ZLinkJavaRawMeshNode(context, "mesh");
                var stream = new ZLinkJavaStreamSocket(context.createStreamSocket(), node, sink);
                var reply =
                        Message.from(
                                ZLinkStreamFrameCodec.encode(
                                        ZLinkStreamHeader.createResponse(
                                                requestHeader(41L),
                                                ZLinkStreamCodec.RAW,
                                                EnumSet.noneOf(ZLinkStreamHeaderFlag.class),
                                                "BoundSessionReply",
                                                Map.of()),
                                        new byte[] {1}))) {
            invoke(
                    node.spotNode(),
                    "replyBoundStreamSession",
                    new Class<?>[] {
                        ZLinkJavaStreamSocket.class,
                        RoutingId.class,
                        int.class,
                        ZLinkStreamHeader.class,
                        List.class
                    },
                    stream,
                    sessionRid,
                    7,
                    requestHeader(41L),
                    List.of(reply));

            Thread.sleep(40L);
            assertAll(
                    () -> assertEquals(1, sink.asyncAttempts.get()),
                    () -> assertEquals(0, sink.syncAttempts.get()),
                    () -> assertEquals(7, sink.actorSlot),
                    () -> assertFalse(sink.terminal.isDone()));

            sink.terminal.complete(null);
            sink.terminal.get(1, TimeUnit.SECONDS);
        }
    }

    @Test
    void boundSessionErrorReplyDelegatesOnceToTheStreamAdmissionTerminal() throws Exception {
        AdmissionSink sink = new AdmissionSink();
        RoutingId sessionRid = RoutingId.from("bound-session-error-reply");
        try (var context = Zlink.createContext();
                var node = new ZLinkJavaRawMeshNode(context, "mesh");
                var stream = new ZLinkJavaStreamSocket(context.createStreamSocket(), node, sink)) {
            invoke(
                    node.spotNode(),
                    "replyBoundStreamError",
                    new Class<?>[] {
                        ZLinkJavaStreamSocket.class,
                        RoutingId.class,
                        int.class,
                        ZLinkStreamHeader.class,
                        Throwable.class
                    },
                    stream,
                    sessionRid,
                    9,
                    requestHeader(42L),
                    new IllegalStateException("failed"));

            Thread.sleep(40L);
            assertAll(
                    () -> assertEquals(1, sink.asyncAttempts.get()),
                    () -> assertEquals(0, sink.syncAttempts.get()),
                    () -> assertEquals(9, sink.actorSlot),
                    () -> assertFalse(sink.terminal.isDone()));

            sink.terminal.complete(null);
            sink.terminal.get(1, TimeUnit.SECONDS);
        }
    }

    private static ZLinkStreamHeader requestHeader(long requestSequence) {
        return new ZLinkStreamHeader("BoundSessionRequest", Map.of(), Optional.of(requestSequence));
    }

    private static void invoke(
            Object target, String methodName, Class<?>[] parameterTypes, Object... arguments)
            throws Exception {
        Method method = target.getClass().getDeclaredMethod(methodName, parameterTypes);
        method.setAccessible(true);
        try {
            method.invoke(target, arguments);
        } catch (InvocationTargetException failure) {
            Throwable cause = failure.getCause();
            if (cause instanceof Exception exception) {
                throw exception;
            }
            throw failure;
        }
    }

    private static final class AdmissionSink implements ZLinkJavaStreamSocket.BoundSessionSink {
        private final AtomicInteger syncAttempts = new AtomicInteger();
        private final AtomicInteger asyncAttempts = new AtomicInteger();
        private final CompletableFuture<Void> terminal = new CompletableFuture<>();
        private volatile int actorSlot;

        @Override
        public boolean send(RoutingId sessionRid, List<Message> parts, SendFlags flags) {
            syncAttempts.incrementAndGet();
            return false;
        }

        @Override
        public CompletionStage<Void> sendAsync(
                RoutingId sessionRid, List<Message> parts, Duration timeout) {
            asyncAttempts.incrementAndGet();
            ZLinkStreamFrameCodec.DecodedFrame decoded =
                    ZLinkStreamFrameCodec.tryDecode(parts.getFirst().toByteArray()).orElseThrow();
            actorSlot =
                    ZLinkStreamHeaderCodec.decodeOrPlain(decoded.header())
                            .actorSlot()
                            .orElseThrow();
            return terminal;
        }
    }
}
