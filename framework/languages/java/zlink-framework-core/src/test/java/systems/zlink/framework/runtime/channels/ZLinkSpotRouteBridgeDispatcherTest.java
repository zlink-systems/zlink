package systems.zlink.framework.runtime.channels;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertInstanceOf;
import static org.junit.jupiter.api.Assertions.assertTrue;

import java.time.Duration;
import java.util.List;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionException;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.Executors;
import java.util.concurrent.ScheduledExecutorService;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.concurrent.atomic.AtomicReference;
import org.junit.jupiter.api.Test;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.framework.errors.ZLinkFrameworkException;
import systems.zlink.framework.errors.ZLinkFrameworkErrorKind;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendRouterSocket;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendSpotRouteBridge;

final class ZLinkSpotRouteBridgeDispatcherTest {
    @Test
    void sendAwaitsOneBindingStageWithoutFrameworkRetry() {
        DirectBridge bridge = new DirectBridge();
        CompletableFuture<Void> result = new CompletableFuture<>();
        Message payload = Message.from("payload");

        ZLinkSpotRouteBridgeDispatcher.submitSend(
            bridge,
            "play.route",
            RoutingId.from("play-node"),
            "room-spot",
            List.of(payload),
            result);

        assertEquals(1, bridge.sendAttempts.get());
        assertFalse(result.isDone());
        assertFalse(payload.empty());

        bridge.sendAdmission.complete(null);

        result.join();
        assertEquals(1, bridge.sendAttempts.get());
        assertTrue(payload.empty());
    }

    @Test
    void channelCloseDoesNotResubmitPendingBindingSend() {
        ScheduledExecutorService deadlines =
            Executors.newSingleThreadScheduledExecutor();
        ZLinkChannelCallRuntime calls = new ZLinkChannelCallRuntime(
            null,
            deadlines,
            null,
            null,
            null,
            null);
        DirectBridge bridge = new DirectBridge();
        CompletableFuture<Void> result = new CompletableFuture<>();
        try {
            calls.track(result, Duration.ofSeconds(1));
            ZLinkSpotRouteBridgeDispatcher.submitSend(
                bridge,
                "play.route",
                RoutingId.from("play-node"),
                "room-spot",
                List.of(Message.from("payload")),
                result);

            calls.beginClose();
            bridge.sendAdmission.complete(null);

            ZLinkFrameworkException failure = assertInstanceOf(
                ZLinkFrameworkException.class,
                completionFailure(result));
            assertEquals(ZLinkFrameworkErrorKind.SHUTTING_DOWN, failure.kind());
            assertEquals(1, bridge.sendAttempts.get());
        } finally {
            calls.beginClose();
            deadlines.shutdownNow();
        }
    }

    @Test
    void requestUsesOneBindingStageAndClosesBindingReply() {
        DirectBridge bridge = new DirectBridge();
        CompletableFuture<List<Message>> result = new CompletableFuture<>();
        Message request = Message.from("request");
        Message reply = Message.from("reply");

        ZLinkSpotRouteBridgeDispatcher.submitRequest(
            bridge,
            "play.route",
            RoutingId.from("play-node"),
            "room-spot",
            List.of(request),
            Duration.ofSeconds(1),
            result);

        bridge.requestReply.complete(List.of(reply));

        List<Message> copied = result.join();
        try {
            assertEquals("reply", copied.get(0).toUtf8String());
            assertEquals(1, bridge.requestAttempts.get());
            assertTrue(request.empty());
            assertTrue(reply.empty());
        } finally {
            copied.forEach(Message::close);
        }
    }

    private static Throwable completionFailure(CompletableFuture<?> result) {
        try {
            result.join();
            throw new AssertionError("result completed successfully");
        } catch (CompletionException failure) {
            return failure.getCause();
        }
    }

    private static final class DirectBridge
        implements ZLinkBackendSpotRouteBridge {
        private final AtomicInteger sendAttempts = new AtomicInteger();
        private final AtomicInteger requestAttempts = new AtomicInteger();
        private final CompletableFuture<Void> sendAdmission =
            new CompletableFuture<>();
        private final CompletableFuture<List<Message>> requestReply =
            new CompletableFuture<>();
        private final AtomicReference<List<Message>> lastSend =
            new AtomicReference<>(List.of());

        @Override
        public void attachRouterChannel(
            String channelName,
            ZLinkBackendRouterSocket router) {
        }

        @Override
        public CompletionStage<Void> send(
            String channelName,
            RoutingId targetNodeRid,
            String targetSpotId,
            List<Message> parts) {
            sendAttempts.incrementAndGet();
            lastSend.set(List.copyOf(parts));
            return sendAdmission;
        }

        @Override
        public CompletionStage<List<Message>> request(
            String channelName,
            RoutingId targetNodeRid,
            String targetSpotId,
            List<Message> parts,
            Duration timeout) {
            requestAttempts.incrementAndGet();
            return requestReply;
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
            return "direct-bridge";
        }

        @Override
        public void close() {
        }
    }
}
