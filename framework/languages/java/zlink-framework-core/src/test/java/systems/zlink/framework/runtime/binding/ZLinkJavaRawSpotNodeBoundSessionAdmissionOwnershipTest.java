package systems.zlink.framework.runtime.binding;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertAll;

import java.lang.reflect.InvocationTargetException;
import java.lang.reflect.Method;
import java.time.Duration;
import java.util.List;
import java.util.Map;
import java.util.Optional;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicInteger;
import org.junit.jupiter.api.Test;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.core.Zlink;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.contracts.sockets.SendFlags;
import systems.zlink.framework.runtime.streams.ZLinkStreamHeader;

final class ZLinkJavaRawSpotNodeBoundSessionAdmissionOwnershipTest {
    @Test
    void boundSessionReplyDelegatesOnceToTheStreamAdmissionTerminal()
        throws Exception {
        AdmissionSink sink = new AdmissionSink();
        RoutingId sessionRid = RoutingId.from("bound-session-reply");
        try (var context = Zlink.createContext();
             var node = new ZLinkJavaRawMeshNode(context, "mesh");
             var stream = new ZLinkJavaStreamSocket(
                 context.createStreamSocket(), node, sink);
             var reply = Message.from("encoded-reply")) {
            invoke(
                node.spotNode(),
                "replyBoundStreamSession",
                new Class<?>[] {
                    ZLinkJavaStreamSocket.class,
                    RoutingId.class,
                    ZLinkStreamHeader.class,
                    List.class
                },
                stream,
                sessionRid,
                requestHeader(41L),
                List.of(reply));

            Thread.sleep(40L);
            assertAll(
                () -> assertEquals(1, sink.asyncAttempts.get()),
                () -> assertEquals(0, sink.syncAttempts.get()),
                () -> assertFalse(sink.terminal.isDone()));

            sink.terminal.complete(null);
            sink.terminal.get(1, TimeUnit.SECONDS);
        }
    }

    @Test
    void boundSessionErrorReplyDelegatesOnceToTheStreamAdmissionTerminal()
        throws Exception {
        AdmissionSink sink = new AdmissionSink();
        RoutingId sessionRid = RoutingId.from("bound-session-error-reply");
        try (var context = Zlink.createContext();
             var node = new ZLinkJavaRawMeshNode(context, "mesh");
             var stream = new ZLinkJavaStreamSocket(
                 context.createStreamSocket(), node, sink)) {
            invoke(
                node.spotNode(),
                "replyBoundStreamError",
                new Class<?>[] {
                    ZLinkJavaStreamSocket.class,
                    RoutingId.class,
                    ZLinkStreamHeader.class,
                    Throwable.class
                },
                stream,
                sessionRid,
                requestHeader(42L),
                new IllegalStateException("failed"));

            Thread.sleep(40L);
            assertAll(
                () -> assertEquals(1, sink.asyncAttempts.get()),
                () -> assertEquals(0, sink.syncAttempts.get()),
                () -> assertFalse(sink.terminal.isDone()));

            sink.terminal.complete(null);
            sink.terminal.get(1, TimeUnit.SECONDS);
        }
    }

    private static ZLinkStreamHeader requestHeader(long requestSequence) {
        return new ZLinkStreamHeader(
            "BoundSessionRequest",
            Map.of(),
            Optional.of(requestSequence));
    }

    private static void invoke(
        Object target,
        String methodName,
        Class<?>[] parameterTypes,
        Object... arguments) throws Exception {
        Method method = target.getClass().getDeclaredMethod(
            methodName, parameterTypes);
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

    private static final class AdmissionSink
        implements ZLinkJavaStreamSocket.BoundSessionSink {
        private final AtomicInteger syncAttempts = new AtomicInteger();
        private final AtomicInteger asyncAttempts = new AtomicInteger();
        private final CompletableFuture<Void> terminal =
            new CompletableFuture<>();

        @Override
        public boolean send(
            RoutingId sessionRid,
            List<Message> parts,
            SendFlags flags) {
            syncAttempts.incrementAndGet();
            return false;
        }

        @Override
        public CompletionStage<Void> sendAsync(
            RoutingId sessionRid,
            List<Message> parts,
            Duration timeout) {
            asyncAttempts.incrementAndGet();
            return terminal;
        }
    }
}
