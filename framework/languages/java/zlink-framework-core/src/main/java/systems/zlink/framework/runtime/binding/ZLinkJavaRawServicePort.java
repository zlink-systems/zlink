package systems.zlink.framework.runtime.binding;

import java.util.ArrayList;
import java.util.IdentityHashMap;
import java.util.List;
import java.util.Map;
import java.util.Objects;
import java.util.Optional;
import java.util.concurrent.atomic.AtomicBoolean;
import java.time.Duration;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.CompletionException;
import java.util.function.Supplier;
import systems.zlink.contracts.core.Context;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.core.Zlink;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.contracts.messaging.Received;
import systems.zlink.contracts.messaging.ReplyToken;
import systems.zlink.contracts.messaging.RequestSubmitOperation;
import systems.zlink.contracts.sockets.RecvFlags;
import systems.zlink.contracts.sockets.RouterSocket;
import systems.zlink.contracts.eventing.MonitorEventType;
import systems.zlink.contracts.eventing.SocketMonitor;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceWireCodec;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceWireFrame;
import systems.zlink.framework.runtime.internal.execution.ZLinkStateLane;

/**
 * Private binding-facing port for the JVM service runtime.
 *
 * <p>This is the only service runtime class that constructs binding Context,
 * RouterSocket, Received, and Message objects. Inbound application dispatch
 * retains its native receive owner until the framework record is terminal.
 */
final class ZLinkJavaRawServicePort implements AutoCloseable {
    private final Context context;
    private final boolean ownsContext;
    private final List<RouterSocket> routers = new ArrayList<>();
    private final Map<RouterSocket, ZLinkJavaSocketReceivePoller> receivePollers =
        new IdentityHashMap<>();
    private final AtomicBoolean closed = new AtomicBoolean();
    private final ZLinkStateLane stateLane = new ZLinkStateLane();

    ZLinkJavaRawServicePort() {
        this(Zlink.createContext(), true);
    }

    ZLinkJavaRawServicePort(Context context) {
        this(context, false);
    }

    private ZLinkJavaRawServicePort(
        Context context,
        boolean ownsContext) {
        this.context = Objects.requireNonNull(context, "context");
        this.ownsContext = ownsContext;
    }

    RouterSocket openRouter(RoutingId routingId) {
        return inStateLane(() -> openRouterOnLane(routingId));
    }

    private RouterSocket openRouterOnLane(RoutingId routingId) {
        ensureOpen();
        RouterSocket router = context.createRouterSocket();
        boolean accepted = false;
        try {
            router.setRoutingId(Objects.requireNonNull(routingId, "routingId"));
            routers.add(router);
            receivePollers.put(router, new ZLinkJavaSocketReceivePoller(router));
            accepted = true;
            return router;
        } finally {
            if (!accepted) {
                receivePollers.remove(router);
                router.close();
            }
        }
    }

    void ensureReceivePollerRegistered(RouterSocket router) {
        inStateLane(() -> {
            ensureOwnedOnLane(router);
            receivePollers.get(router).ensureRegistered();
            return null;
        });
    }

    CompletionStage<Void> send(
        RouterSocket router,
        RoutingId target,
        List<byte[]> frames) {
        return inStateLane(() -> sendOnLane(router, target, frames));
    }

    private CompletionStage<Void> sendOnLane(
        RouterSocket router,
        RoutingId target,
        List<byte[]> frames) {
        ensureOwnedOnLane(router);
        Objects.requireNonNull(target, "target");
        if (frames.isEmpty()) {
            throw new IllegalArgumentException("service multipart must not be empty");
        }
        List<Message> messages = frames.stream()
            .map(frame -> Message.from(Objects.requireNonNull(frame, "frame")))
            .toList();
        boolean submitted = false;
        try {
            var send = router.send(target);
            var submit = send.message(messages.getFirst());
            for (int index = 1; index < messages.size(); index++) {
                submit.message(messages.get(index));
            }
            CompletionStage<Void> completion;
            try {
                completion = submit.submit();
            } catch (RuntimeException failure) {
                completion = CompletableFuture.failedFuture(failure);
            }
            completion = completion.whenComplete((ignored, failure) ->
                Message.closeAll(messages));
            submitted = true;
            return completion;
        } finally {
            if (!submitted) {
                Message.closeAll(messages);
            }
        }
    }

    CompletionStage<Void> sendService(
        RouterSocket router,
        RoutingId target,
        int command,
        int flags,
        List<byte[]> frames) {
        return send(router, target, new ZLinkServiceWireCodec().encode(
            new ZLinkServiceWireFrame(command, flags, frames)));
    }

    CompletionStage<List<byte[]>> request(
        RouterSocket router,
        RoutingId target,
        List<byte[]> frames,
        Duration timeout) {
        return inStateLane(() -> requestOnLane(
            router, target, frames, timeout));
    }

