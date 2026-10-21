package systems.zlink.framework.runtime.binding;

import systems.zlink.contracts.core.Context;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.core.Zlink;
import systems.zlink.contracts.errors.ZlinkRecvException;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.contracts.messaging.Received;
import systems.zlink.contracts.messaging.ReplyToken;
import systems.zlink.contracts.messaging.RequestSubmission;
import systems.zlink.contracts.messaging.RequestSubmitOperation;
import systems.zlink.contracts.sockets.RecvFlags;
import systems.zlink.contracts.sockets.RecvResult;
import systems.zlink.contracts.sockets.RouterRoute;
import systems.zlink.contracts.sockets.RouterSocket;
import systems.zlink.framework.errors.ZLinkFrameworkException;
import systems.zlink.framework.runtime.internal.ZLinkCompletionBridge;
import systems.zlink.framework.runtime.internal.calls.ZLinkOneWayCalls;
import systems.zlink.framework.runtime.internal.execution.ZLinkStateLane;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceOperationRegistry;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceWireCodec;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceWireFrame;

import java.time.Duration;
import java.util.AbstractList;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Objects;
import java.util.Optional;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.function.BiConsumer;
import java.util.function.BiFunction;
import java.util.function.Consumer;
import java.util.function.Function;
import java.util.function.Supplier;

/**
 * Private binding-facing port for the JVM service runtime.
 *
 * <p>This is the only service runtime class that constructs binding Context, RouterSocket,
 * Received, and Message objects. Inbound application dispatch retains its native receive owner
 * until the framework record is terminal.
 */
final class ZLinkJavaRawServicePort implements AutoCloseable {
    static final BiFunction<RouterSocket, Boolean, ZLinkJavaSocketReceivePoller>
            DEFAULT_RECEIVE_POLLER =
                    (socket, observesRoutes) ->
                            new ZLinkJavaSocketReceivePoller(socket, true, observesRoutes);
    private final BiFunction<RouterSocket, Boolean, ZLinkJavaSocketReceivePoller>
            receivePollerFactory;
    private static final Duration INFINITE_WAIT = Duration.ofMillis(-1);
    private final Context context;
    private final boolean ownsContext;
    private final LinkedHashMap<RouterSocket, ZLinkJavaSocketReceivePoller> receivePollers =
            new LinkedHashMap<>();
    private final ZLinkStateLane stateLane;
    private final ZLinkServiceOperationRegistry operations;
    private CompletableFuture<Void> closeCompletion;

    private Function<
                    RoutingId,
                    systems.zlink.framework.runtime.internal.service.ZLinkServiceLivenessRegistry
                            .PeerState>
            receiveSource;

    void setReceiveSource(
            Function<
                            RoutingId,
                            systems.zlink.framework.runtime.internal.service
                                    .ZLinkServiceLivenessRegistry.PeerState>
                    source) {
        receiveSource = source;
    }

    ZLinkJavaRawServicePort() {
        this(Zlink.createContext(), true);
    }

    ZLinkJavaRawServicePort(Context context) {
        this(context, false);
    }

    private ZLinkJavaRawServicePort(Context context, boolean ownsContext) {
        this(context, ownsContext, new ZLinkStateLane());
    }

    ZLinkJavaRawServicePort(Context context, ZLinkStateLane stateLane) {
        this(context, false, stateLane);
    }

    private ZLinkJavaRawServicePort(
            Context context, boolean ownsContext, ZLinkStateLane stateLane) {
        this(
                context,
                ownsContext,
                stateLane,
                new ZLinkServiceOperationRegistry(
                        ZLinkOneWayCalls.failureForStatus(ZLinkOneWayCalls.SHUTDOWN)));
    }

    ZLinkJavaRawServicePort(
            Context context, ZLinkStateLane stateLane, ZLinkServiceOperationRegistry operations) {
        this(context, false, stateLane, operations);
    }

    private ZLinkJavaRawServicePort(
            Context context,
            boolean ownsContext,
            ZLinkStateLane stateLane,
            ZLinkServiceOperationRegistry operations) {
        this(context, ownsContext, stateLane, operations, DEFAULT_RECEIVE_POLLER);
    }

    ZLinkJavaRawServicePort(
            Context context,
            ZLinkStateLane stateLane,
            ZLinkServiceOperationRegistry operations,
            BiFunction<RouterSocket, Boolean, ZLinkJavaSocketReceivePoller> receivePollerFactory) {
        this(context, false, stateLane, operations, receivePollerFactory);
    }

