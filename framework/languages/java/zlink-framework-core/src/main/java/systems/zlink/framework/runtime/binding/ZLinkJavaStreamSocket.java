package systems.zlink.framework.runtime.binding;
import java.util.Optional;
import systems.zlink.framework.streams.ZLinkStreamMessageKind;

import java.time.Duration;
import java.util.ArrayList;
import java.util.List;
import java.util.Map;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionException;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.ConcurrentHashMap;
import java.util.concurrent.ExecutionException;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.TimeoutException;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.concurrent.atomic.AtomicLong;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.errors.ZlinkRequestException;
import systems.zlink.contracts.errors.ZlinkSubmitException;
import systems.zlink.contracts.eventing.MonitorEventType;
import systems.zlink.contracts.eventing.SocketMonitor;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.contracts.messaging.Received;
import systems.zlink.contracts.sockets.RecvFlags;
import systems.zlink.contracts.sockets.RequestResult;
import systems.zlink.contracts.sockets.SendFlags;
import systems.zlink.contracts.sockets.Socket;
import systems.zlink.contracts.sockets.StreamSocket;
import systems.zlink.contracts.sockets.SubmitResult;
import systems.zlink.runtime.framework.FrameworkStreamOperations;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendActorBindOperation;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendActorRef;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendActorUnbindOperation;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendStreamErrorHandler;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendStreamReceived;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendStreamSocket;
import systems.zlink.framework.runtime.streams.ZLinkStreamHeader;
import systems.zlink.framework.runtime.streams.ZLinkStreamHeaderCodec;

final class ZLinkJavaStreamSocket implements ZLinkBackendStreamSocket, ZLinkJavaSocketBacked {
    private final StreamSocket socket;
    private final ZLinkJavaRawMeshNode meshNode;
    private final BoundSessionSink boundSessionSink;
    private final BoundSessionLifecycle boundSessionLifecycle;
    private final Runnable nativeClose;
    private final Map<BindingKey, SessionBinding> bindings =
        new ConcurrentHashMap<>();
    private final AtomicLong nextBoundSessionSequence =
        new AtomicLong(1);
    private final AtomicLong nextBoundSessionRequestSequence =
        new AtomicLong(1);
    private final AtomicBoolean closed = new AtomicBoolean();
    private final ZLinkJavaSocketReceivePoller receivePoller;
    private SocketMonitor monitor;
    private boolean sessionServiceStarted;

    ZLinkJavaStreamSocket(StreamSocket socket, ZLinkJavaRawMeshNode meshNode) {
        this(socket, meshNode, null, null, null);
    }

    ZLinkJavaStreamSocket(
        StreamSocket socket,
        ZLinkJavaRawMeshNode meshNode,
        BoundSessionSink boundSessionSink) {
        this(socket, meshNode, boundSessionSink, null, null);
    }

    ZLinkJavaStreamSocket(
        StreamSocket socket,
        ZLinkJavaRawMeshNode meshNode,
        BoundSessionSink boundSessionSink,
        BoundSessionLifecycle boundSessionLifecycle) {
        this(
            socket,
            meshNode,
            boundSessionSink,
            boundSessionLifecycle,
            null);
    }

    ZLinkJavaStreamSocket(
        StreamSocket socket,
        ZLinkJavaRawMeshNode meshNode,
        BoundSessionSink boundSessionSink,
        BoundSessionLifecycle boundSessionLifecycle,
        Runnable nativeClose) {
        this.socket = socket;
        this.meshNode = meshNode;
        this.boundSessionSink = boundSessionSink;
        this.boundSessionLifecycle = boundSessionLifecycle;
        this.receivePoller = new ZLinkJavaSocketReceivePoller(socket);
        this.nativeClose = nativeClose == null
            ? socket::close
            : nativeClose;
    }

