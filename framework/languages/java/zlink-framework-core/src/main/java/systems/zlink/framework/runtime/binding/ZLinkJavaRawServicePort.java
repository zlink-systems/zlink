package systems.zlink.framework.runtime.binding;

import java.util.ArrayList;
import java.util.IdentityHashMap;
import java.util.List;
import java.util.Map;
import java.util.Objects;
import java.util.Optional;
import java.util.concurrent.atomic.AtomicBoolean;
import java.time.Duration;
import java.util.concurrent.CompletionStage;
import systems.zlink.contracts.core.Context;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.core.Zlink;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.contracts.messaging.Received;
import systems.zlink.contracts.messaging.RequestSubmitOperation;
import systems.zlink.contracts.sockets.RecvFlags;
import systems.zlink.contracts.sockets.RouterSocket;
import systems.zlink.contracts.eventing.MonitorEventType;
import systems.zlink.contracts.eventing.SocketMonitor;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceWireCodec;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceWireFrame;

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

    synchronized RouterSocket openRouter(RoutingId routingId) {
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

    synchronized void ensureReceivePollerRegistered(RouterSocket router) {
        ensureOwned(router);
        receivePollers.get(router).ensureRegistered();
    }

    synchronized CompletionStage<Void> send(
        RouterSocket router,
        RoutingId target,
        List<byte[]> frames) {
        return send(router, target, 0L, 0L, frames);
    }

    synchronized CompletionStage<Void> send(
        RouterSocket router,
        RoutingId target,
        long transportPairId,
        long transportPairGeneration,
        List<byte[]> frames) {
        ensureOwned(router);
        Objects.requireNonNull(target, "target");
        if (frames.isEmpty()) {
            throw new IllegalArgumentException("service multipart must not be empty");
        }
        List<Message> messages = frames.stream()
            .map(frame -> Message.from(Objects.requireNonNull(frame, "frame")))
            .toList();
        boolean submitted = false;
        try {
            var send = transportPairId == 0 || transportPairGeneration == 0
                ? router.send(target)
                : router.send(target, transportPairId,
                    transportPairGeneration);
            var submit = send.message(messages.getFirst());
            for (int index = 1; index < messages.size(); index++) {
                submit.message(messages.get(index));
            }
            CompletionStage<Void> completion = submit.submit()
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

    CompletionStage<Void> sendService(
        RouterSocket router,
        RoutingId target,
        int command,
        int flags,
        List<byte[]> frames) {
        return send(router, target, new ZLinkServiceWireCodec().encode(
            new ZLinkServiceWireFrame(command, flags, frames)));
    }

    synchronized CompletionStage<List<byte[]>> request(
        RouterSocket router,
        RoutingId target,
        List<byte[]> frames,
        Duration timeout) {
        return request(router, target, 0L, 0L, frames, timeout);
    }

    synchronized CompletionStage<List<byte[]>> request(
        RouterSocket router,
        RoutingId target,
        long transportPairId,
        long transportPairGeneration,
        List<byte[]> frames,
        Duration timeout) {
        ensureOwned(router);
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
            var request = transportPairId == 0 || transportPairGeneration == 0
                ? router.request(target)
                : router.request(target, transportPairId,
                    transportPairGeneration);
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

    synchronized void reply(
        RouterSocket router,
        RoutingId target,
        long requestSequence,
        List<byte[]> frames) {
        ensureOwned(router);
        if (requestSequence <= 0 || frames.isEmpty()) {
            throw new IllegalArgumentException(
                "service reply requires request sequence and frames");
        }
        List<Message> messages = frames.stream()
            .map(frame -> Message.from(Objects.requireNonNull(frame, "frame")))
            .toList();
        boolean submitted = false;
        try {
            var submit = router.reply(target, requestSequence)
                .message(messages.getFirst());
            for (int index = 1; index < messages.size(); index++) {
                submit.message(messages.get(index));
            }
            submit.submit();
            submitted = true;
        } finally {
            if (!submitted) {
                Message.closeAll(messages);
            }
        }
    }

    SocketMonitor openMonitor(
        RouterSocket router,
        MonitorEventType... eventTypes) {
        ensureOwned(router);
        return router.monitorOpen(eventTypes);
    }

    synchronized Optional<Inbound> receive(RouterSocket router) {
        ensureOwned(router);
        ZLinkJavaSocketReceivePoller receivePoller = receivePoller(router);
        if (receivePoller == null
            || !receivePoller.waitForReadable(Duration.ZERO)) {
            return Optional.empty();
        }
        Received received = new Received();
        boolean transferred = false;
        try {
            if (!router.recvRetained(received, RecvFlags.DONT_WAIT)) {
                return Optional.empty();
            }
            RoutingId source = received.getRoutingId().orElseThrow(
                () -> new IllegalStateException("service ROUTER receive has no routing id"));
            List<byte[]> frames = received.parts().stream().map(Message::toByteArray).toList();
            Inbound inbound = new Inbound(
                source,
                received.requestSeq().orElse(null),
                received.transportPairId(),
                received.transportPairGeneration(),
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
    public synchronized void close() {
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

    private synchronized void ensureOwned(RouterSocket router) {
        ensureOpen();
        if (!routers.contains(Objects.requireNonNull(router, "router"))) {
            throw new IllegalArgumentException("router is not owned by this service port");
        }
    }

    private synchronized ZLinkJavaSocketReceivePoller receivePoller(
        RouterSocket router) {
        ensureOwned(router);
        return receivePollers.get(router);
    }

    private void ensureOpen() {
        if (closed.get()) {
            throw new IllegalStateException("service port is closed");
        }
    }

    record Inbound(
        RoutingId source,
        Long requestSequence,
        long transportPairId,
        long transportPairGeneration,
        List<byte[]> frames,
        Received retained) implements AutoCloseable {
        Inbound {
            Objects.requireNonNull(source, "source");
            frames = frames.stream().map(byte[]::clone).toList();
            Objects.requireNonNull(retained, "retained");
        }

        @Override
        public List<byte[]> frames() {
            return frames.stream().map(byte[]::clone).toList();
        }

        @Override
        public void close() {
            retained.close();
        }
    }
}