    private ZLinkJavaRawServicePort(
            Context context,
            boolean ownsContext,
            ZLinkStateLane stateLane,
            ZLinkServiceOperationRegistry operations,
            BiFunction<RouterSocket, Boolean, ZLinkJavaSocketReceivePoller> receivePollerFactory) {
        this.receivePollerFactory =
                Objects.requireNonNull(receivePollerFactory, "receivePollerFactory");
        this.operations = Objects.requireNonNull(operations, "operations");
        this.context = Objects.requireNonNull(context, "context");
        this.ownsContext = ownsContext;
        this.stateLane = Objects.requireNonNull(stateLane, "stateLane");
    }

    RouterSocket openRouter(RoutingId routingId) {
        return openRouter(routingId, false);
    }

    /**
     * {@code observesRoutes} makes the caller's receive loop this ROUTER's one route observer: its
     * waits report {@code POLLROUTE}, and it must read {@link #routesSnapshot} on each report (Core
     * ROUTER §10.1).
     */
    RouterSocket openRouter(RoutingId routingId, boolean observesRoutes) {
        return inStateLane(() -> openRouterOnLane(routingId, observesRoutes));
    }

    private RouterSocket openRouterOnLane(RoutingId routingId, boolean observesRoutes) {
        ensureOpen();
        RouterSocket router = context.createRouterSocket();
        boolean accepted = false;
        try {
            router.setRoutingId(Objects.requireNonNull(routingId, "routingId"));
            receivePollers.put(router, receivePollerFactory.apply(router, observesRoutes));
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
        inStateLane(() -> receivePollerOnLane(router)).ensureRegistered();
    }

    CompletionStage<Void> send(RouterSocket router, RoutingId target, List<byte[]> frames) {
        return sendMessages(
                router, target, copyMessages(frames, "service multipart must not be empty"));
    }

    /** Consumes every message on every return or throw path. */
    CompletionStage<Void> sendMessages(
            RouterSocket router, RoutingId target, List<Message> messages) {
        List<Message> ownedMessages = claimMessages(messages);
        boolean completionOwns = false;
        try {
            CompletionStage<Void> completion =
                    inStateLane(
                            () -> {
                                ensureOwnedOnLane(router);
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
                                try {
                                    return ZLinkJavaSocketSupport.submit(submit, operations);
                                } catch (RuntimeException failure) {
                                    return CompletableFuture.failedFuture(failure);
                                }
                            });
            if (ZLinkOneWayCalls.isImmediateAdmission(completion)) {
                Message.closeAll(ownedMessages);
            } else {
                completion =
                        completion.whenComplete(
                                (ignored, failure) -> Message.closeAll(ownedMessages));
            }
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

    private static List<Message> copyMessages(List<byte[]> frames, String emptyMessage) {
        Objects.requireNonNull(frames, "frames");
        if (frames.isEmpty()) {
            throw new IllegalArgumentException(emptyMessage);
        }
        var messages = new java.util.ArrayList<Message>(frames.size());
        try {
            for (byte[] frame : frames) {
                messages.add(Message.from(Objects.requireNonNull(frame, "frame")));
            }
            return List.copyOf(messages);
        } catch (RuntimeException | Error failure) {
            Message.closeAll(messages);
            throw failure;
        }
    }

    CompletionStage<Void> sendService(
            RouterSocket router, RoutingId target, int command, int flags, List<byte[]> frames) {
        return send(
                router,
                target,
                new ZLinkServiceWireCodec()
                        .encode(new ZLinkServiceWireFrame(command, flags, frames)));
    }

    CompletionStage<List<byte[]>> request(
            RouterSocket router,
            RoutingId target,
            List<byte[]> frames,
            Duration timeout,
            CompletableFuture<Void> requestAdmission) {
        return request(
                router,
                target,
                frames,
                timeout,
                reply -> reply.stream().map(Message::toByteArray).toList(),
                requestAdmission);
    }

    <T> CompletionStage<T> request(
            RouterSocket router,
            RoutingId target,
            List<byte[]> frames,
            Duration timeout,
            Function<List<Message>, T> decodeReply,
            CompletableFuture<Void> requestAdmission) {
        return requestMessages(
                router,
                target,
                copyMessages(frames, "service request must not be empty"),
                timeout,
                decodeReply,
                ignored -> {},
                null,
                requestAdmission);
    }

    /** Consumes request messages and discards a decoded reply if cancellation wins. */
    <T> CompletionStage<T> requestMessages(
            RouterSocket router,
            RoutingId target,
            List<Message> messages,
            Duration timeout,
            Function<List<Message>, T> decodeReply,
            Consumer<T> discardReply,
            systems.zlink.framework.runtime.internal.service.ZLinkServiceLivenessRegistry.PeerState
                    receivedAdmission,
            CompletableFuture<Void> requestAdmission) {
        Objects.requireNonNull(discardReply, "discardReply");
        CompletableFuture<T> completion = new CompletableFuture<>();
        CompletionStage<?> bindingReply =
                requestMessagesTo(
                        router,
                        target,
                        messages,
                        timeout,
                        decodeReply,
                        null,
                        (value, failure) -> {
                            if (failure != null) completion.completeExceptionally(failure);
                            else
                                ZLinkCompletionBridge.completeOrDiscard(
                                        completion, value, discardReply);
                        },
                        receivedAdmission,
                        requestAdmission,
                        (submission, deadline, admission, admitted, terminal) -> {
                            var reply =
                                    ZLinkJavaSocketSupport.reply(
                                            submission, deadline, admission, admitted);
                            reply.whenComplete(terminal);
                            return reply;
                        });
        if (bindingReply != null) {
            ZLinkCompletionBridge.forwardCancellation(completion, bindingReply);
        }
        return completion;
    }

    /**
     * Consumes request messages and reports the request's terminal to {@code terminal} exactly
     * once, without a stage of its own. Binding failures use the shared request failure projection.
     * A decoded reply, or {@code mapFailure}'s projection of a decode failure, is the value; any
     * other outcome is the failure. Returns the binding stage that caller cancellation must reach,
     * or {@code null} when the request failed before submission.
     */
    <T> CompletionStage<?> requestMessagesTo(
            RouterSocket router,
            RoutingId target,
            List<Message> messages,
            Duration timeout,
            Function<List<Message>, T> decodeReply,
            Function<Throwable, T> mapFailure,
            BiConsumer<T, Throwable> terminal,
            systems.zlink.framework.runtime.internal.service.ZLinkServiceLivenessRegistry.PeerState
                    receivedAdmission,
            CompletableFuture<Void> requestAdmission) {
        return requestMessagesTo(
                router,
                target,
                messages,
                timeout,
                decodeReply,
                mapFailure,
                terminal,
                receivedAdmission,
                requestAdmission,
                ZLinkJavaSocketSupport::observeReply);
    }

    @FunctionalInterface
    private interface ReplyObserver {
        CompletionStage<List<Message>> observe(
                RequestSubmission submission,
                long deadlineNanos,
                Runnable admission,
                CompletableFuture<Void> requestAdmission,
                BiConsumer<List<Message>, Throwable> terminal);
    }

    private <T> CompletionStage<?> requestMessagesTo(
            RouterSocket router,
            RoutingId target,
            List<Message> messages,
            Duration timeout,
            Function<List<Message>, T> decodeReply,
            Function<Throwable, T> mapFailure,
            BiConsumer<T, Throwable> terminal,
            systems.zlink.framework.runtime.internal.service.ZLinkServiceLivenessRegistry.PeerState
                    receivedAdmission,
            CompletableFuture<Void> requestAdmission,
            ReplyObserver replyObserver) {
        long startedAt = System.nanoTime();
        List<Message> ownedMessages = claimMessages(messages);
        boolean replyOwns = false;
        try {
            Objects.requireNonNull(decodeReply, "decodeReply");
            Objects.requireNonNull(terminal, "terminal");
            Objects.requireNonNull(target, "target");
            Objects.requireNonNull(timeout, "timeout");
            long deadlineNanos = startedAt + timeout.toNanos();
            if (ownedMessages.isEmpty()) {
                throw new IllegalArgumentException("service request must not be empty");
            }
            var admission =
                    receivedAdmission != null
                            ? receivedAdmission
                            : receiveSource == null ? null : receiveSource.apply(target);
            var submission =
                    inStateLane(
                            () -> {
                                ensureOwnedOnLane(router);
                                var request = router.request(target);
                                RequestSubmitOperation submit =
                                        request.message(ownedMessages.getFirst());
                                for (int index = 1; index < ownedMessages.size(); index++) {
                                    submit.message(ownedMessages.get(index));
                                }
                                return submit.timeout(timeout).submit();
                            });
            CompletionStage<List<Message>> bindingReply =
                    replyObserver.observe(
                            submission,
                            deadlineNanos,
                            admission,
                            requestAdmission,
                            (reply, failure) -> {
                                try {
                                    deliverTerminal(
                                            reply, failure, decodeReply, mapFailure, terminal);
                                } finally {
                                    if (reply != null) Message.closeAll(reply);
                                    Message.closeAll(ownedMessages);
                                }
                            });
            replyOwns = true;
            return bindingReply;
        } catch (RuntimeException failure) {
            deliverTerminal(null, failure, decodeReply, mapFailure, terminal);
            return null;
        } finally {
            if (!replyOwns) {
                Message.closeAll(ownedMessages);
            }
        }
    }

    private static <T> void deliverTerminal(
            List<Message> reply,
            Throwable failure,
            Function<List<Message>, T> decodeReply,
            Function<Throwable, T> mapFailure,
            BiConsumer<T, Throwable> terminal) {
        if (failure != null) {
            terminal.accept(null, ZLinkJavaRawMeshNode.requestFailure(failure));
            return;
        }
        T value = null;
        Throwable terminalFailure = null;
        try {
            value = decodeReply.apply(reply);
        } catch (RuntimeException | Error error) {
            terminalFailure = error;
        }
        if (terminalFailure != null && mapFailure != null) {
            if (ZLinkJavaRawMeshNode.unwrap(terminalFailure)
                    instanceof ZLinkFrameworkException framework) {
                terminalFailure = framework;
            } else {
                try {
                    value = mapFailure.apply(terminalFailure);
                    terminalFailure = null;
                } catch (RuntimeException | Error error) {
                    terminalFailure = error;
                }
            }
        }
        terminal.accept(terminalFailure == null ? value : null, terminalFailure);
    }

    void reply(
            RouterSocket router,
            RoutingId target,
            ReplyToken requestSequence,
            List<byte[]> frames) {
        replyMessages(
                router,
                target,
                requestSequence,
                copyMessages(frames, "service reply requires request sequence and frames"));
    }

    /** Consumes every reply message on every return or throw path. */
    void replyMessages(
            RouterSocket router,
            RoutingId target,
            ReplyToken requestSequence,
            List<Message> replyMessages) {
        List<Message> messages = claimMessages(replyMessages);
        try {
            inStateLane(
                    () -> {
                        ensureOwnedOnLane(router);
                        if (requestSequence == null || messages.isEmpty()) {
                            throw new IllegalArgumentException(
                                    "service reply requires request sequence and frames");
                        }
                        var submit =
                                router.reply(target, requestSequence).message(messages.getFirst());
                        for (int index = 1; index < messages.size(); index++) {
                            submit.message(messages.get(index));
                        }
                        submit.submit();
                        return null;
                    });
        } finally {
            Message.closeAll(messages);
        }
    }

    /** Reads Core's selected routes. Only the socket's route observer calls this. */
    List<RouterRoute> routesSnapshot(RouterSocket router) {
        return inStateLane(
                () -> {
                    ensureOwnedOnLane(router);
                    return router.routesSnapshot();
                });
    }

    Optional<Inbound> receive(RouterSocket router) {
        ZLinkJavaSocketReceivePoller receivePoller = inStateLane(() -> receivePollerOnLane(router));
        if (receivePoller == null || !receivePoller.waitForReadable(Duration.ZERO)) {
            return Optional.empty();
        }
        return inStateLane(() -> receiveOnLane(router));
    }

    boolean waitForReadable(RouterSocket router, Duration timeout) {
        return (waitForReady(router, timeout) & ZLinkJavaSocketReceivePoller.READABLE) != 0;
    }

    /** Waits once and returns {@link ZLinkJavaSocketReceivePoller} readiness bits. */
    int waitForReady(RouterSocket router, Duration timeout) {
        Objects.requireNonNull(timeout, "timeout");
        ZLinkJavaSocketReceivePoller receivePoller = inStateLane(() -> receivePollerOnLane(router));
        return receivePoller == null
                ? 0
                : consumeOwnerWake(receivePoller, receivePoller.waitForOwnerReady(timeout));
    }

    int waitForManagementReady(RouterSocket router, long deadline, long now) {
        var receivePoller =
                inStateLane(
                        () -> {
                            var owner = receivePollerOnLane(router);
                            owner.prepareManagementWait(deadline, now);
                            return owner;
                        });
        return consumeOwnerWake(receivePoller, receivePoller.waitForOwnerReady(INFINITE_WAIT));
    }

    private int consumeOwnerWake(ZLinkJavaSocketReceivePoller owner, int ready) {
        if ((ready & ZLinkJavaSocketReceivePoller.OWNER_WAKE) != 0) {
            inStateLane(
                    () -> {
                        if (closeCompletion == null) owner.consumeOwnerWake();
                        return null;
                    });
        }
        return ready & ~ZLinkJavaSocketReceivePoller.OWNER_WAKE;
    }

    void signalReceiveOwner(RouterSocket router) {
        if (router == null) return;
        inStateLane(
                () -> {
                    // The close claim seals producers before resource disposal outside the lane.
                    // An in-flight signal has left this lane before close can claim disposal.
                    if (closeCompletion == null) {
                        var receivePoller = receivePollers.get(router);
                        if (receivePoller != null) receivePoller.signal();
                    }
                    return null;
                });
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
            RoutingId source =
                    received.getRoutingId()
                            .orElseThrow(
                                    () ->
                                            new IllegalStateException(
                                                    "service ROUTER receive has no routing id"));
            // Control decoders request individual headers. Application decoders
            // borrow received.parts() directly while this owner is retained.
            Inbound inbound = new Inbound(source, received.replyToken().orElse(null), received);
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
        var attempt = new CompletableFuture<Void>();
        var completion =
                inStateLane(
                        () -> {
                            if (closeCompletion == null) {
                                closeCompletion = attempt;
                                operations.close();
                            }
                            return closeCompletion;
                        });
        if (completion == attempt) {
            try {
                // Claim close after every started native submission has left the state turn.
                // Pending operation completion and resource disposal remain outside the turn.
                var remaining =
                        inStateLane(() -> List.copyOf(receivePollers.reversed().entrySet()));
                for (var entry : remaining) {
                    entry.getValue().close();
                    entry.getKey().close();
                    inStateLane(() -> receivePollers.remove(entry.getKey()));
                }
                if (ownsContext) context.close();
                attempt.complete(null);
            } catch (RuntimeException | Error failure) {
                // Retain resources whose disposal failed for the caller's next close.
                inStateLane(
                        () -> {
                            closeCompletion = null;
                            return null;
                        });
                attempt.completeExceptionally(failure);
            }
        }
        ZLinkCompletionBridge.await(completion);
    }

    private void ensureOwnedOnLane(RouterSocket router) {
        ensureOpen();
        if (!receivePollers.containsKey(Objects.requireNonNull(router, "router"))) {
            throw new IllegalArgumentException("router is not owned by this service port");
        }
    }

    private ZLinkJavaSocketReceivePoller receivePollerOnLane(RouterSocket router) {
        ensureOwnedOnLane(router);
        return receivePollers.get(router);
    }

    <T> T inStateLane(Supplier<T> work) {
        return stateLane.isOnLane() ? work.get() : stateLane.awaitTurn(work);
    }

    private void ensureOpen() {
        if (closeCompletion != null) {
            throw new IllegalStateException("service port is closed");
        }
    }

    /**
     * A received record. For a native record it is also its own header-frame view, so no separate
     * adapter is allocated per record; a synthetic record keeps its supplied frames. The list view
     * always matches {@link #frames()}.
     */
    static final class Inbound extends AbstractList<byte[]> implements AutoCloseable {
        private final RoutingId source;
        private final ReplyToken requestSequence;
        private final List<byte[]> frames;
        private final Received received;

        Inbound(RoutingId source, ReplyToken requestSequence, Received received) {
            this.source = Objects.requireNonNull(source, "source");
            this.requestSequence = requestSequence;
            this.received = Objects.requireNonNull(received, "received");
            frames = this;
        }

        // Synthetic records can supply their existing frame representation without a native owner.
        Inbound(
                RoutingId source,
                ReplyToken requestSequence,
                List<byte[]> frames,
                Received received) {
            this.source = Objects.requireNonNull(source, "source");
            this.requestSequence = requestSequence;
            this.frames = Objects.requireNonNull(frames, "frames");
            this.received = Objects.requireNonNull(received, "received");
        }

        RoutingId source() {
            return source;
        }

        ReplyToken requestSequence() {
            return requestSequence;
        }

        List<byte[]> frames() {
            return frames;
        }

        Received received() {
            return received;
        }

        @Override
        public byte[] get(int index) {
            return frames == this ? received.parts().get(index).toByteArray() : frames.get(index);
        }

        @Override
        public int size() {
            return frames == this ? received.parts().size() : frames.size();
        }

        /** The Core route generation that delivered this record (Core ROUTER §10.1). */
        long routeGeneration() {
            return received.routeGeneration();
        }

        @Override
        public void close() {
            received.close();
        }
    }
}
