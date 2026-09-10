package systems.zlink.framework.runtime.binding;

import java.util.AbstractList;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Objects;
import java.util.Optional;
import java.util.concurrent.atomic.AtomicBoolean;
import java.time.Duration;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.function.Supplier;
import java.util.function.Function;
import systems.zlink.contracts.core.Context;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.core.Zlink;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.contracts.messaging.Received;
import systems.zlink.contracts.messaging.ReplyToken;
import systems.zlink.contracts.messaging.RequestSubmitOperation;
import systems.zlink.contracts.sockets.RecvFlags;
import systems.zlink.contracts.sockets.RouterSocket;
import systems.zlink.contracts.sockets.RecvResult;
import systems.zlink.contracts.errors.ZlinkRecvException;
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
    private final LinkedHashMap<RouterSocket, ZLinkJavaSocketReceivePoller> receivePollers =
        new LinkedHashMap<>();
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

    RouterSocket openRouter(RoutingId routingId) {
        return inStateLane(() -> openRouterOnLane(routingId));
    }

    private RouterSocket openRouterOnLane(RoutingId routingId) {
        ensureOpen();
        RouterSocket router = context.createRouterSocket();
        boolean accepted = false;
        try {
            router.setRoutingId(Objects.requireNonNull(routingId, "routingId"));
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
        return sendMessages(router, target,
            copyMessages(frames, "service multipart must not be empty"));
    }

    /** Consumes every message on every return or throw path. */
    CompletionStage<Void> sendMessages(
        RouterSocket router,
        RoutingId target,
        List<Message> messages) {
        List<Message> ownedMessages = claimMessages(messages);
        boolean completionOwns = false;
        try {
            ensureOwned(router);
            Objects.requireNonNull(target, "target");
            if (ownedMessages.isEmpty()) {
                throw new IllegalArgumentException(
                    "service multipart must not be empty");
            }
            var send = router.send(target);
            var submit = send.message(ownedMessages.getFirst());
            for (int index = 1; index < ownedMessages.size(); index++) {
                submit.message(ownedMessages.get(index));
            }
            CompletionStage<Void> completion;
            try {
                completion = submit.submit().admitted();
            } catch (RuntimeException failure) {
                completion = CompletableFuture.failedFuture(failure);
            }
            completion = completion.whenComplete((ignored, failure) ->
                Message.closeAll(ownedMessages));
            completionOwns = true;
            return completion;
        } finally {
            if (!completionOwns) {
                Message.closeAll(ownedMessages);
            }
        }
    }

    private static List<Message> claimMessages(List<Message> messages) {
        Objects.requireNonNull(messages, "messages");
        try {
            return List.copyOf(messages);
        } catch (RuntimeException | Error failure) {
            Message.closeAll(messages);
            throw failure;
        }
    }

    private static List<Message> copyMessages(
        List<byte[]> frames,
        String emptyMessage) {
        Objects.requireNonNull(frames, "frames");
        if (frames.isEmpty()) {
            throw new IllegalArgumentException(emptyMessage);
        }
        var messages = new java.util.ArrayList<Message>(frames.size());
        try {
            for (byte[] frame : frames) {
                messages.add(Message.from(
                    Objects.requireNonNull(frame, "frame")));
            }
            return List.copyOf(messages);
        } catch (RuntimeException | Error failure) {
            Message.closeAll(messages);
            throw failure;
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
        return request(router, target, frames, timeout,
            reply -> reply.stream().map(Message::toByteArray).toList());
    }

    <T> CompletionStage<T> request(
        RouterSocket router,
        RoutingId target,
        List<byte[]> frames,
        Duration timeout,
        Function<List<Message>, T> decodeReply) {
        return requestMessages(router, target,
            copyMessages(frames, "service request must not be empty"),
            timeout,
            decodeReply);
    }

    /** Consumes every request message on every return or throw path. */
    <T> CompletionStage<T> requestMessages(
        RouterSocket router,
        RoutingId target,
        List<Message> messages,
        Duration timeout,
        Function<List<Message>, T> decodeReply) {
        List<Message> ownedMessages = claimMessages(messages);
        boolean completionOwns = false;
        try {
            ensureOwned(router);
            Objects.requireNonNull(decodeReply, "decodeReply");
            Objects.requireNonNull(target, "target");
            Objects.requireNonNull(timeout, "timeout");
            if (ownedMessages.isEmpty()) {
                throw new IllegalArgumentException(
                    "service request must not be empty");
            }
            var request = router.request(target);
            RequestSubmitOperation submit = request.message(
                ownedMessages.getFirst());
            for (int index = 1; index < ownedMessages.size(); index++) {
                submit.message(ownedMessages.get(index));
            }
            CompletionStage<T> completion = submit.timeout(timeout)
                .submit()
                .reply()
                .thenApply(reply -> {
                    try {
                        return decodeReply.apply(reply);
                    } finally {
                        reply.forEach(Message::close);
                    }
                })
                .whenComplete((ignored, failure) ->
                    Message.closeAll(ownedMessages));
            completionOwns = true;
            return completion;
        } finally {
            if (!completionOwns) {
                Message.closeAll(ownedMessages);
            }
        }
    }

    void reply(
        RouterSocket router,
        RoutingId target,
        ReplyToken requestSequence,
        List<byte[]> frames) {
        ensureOwned(router);
        replyOnLane(router, target, requestSequence, frames);
    }

    private void replyOnLane(
        RouterSocket router,
        RoutingId target,
        ReplyToken requestSequence,
        List<byte[]> frames) {
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

    boolean waitForReadable(RouterSocket router, Duration timeout) {
        Objects.requireNonNull(timeout, "timeout");
        ZLinkJavaSocketReceivePoller receivePoller =
            inStateLane(() -> receivePollerOnLane(router));
        return receivePoller != null
            && receivePoller.waitForReadable(timeout);
    }

    Optional<Inbound> receiveNow(RouterSocket router) {
        return inStateLane(() -> receiveOnLane(router));
    }

    private Optional<Inbound> receiveOnLane(RouterSocket router) {
        ensureOwnedOnLane(router);
        Received received = new Received();
        boolean transferred = false;
        try {
            boolean receivedRecord;
            try {
                receivedRecord = router.recv(received, RecvFlags.DONT_WAIT);
            } catch (ZlinkRecvException noData) {
                if (noData.getResult() == RecvResult.NO_DATA
                    || noData.getResult() == RecvResult.BUSY) {
                    return Optional.empty();
                }
                throw noData;
            }
            if (!receivedRecord) {
                return Optional.empty();
            }
            RoutingId source = received.getRoutingId().orElseThrow(
                () -> new IllegalStateException("service ROUTER receive has no routing id"));
            // Control decoders request individual headers. Application decoders
            // borrow received.parts() directly while this owner is retained.
            List<byte[]> frames = new AbstractList<>() {
                @Override
                public byte[] get(int index) {
                    return received.parts().get(index).toByteArray();
                }

                @Override
                public int size() {
                    return received.parts().size();
                }
            };
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
        for (var entry : receivePollers.reversed().entrySet()) {
            entry.getValue().close();
            entry.getKey().close();
        }
        receivePollers.clear();
        if (ownsContext) {
            context.close();
        }
    }

    private void ensureOwnedOnLane(RouterSocket router) {
        ensureOpen();
        if (!receivePollers.containsKey(Objects.requireNonNull(router, "router"))) {
            throw new IllegalArgumentException("router is not owned by this service port");
        }
    }

    private void ensureOwned(RouterSocket router) {
        inStateLane(() -> {
            ensureOwnedOnLane(router);
            return null;
        });
    }

    private ZLinkJavaSocketReceivePoller receivePollerOnLane(
        RouterSocket router) {
        ensureOwnedOnLane(router);
        return receivePollers.get(router);
    }

    private synchronized <T> T inStateLane(Supplier<T> work) {
        return work.get();
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
            Objects.requireNonNull(frames, "frames");
            Objects.requireNonNull(received, "received");
        }

        @Override
        public List<byte[]> frames() {
            return frames;
        }

        @Override
        public void close() {
            received.close();
        }
    }
}