    @Override public synchronized Socket nativeSocket() { return socket; }
    @Override public synchronized String name() { return "stream"; }
    @Override public synchronized String lastEndpoint() {
        return socket.options().lastEndpoint();
    }
    @Override public synchronized void setTlsServer(
        String certificatePath,
        String keyPath,
        boolean requireClientCertificate) {
        socket.setTlsServer(certificatePath, keyPath, requireClientCertificate);
    }
    @Override public synchronized void setMaxMessageSize(long value) {
        socket.options().maxMessageSize(value == 0 ? -1 : value);
    }
    @Override public synchronized void bind(String endpoint) { socket.bind(endpoint); }
    @Override public synchronized void enableNotifications() {
        socket.options().notify(true);
    }
    @Override public boolean waitForReadable(Duration timeout) {
        return receivePoller.waitForReadable(timeout);
    }
    @Override public synchronized ZLinkBackendStreamReceived recv() {
        Received received = new Received();
        boolean transferred = false;
        try {
            if (!socket.recvRetained(received, RecvFlags.DONT_WAIT)) {
                return null;
            }
            ZLinkBackendStreamReceived result = new ZLinkBackendStreamReceived(
                received.getRoutingId(),
                received.parts(),
                received::close);
            transferred = true;
            return result;
        } finally {
            if (!transferred) {
                received.close();
            }
        }
    }
    @Override public synchronized void disconnectPeer(RoutingId routingId) {
        socket.disconnectRid(routingId);
    }
    @Override public synchronized void onTransportError(ZLinkBackendStreamErrorHandler handler) {
        closeMonitor();
        monitor = socket.monitorOpen(MonitorEventType.DISCONNECTED);
        monitor.onEvent(event -> event.routingId().ifPresent(routingId ->
            handler.handle(routingId, 0, event.event().name())));
    }
    @Override public void startSessionService() {
        if (meshNode != null && !sessionServiceStarted) {
            sessionServiceStarted = true;
        }
    }
    @Override public synchronized boolean send(RoutingId routingId, List<Message> parts, SendFlags flags) {
        return ZLinkJavaStreamFraming.submit(socket.send(routingId), 1, null, null, parts, flags);
    }
    @Override public synchronized boolean sendBoundSessionPush(
        RoutingId routingId,
        List<Message> parts,
        SendFlags flags) {
        if (boundSessionSink != null) {
            return boundSessionSink.send(routingId, parts, flags);
        }
        if (parts == null || parts.size() != 1) {
            throw new IllegalArgumentException(
                "bound Session push requires one encoded STREAM frame");
        }
        return socket.send(routingId)
            .message(parts.getFirst())
            .flags(flags)
            .submit();
    }

    CompletionStage<Void> sendBoundSessionPushAsync(
        RoutingId routingId,
        List<Message> parts,
        Duration timeout) {
        if (boundSessionSink != null) {
            return boundSessionSink.sendAsync(routingId, parts, timeout);
        }
        if (parts == null || parts.size() != 1) {
            throw new IllegalArgumentException(
                "bound Session push requires one encoded STREAM frame");
        }
        return FrameworkStreamOperations.send(
            socket, routingId, parts, timeout);
    }

    @Override public CompletionStage<Void> sendBoundSessionPushAsync(
        RoutingId routingId,
        List<Message> parts) {
        return sendBoundSessionPushAsync(
            routingId, parts, admissionTimeout());
    }
    @Override public synchronized boolean send(RoutingId routingId, String packetName, List<Message> parts, SendFlags flags) {
        return ZLinkJavaStreamFraming.submit(socket.send(routingId), 1, null, packetName, parts, flags);
    }
    @Override public synchronized boolean send(RoutingId routingId, ZLinkStreamHeader header, List<Message> parts, SendFlags flags) {
        return ZLinkJavaStreamFraming.submit(socket.send(routingId), header, parts, flags);
    }
    @Override public CompletionStage<Void> sendAsync(
        RoutingId routingId,
        ZLinkStreamHeader header,
        List<Message> parts) {
        return sendAsync(routingId, header, parts, admissionTimeout());
    }
    @Override public CompletionStage<Void> sendAsync(
        RoutingId routingId,
        ZLinkStreamHeader header,
        List<Message> parts,
        Duration timeout) {
        return submitStreamFrameAsync(routingId, header, parts, timeout);
    }
    @Override public synchronized boolean reply(RoutingId routingId, long requestSeq, String packetName, List<Message> parts, SendFlags flags) {
        return ZLinkJavaStreamFraming.submit(socket.send(routingId), 3, requestSeq, packetName, parts, flags);
    }
    @Override public synchronized boolean reply(RoutingId routingId, ZLinkStreamHeader header, List<Message> parts, SendFlags flags) {
        return ZLinkJavaStreamFraming.submit(socket.send(routingId), header, parts, flags);
    }
    @Override public CompletionStage<Void> replyAsync(
        RoutingId routingId,
        ZLinkStreamHeader header,
        List<Message> parts) {
        return submitStreamFrameAsync(
            routingId, header, parts, admissionTimeout());
    }

