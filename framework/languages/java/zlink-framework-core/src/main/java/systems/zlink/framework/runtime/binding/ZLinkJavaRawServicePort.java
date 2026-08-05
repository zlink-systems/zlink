package systems.zlink.framework.runtime.binding;

import java.util.ArrayList;
import java.util.IdentityHashMap;
import java.util.List;
import java.util.Map;
import java.util.Objects;
import java.util.Optional;
import java.util.concurrent.atomic.AtomicBoolean;
import java.time.Duration;
import java.util.function.BiConsumer;
import java.util.function.Predicate;
import systems.zlink.contracts.core.Context;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.core.Zlink;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.contracts.messaging.Received;
import systems.zlink.contracts.messaging.SendSubmitOperation;
import systems.zlink.contracts.sockets.RecvFlags;
import systems.zlink.contracts.sockets.RouterSocket;
import systems.zlink.contracts.sockets.SendFlags;
import systems.zlink.contracts.sockets.RequestResult;
import systems.zlink.contracts.eventing.MonitorEventType;
import systems.zlink.contracts.eventing.SocketMonitor;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceWireCodec;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceWireFrame;

/**
 * Private binding-facing port for the JVM service runtime.
 *
 * <p>This is the only service runtime class that constructs binding Context,
 * RouterSocket, Received, and Message objects. It copies received frames before
 * returning so binding-owned storage never reaches a handler or mailbox.
 */
final class ZLinkJavaRawServicePort implements AutoCloseable {
    private final Context context;
    private final boolean ownsContext;
    private final Predicate<List<byte[]>> completionControlSelector;
    private final List<RouterSocket> routers = new ArrayList<>();
    private final Map<RouterSocket, ZLinkJavaSocketReceivePoller> receivePollers =
        new IdentityHashMap<>();
    private final AtomicBoolean closed = new AtomicBoolean();

    ZLinkJavaRawServicePort() {
        this(Zlink.createContext(), true, ignored -> false);
    }

    ZLinkJavaRawServicePort(Context context) {
        this(context, false, ignored -> false);
    }

    ZLinkJavaRawServicePort(
        Context context,
        Predicate<List<byte[]>> completionControlSelector) {
        this(context, false, completionControlSelector);
    }

    private ZLinkJavaRawServicePort(
        Context context,
        boolean ownsContext,
        Predicate<List<byte[]>> completionControlSelector) {
        this.context = Objects.requireNonNull(context, "context");
        this.ownsContext = ownsContext;
        this.completionControlSelector = Objects.requireNonNull(
            completionControlSelector, "completionControlSelector");
    }

    synchronized RouterSocket openRouter(RoutingId routingId) {
        ensureOpen();
        RouterSocket router = context.createRouterSocket();
        boolean accepted = false;
        try {
            router.setRoutingId(Objects.requireNonNull(routingId, "routingId"));
            routers.add(router);
            receivePollers.put(router, new ZLinkJavaSocketReceivePoller(router, true));
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

    synchronized boolean send(
        RouterSocket router,
        RoutingId target,
        List<byte[]> frames) {
        ensureOwned(router);
        Objects.requireNonNull(target, "target");
        if (frames.isEmpty()) {
            throw new IllegalArgumentException("service multipart must not be empty");
        }
        if (completionControlSelector.test(frames)) {
            return sendCompletionControl(router, target, frames);
        }
        List<Message> messages = frames.stream()
            .map(frame -> Message.from(Objects.requireNonNull(frame, "frame")))
            .toList();
        boolean submitted = false;
        try {
            SendSubmitOperation submit = router.send(target).message(messages.getFirst());
            for (int index = 1; index < messages.size(); index++) {
                submit.message(messages.get(index));
            }
            submitted = submit.flags(SendFlags.DONT_WAIT).submit();
            return submitted;
        } finally {
            if (!submitted) {
                messages.forEach(Message::close);
            }
        }
    }

    synchronized boolean sendCompletionControl(
        RouterSocket router,
        RoutingId target,
        List<byte[]> frames) {
        ensureOwned(router);
        Objects.requireNonNull(target, "target");
        if (frames.isEmpty()) {
            throw new IllegalArgumentException(
                "completion control multipart must not be empty");
        }
        List<Message> messages = frames.stream()
            .map(frame -> Message.from(Objects.requireNonNull(frame, "frame")))
            .toList();
        try {
            return router.trySendCompletionControl(target, messages);
        } finally {
            messages.forEach(Message::close);
        }
    }

    boolean sendService(
        RouterSocket router,
        RoutingId target,
        int command,
        int flags,
        List<byte[]> frames) {
        return send(router, target, new ZLinkServiceWireCodec().encode(
            new ZLinkServiceWireFrame(command, flags, frames)));
    }

    synchronized boolean request(
        RouterSocket router,
        RoutingId target,
        List<byte[]> frames,
        Duration timeout,
        BiConsumer<RequestResult, List<byte[]>> completion) {
        ensureOwned(router);
        Objects.requireNonNull(target, "target");
        Objects.requireNonNull(timeout, "timeout");
        Objects.requireNonNull(completion, "completion");
        if (frames.isEmpty()) {
            throw new IllegalArgumentException("service request must not be empty");
        }
        List<Message> messages = frames.stream()
            .map(frame -> Message.from(Objects.requireNonNull(frame, "frame")))
            .toList();
        boolean submitted = false;
        try {
            var submit = router.request(target).message(messages.getFirst());
            for (int index = 1; index < messages.size(); index++) {
                submit.message(messages.get(index));
            }
            submitted = submit.timeout(timeout)
                .flags(SendFlags.DONT_WAIT)
                .submit((result, reply) -> {
                    try {
                        completion.accept(
                            result,
                            reply.stream().map(Message::toByteArray).toList());
                    } finally {
                        reply.forEach(Message::close);
                    }
                });
            return submitted;
        } finally {
            if (!submitted) {
                messages.forEach(Message::close);
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
                messages.forEach(Message::close);
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
        try (Received received = new Received()) {
            if (!router.recv(received, RecvFlags.DONT_WAIT)) {
                return Optional.empty();
            }
            RoutingId source = received.getRoutingId().orElseThrow(
                () -> new IllegalStateException("service ROUTER receive has no routing id"));
            List<byte[]> frames = received.parts().stream().map(Message::toByteArray).toList();
            return Optional.of(new Inbound(
                source,
                received.requestSeq().orElse(null),
                frames));
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
        List<byte[]> frames) {
        Inbound {
            Objects.requireNonNull(source, "source");
            frames = frames.stream().map(byte[]::clone).toList();
        }

        @Override
        public List<byte[]> frames() {
            return frames.stream().map(byte[]::clone).toList();
        }
    }
}