    private CompletionStage<List<byte[]>> requestOnLane(
        RouterSocket router,
        RoutingId target,
        List<byte[]> frames,
        Duration timeout) {
        ensureOwnedOnLane(router);
        Objects.requireNonNull(target, "target");
        Objects.requireNonNull(timeout, "timeout");
        if (frames.isEmpty()) {
            throw new IllegalArgumentException("service request must not be empty");
        }
        List<Message> messages = frames.stream()
            .map(frame -> Message.from(Objects.requireNonNull(frame, "frame")))
            .toList();
        boolean submitted = false;
        try {
            var request = router.request(target);
            RequestSubmitOperation submit = request.message(messages.getFirst());
            for (int index = 1; index < messages.size(); index++) {
                submit.message(messages.get(index));
            }
            CompletionStage<List<byte[]>> completion = submit.timeout(timeout)
                .submit()
                .thenApply(reply -> {
                    try {
                        return reply.stream().map(Message::toByteArray).toList();
                    } finally {
                        reply.forEach(Message::close);
                    }
                })
                .whenComplete((ignored, failure) ->
                    Message.closeAll(messages));
            submitted = true;
            return completion;
        } finally {
            if (!submitted) {
                Message.closeAll(messages);
            }
        }
    }

    void reply(
        RouterSocket router,
        RoutingId target,
        ReplyToken requestSequence,
        List<byte[]> frames) {
        inStateLane(() -> {
            replyOnLane(router, target, requestSequence, frames);
            return null;
        });
    }

    private void replyOnLane(
        RouterSocket router,
        RoutingId target,
        ReplyToken requestSequence,
        List<byte[]> frames) {
        ensureOwnedOnLane(router);
        if (requestSequence == null || frames.isEmpty()) {
            throw new IllegalArgumentException(
                "service reply requires request sequence and frames");
        }
        List<Message> messages = frames.stream()
            .map(frame -> Message.from(Objects.requireNonNull(frame, "frame")))
            .toList();
        try {
            var submit = router.reply(target, requestSequence)
                .message(messages.getFirst());
            for (int index = 1; index < messages.size(); index++) {
                submit.message(messages.get(index));
            }
            submit.submit();
        } finally {
            Message.closeAll(messages);
        }
    }

    SocketMonitor openMonitor(
        RouterSocket router,
        MonitorEventType... eventTypes) {
        return inStateLane(() -> {
            ensureOwnedOnLane(router);
            return router.monitorOpen(eventTypes);
        });
    }

    Optional<Inbound> receive(RouterSocket router) {
        ZLinkJavaSocketReceivePoller receivePoller =
            inStateLane(() -> receivePollerOnLane(router));
        if (receivePoller == null
            || !receivePoller.waitForReadable(Duration.ZERO)) {
            return Optional.empty();
        }
        return inStateLane(() -> receiveOnLane(router));
    }

    private Optional<Inbound> receiveOnLane(RouterSocket router) {
        ensureOwnedOnLane(router);
        Received received = new Received();
        boolean transferred = false;
        try {
            if (!router.recv(received, RecvFlags.DONT_WAIT)) {
                return Optional.empty();
            }
            RoutingId source = received.getRoutingId().orElseThrow(
                () -> new IllegalStateException("service ROUTER receive has no routing id"));
            List<byte[]> frames = received.parts().stream().map(Message::toByteArray).toList();
            Inbound inbound = new Inbound(
                source,
                received.replyToken().orElse(null),
                frames,
                received);
            transferred = true;
            return Optional.of(inbound);
        } finally {
            if (!transferred) {
                received.close();
            }
        }
    }

    @Override
    public void close() {
        inStateLane(() -> {
            closeOnLane();
            return null;
        });
    }

    private void closeOnLane() {
        if (!closed.compareAndSet(false, true)) {
            return;
        }
        for (int index = routers.size() - 1; index >= 0; index--) {
            RouterSocket router = routers.get(index);
            ZLinkJavaSocketReceivePoller receivePoller = receivePollers.remove(router);
            if (receivePoller != null) {
                receivePoller.close();
            }
            router.close();
        }
        receivePollers.clear();
        routers.clear();
        if (ownsContext) {
            context.close();
        }
    }

    private void ensureOwnedOnLane(RouterSocket router) {
        ensureOpen();
        if (!routers.contains(Objects.requireNonNull(router, "router"))) {
            throw new IllegalArgumentException("router is not owned by this service port");
        }
    }

    private ZLinkJavaSocketReceivePoller receivePollerOnLane(
        RouterSocket router) {
        ensureOwnedOnLane(router);
        return receivePollers.get(router);
    }

    private <T> T inStateLane(Supplier<T> work) {
        try {
            return stateLane.runAsync(work).toCompletableFuture().join();
        } catch (CompletionException failure) {
            Throwable cause = failure.getCause();
            if (cause instanceof RuntimeException runtimeFailure) {
                throw runtimeFailure;
            }
            if (cause instanceof Error error) {
                throw error;
            }
            throw failure;
        }
    }

    private void ensureOpen() {
        if (closed.get()) {
            throw new IllegalStateException("service port is closed");
        }
    }

    record Inbound(
        RoutingId source,
        ReplyToken requestSequence,
        List<byte[]> frames,
        Received received) implements AutoCloseable {
        Inbound {
            Objects.requireNonNull(source, "source");
            frames = frames.stream().map(byte[]::clone).toList();
            Objects.requireNonNull(received, "received");
        }

        @Override
        public List<byte[]> frames() {
            return frames.stream().map(byte[]::clone).toList();
        }

        @Override
        public void close() {
            received.close();
        }
    }
}