    private CompletionStage<Void> submitStreamFrameAsync(
        RoutingId routingId,
        ZLinkStreamHeader header,
        List<Message> parts,
        Duration timeout) {
        Message frame;
        try {
            frame = ZLinkJavaStreamFraming.frame(header, parts);
        } catch (RuntimeException failure) {
            return CompletableFuture.failedFuture(failure);
        }
        try {
            return FrameworkStreamOperations.send(
                socket, routingId, List.of(frame), timeout);
        } finally {
            frame.close();
        }
    }
    @Override public ZLinkBackendActorBindOperation bindActor(RoutingId sessionRid, ZLinkBackendActorRef actor) {
        return timeout -> {
            requireSessionRuntime();
            BindingKey key = new BindingKey(sessionRid, actor.actorId());
            long generation =
                rawSpotNode().allocateStreamBindingGeneration();
            SessionBinding candidate =
                new SessionBinding(actor, generation);
            CompletionStage<Void> bound = boundSessionLifecycle == null
                ? rawSpotNode().bindStreamSession(
                    sessionRid,
                    actor,
                    generation,
                    this,
                    timeout)
                : boundSessionLifecycle.bind(
                    sessionRid,
                    actor,
                    generation,
                    timeout);
            return bound.thenRun(() ->
                bindings.compute(key, (ignored, current) ->
                    current == null
                        || current.generation() < candidate.generation()
                        ? candidate
                        : current));
        };
    }
    @Override public ZLinkBackendActorUnbindOperation unbindActor(RoutingId sessionRid, String actorId) {
        return timeout -> {
            BindingKey key = new BindingKey(sessionRid, actorId);
            SessionBinding binding = requireBinding(sessionRid, actorId);
            CompletionStage<Void> unbound = unbindBinding(
                sessionRid,
                binding,
                timeout);
            return unbound.thenRun(() ->
                bindings.remove(key, binding));
        };
    }
    @Override public boolean sendBoundActor(RoutingId sessionRid, String actorId, List<Message> parts, SendFlags flags) {
        SessionBinding binding =
            requireBinding(sessionRid, actorId);
        return rawSpotNode().forwardBoundStreamSession(
            binding.actor(),
            sessionRid,
            binding.generation(),
            allocateBoundSessionSequence(),
            this,
            parts);
    }
    @Override public boolean relayBoundActor(
        RoutingId sessionRid,
        String actorId,
        ZLinkStreamHeader streamHeader,
        List<Message> parts,
        SendFlags flags) {
        return relayBoundActor(
            sessionRid,
            actorId,
            allocateBoundSessionSequence(),
            streamHeader,
            parts,
            flags);
    }

    @Override public CompletionStage<Void> relayBoundActorAsync(
        RoutingId sessionRid,
        String actorId,
        ZLinkStreamHeader streamHeader,
        List<Message> parts) {
        return relayBoundActorAsync(
            sessionRid,
            actorId,
            allocateBoundSessionSequence(),
            streamHeader,
            parts);
    }

    @Override public CompletionStage<Void> relayBoundActorAsync(
        RoutingId sessionRid,
        String actorId,
        long sourceSessionSequence,
        ZLinkStreamHeader streamHeader,
        List<Message> parts) {
        if (sourceSessionSequence <= 0) {
            return CompletableFuture.failedFuture(
                new IllegalArgumentException(
                    "bound Session ingress sequence must be positive"));
        }
        Message header = Message.from(
            ZLinkStreamHeaderCodec.encode(streamHeader));
        CompletionStage<Void> submission;
        try {
            SessionBinding binding = requireBinding(sessionRid, actorId);
            submission = rawSpotNode().forwardBoundStreamSessionAsync(
                binding.actor(),
                sessionRid,
                binding.generation(),
                sourceSessionSequence,
                this,
                streamHeader,
                prepend(header, parts));
        } catch (RuntimeException failure) {
            header.close();
            return CompletableFuture.failedFuture(failure);
        }
        submission.whenComplete((ignored, failure) -> header.close());
        return submission;
    }

    @Override public boolean relayBoundActor(
        RoutingId sessionRid,
        String actorId,
        long sourceSessionSequence,
        ZLinkStreamHeader streamHeader,
        List<Message> parts,
        SendFlags flags) {
        if (sourceSessionSequence <= 0) {
            throw new IllegalArgumentException(
                "bound Session ingress sequence must be positive");
        }
        Message header = Message.from(ZLinkStreamHeaderCodec.encode(streamHeader));
        try {
            SessionBinding binding =
                requireBinding(sessionRid, actorId);
            return rawSpotNode().forwardBoundStreamSession(
                binding.actor(),
                sessionRid,
                binding.generation(),
                sourceSessionSequence,
                this,
                streamHeader,
                prepend(header, parts));
        } finally {
            header.close();
        }
    }

    @Override public CompletionStage<List<Message>> requestBoundActor(
        RoutingId sessionRid,
        String actorId,
        ZLinkStreamHeader header,
        List<Message> parts,
        Duration timeout) {
        Message encodedHeader = null;
        try {
            SessionBinding binding = requireBinding(sessionRid, actorId);
            long requestSequence = allocateBoundSessionRequestSequence();
            ZLinkStreamHeader requestHeader = new ZLinkStreamHeader(
                ZLinkStreamMessageKind.REQUEST,
                header.codec(),
                header.flags(),
                Optional.of(requestSequence),
                header.name(),
                header.metadata(),
                header.correlationId(),
                header.flowId(),
                header.flowOrigin());
            encodedHeader = Message.from(
                ZLinkStreamHeaderCodec.encode(requestHeader));
            return rawSpotNode().requestBoundStreamSession(
                binding.actor(),
                sessionRid,
                binding.generation(),
                allocateBoundSessionSequence(),
                this,
                requestHeader,
                prepend(encodedHeader, parts),
                timeout);
        } finally {
            if (encodedHeader != null) {
                encodedHeader.close();
            }
        }
    }

    @Override public CompletionStage<List<Message>> requestExactActor(
        ZLinkBackendActorRef actor,
        ZLinkStreamHeader header,
        List<Message> parts,
        Duration timeout) {
        Message encodedHeader = null;
        try {
            long requestSequence = allocateBoundSessionRequestSequence();
            ZLinkStreamHeader requestHeader = new ZLinkStreamHeader(
                ZLinkStreamMessageKind.REQUEST,
                header.codec(),
                header.flags(),
                Optional.of(requestSequence),
                header.name(),
                header.metadata(),
                header.correlationId(),
                header.flowId(),
                header.flowOrigin());
            encodedHeader = Message.from(
                ZLinkStreamHeaderCodec.encode(requestHeader));
            return rawSpotNode().requestToActor(
                actor,
                prepend(encodedHeader, parts),
                SendFlags.DONT_WAIT,
                timeout);
        } finally {
            if (encodedHeader != null) {
                encodedHeader.close();
            }
        }
    }
    @Override public synchronized void close() {
        if (!closed.compareAndSet(false, true)) {
            return;
        }
        receivePoller.close();
        closeMonitor();
        Throwable cleanupFailure = null;
        if (meshNode != null) {
            Map<BindingKey, SessionBinding> closingBindings =
                Map.copyOf(bindings);
            List<CompletableFuture<Void>> cleanup =
                new ArrayList<>(closingBindings.size());
            closingBindings.forEach((key, binding) -> {
                try {
                    cleanup.add(unbindBinding(
                        key.sessionRid(),
                        binding,
                        Duration.ofMillis(250))
                        .<Void>handle((ignored, failure) -> {
                            if (failure != null
                                && !isStaleBinding(failure)
                                && !isRemoteRouteUnavailable(failure)) {
                                Throwable cause = unwrapFailure(failure);
                                if (cause instanceof RuntimeException runtime) {
                                    throw runtime;
                                }
                                if (cause instanceof Error error) {
                                    throw error;
                                }
                                throw new CompletionException(cause);
                            }
                            return null;
                        })
                        .toCompletableFuture());
                } catch (RuntimeException | Error failure) {
                    cleanup.add(
                        CompletableFuture.failedFuture(failure));
                }
            });
            try {
                CompletableFuture.allOf(
                    cleanup.toArray(CompletableFuture[]::new))
                    .get(500, TimeUnit.MILLISECONDS);
            } catch (InterruptedException interrupted) {
                Thread.currentThread().interrupt();
                cleanupFailure = interrupted;
            } catch (ExecutionException | TimeoutException failure) {
                cleanupFailure = failure instanceof ExecutionException
                    && failure.getCause() != null
                    ? failure.getCause()
                    : failure;
            } finally {
                closingBindings.forEach((key, binding) ->
                    rawSpotNode().discardStreamSession(
                        key.sessionRid(),
                        binding.actor(),
                        binding.generation(),
                        this));
                bindings.clear();
            }
        }
        Throwable nativeCloseFailure = null;
        try {
            nativeClose.run();
        } catch (RuntimeException | Error failure) {
            nativeCloseFailure = failure;
        }
        if (cleanupFailure != null) {
            IllegalStateException failure = new IllegalStateException(
                "STREAM binding cleanup did not complete",
                cleanupFailure);
            if (nativeCloseFailure != null) {
                failure.addSuppressed(nativeCloseFailure);
            }
            throw failure;
        }
        if (nativeCloseFailure instanceof RuntimeException failure) {
            throw failure;
        }
        if (nativeCloseFailure instanceof Error failure) {
            throw failure;
        }
    }

    private void closeMonitor() {
        if (monitor != null) {
            monitor.close();
            monitor = null;
        }
    }

    private static List<Message> prepend(Message first, List<Message> rest) {
        ArrayList<Message> result = new ArrayList<>(rest.size() + 1);
        result.add(first);
        result.addAll(rest);
        return result;
    }

    private SessionBinding requireBinding(
        RoutingId sessionRid,
        String actorId) {
        requireSessionRuntime();
        SessionBinding binding =
            bindings.get(new BindingKey(sessionRid, actorId));
        if (binding == null) {
            throw new IllegalStateException(
                "STREAM session is not bound to actor: " + actorId);
        }
        return binding;
    }

    private ZLinkJavaRawSpotNode rawSpotNode() {
        return (ZLinkJavaRawSpotNode) meshNode.spotNode();
    }

    @Override public long allocateBoundSessionIngressSequence() {
        return allocateBoundSessionSequence();
    }

    @Override public long boundActorBindingGeneration(
        RoutingId sessionRid,
        String actorId) {
        return requireBinding(sessionRid, actorId).generation();
    }

    @Override public CompletionStage<Void> relocateBoundActor(
        RoutingId sessionRid,
        String actorId,
        long bindingGeneration,
        ZLinkBackendActorRef targetActor,
        Duration timeout) {
        if (targetActor == null
            || !actorId.equals(targetActor.actorId())
            || bindingGeneration <= 0) {
            return CompletableFuture.failedFuture(
                new IllegalArgumentException(
                    "exact bound Session relocation fence is required"));
        }
        BindingKey key = new BindingKey(sessionRid, actorId);
        final SessionBinding source;
        try {
            source = requireBinding(sessionRid, actorId);
        } catch (RuntimeException failure) {
            return CompletableFuture.failedFuture(failure);
        }
        if (source.generation() != bindingGeneration) {
            return CompletableFuture.failedFuture(
                new IllegalStateException(
                    "bound Session relocation binding generation is stale"));
        }
        SessionBinding target = new SessionBinding(
            targetActor,
            bindingGeneration);
        CompletionStage<Void> unbound = unbindBinding(
            sessionRid,
            source,
            timeout);
        CompletionStage<Void> prepared = unbound.thenCompose(ignored ->
            bindExactSessionRoute(
                sessionRid,
                targetActor,
                bindingGeneration,
                timeout));
        return prepared.handle((ignored, failure) -> {
            if (failure != null) {
                return bindExactSessionRoute(
                        sessionRid,
                        source.actor(),
                        bindingGeneration,
                        timeout)
                    .handle((restored, restoreFailure) -> {
                        Throwable cause = unwrapFailure(failure);
                        if (restoreFailure != null) {
                            cause.addSuppressed(unwrapFailure(restoreFailure));
                        }
                        return CompletableFuture.<Void>failedFuture(cause);
                    }).thenCompose(stage -> stage);
            }
            return CompletableFuture.completedFuture(null);
        }).thenCompose(stage -> stage)
            .thenRun(() -> {
                if (!bindings.replace(key, source, target)) {
                    throw new IllegalStateException(
                        "bound Session route changed during relocation");
                }
            });
    }

    private CompletionStage<Void> bindExactSessionRoute(
        RoutingId sessionRid,
        ZLinkBackendActorRef actor,
        long bindingGeneration,
        Duration timeout) {
        return boundSessionLifecycle == null
            ? rawSpotNode().bindStreamSession(
                sessionRid,
                actor,
                bindingGeneration,
                this,
                timeout)
            : boundSessionLifecycle.bind(
                sessionRid,
                actor,
                bindingGeneration,
                timeout);
    }

    private long allocateBoundSessionSequence() {
        long sequence = nextBoundSessionSequence.getAndUpdate(
            current -> current >= Long.MAX_VALUE - 1
                ? Long.MAX_VALUE
                : current + 1);
        if (sequence <= 0 || sequence == Long.MAX_VALUE) {
            throw new IllegalStateException(
                "bound Session ingress sequence is exhausted");
        }
        return sequence;
    }

    private long allocateBoundSessionRequestSequence() {
        return nextBoundSessionRequestSequence.getAndUpdate(
            current -> current == Long.MAX_VALUE ? 1 : current + 1);
    }

    private CompletionStage<Void> unbindBinding(
        RoutingId sessionRid,
        SessionBinding binding,
        Duration timeout) {
        return boundSessionLifecycle == null
            ? rawSpotNode().unbindStreamSession(
                sessionRid,
                binding.actor(),
                binding.generation(),
                this,
                timeout)
            : boundSessionLifecycle.unbind(
                sessionRid,
                binding.actor(),
                binding.generation(),
                timeout);
    }

    private static boolean isStaleBinding(Throwable failure) {
        Throwable current = unwrapFailure(failure);
        return current instanceof IllegalStateException
            && "STREAM session binding is stale".equals(current.getMessage());
    }

    private static boolean isRemoteRouteUnavailable(Throwable failure) {
        Throwable current = unwrapFailure(failure);
        if (current instanceof ZlinkRequestException request) {
            return request.getResult() == RequestResult.NOT_CONNECTED
                || request.getResult() == RequestResult.NOT_FOUND
                || request.getResult() == RequestResult.TERMINATED;
        }
        if (current instanceof ZlinkSubmitException submit) {
            return submit.getResult() == SubmitResult.NOT_CONNECTED
                || submit.getResult() == SubmitResult.NOT_FOUND
                || submit.getResult() == SubmitResult.TERMINATED
                || submit.getResult() == SubmitResult.NOT_ADMITTED;
        }
        return false;
    }

    private static Throwable unwrapFailure(Throwable failure) {
        Throwable current = failure;
        while (current instanceof CompletionException
            || current instanceof ExecutionException) {
            current = current.getCause();
        }
        return current;
    }

    private void requireSessionRuntime() {
        if (meshNode == null || !sessionServiceStarted) {
            throw new IllegalStateException(
                "STREAM actor dispatch is not enabled for this stream node");
        }
    }

    private record BindingKey(RoutingId sessionRid, String actorId) {
    }

    private record SessionBinding(
        ZLinkBackendActorRef actor,
        long generation) {
    }

    @FunctionalInterface
    interface BoundSessionSink {
        boolean send(
            RoutingId sessionRid,
            List<Message> parts,
            SendFlags flags);

        default CompletionStage<Void> sendAsync(
            RoutingId sessionRid,
            List<Message> parts,
            Duration timeout) {
            try {
                return send(sessionRid, parts, SendFlags.DONT_WAIT)
                    ? CompletableFuture.completedFuture(null)
                    : CompletableFuture.failedFuture(
                        new ZlinkSubmitException(SubmitResult.BACKPRESSURED));
            } catch (RuntimeException failure) {
                return CompletableFuture.failedFuture(failure);
            }
        }
    }

    interface BoundSessionLifecycle {
        CompletionStage<Void> bind(
            RoutingId sessionRid,
            ZLinkBackendActorRef actor,
            long bindingGeneration,
            Duration timeout);

        CompletionStage<Void> unbind(
            RoutingId sessionRid,
            ZLinkBackendActorRef actor,
            long bindingGeneration,
            Duration timeout);
    }
}
