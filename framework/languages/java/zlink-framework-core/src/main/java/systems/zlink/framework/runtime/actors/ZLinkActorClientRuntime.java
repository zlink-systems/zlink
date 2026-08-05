package systems.zlink.framework.runtime.actors;

import systems.zlink.framework.runtime.internal.calls.ZLinkOneWayCalls;

import java.time.Duration;
import java.util.EnumSet;
import java.util.List;
import java.util.Map;
import java.util.Optional;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionException;
import java.util.concurrent.CompletionStage;
import systems.zlink.contracts.errors.ZlinkRequestException;
import systems.zlink.contracts.errors.ZlinkSubmitException;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.contracts.sockets.SendFlags;
import systems.zlink.framework.ZLinkMessageSerializer;
import systems.zlink.framework.actors.ZLinkActorClient;
import systems.zlink.framework.actors.ZLinkActorRequestCall;
import systems.zlink.framework.actors.ZLinkActorSendCall;
import systems.zlink.framework.errors.ZLinkConfigurationException;
import systems.zlink.framework.errors.ZLinkFrameworkErrorKind;
import systems.zlink.framework.errors.ZLinkFrameworkException;
import systems.zlink.framework.runtime.locations.ZLinkStoreLocationResolvers.ActorRoute;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendActorRef;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendAdmissionKey;
import systems.zlink.framework.runtime.internal.backend.ZLinkInternalSpotNode;
import systems.zlink.framework.runtime.locations.ZLinkStoreLocationResolvers;
import systems.zlink.framework.runtime.messaging.ZLinkMessagePayloads;
import systems.zlink.framework.runtime.messaging.ZLinkPacketNames;

import systems.zlink.framework.runtime.streams.ZLinkStreamHeader;
import systems.zlink.framework.runtime.streams.ZLinkStreamHeaderCodec;
import systems.zlink.framework.runtime.streams.ZLinkStreamHeaderFlag;
import systems.zlink.framework.runtime.streams.ZLinkStreamFrameCodec;
import systems.zlink.framework.streams.ZLinkStreamCodec;
import systems.zlink.framework.streams.ZLinkStreamMessageKind;
import systems.zlink.framework.runtime.internal.streams.ZLinkStreamErrorPayload;

public final class ZLinkActorClientRuntime implements ZLinkActorClient {
    private static final Duration FALLBACK_ROUTE_RETRY_TIMEOUT = Duration.ofSeconds(5);
    private static final int MAX_RUNTIME_READY_WAITERS = 4096;

    private final java.util.function.Supplier<ZLinkInternalSpotNode> spotNode;
    private final ZLinkStoreLocationResolvers locations;
    private final ZLinkMessageSerializer serializer;
    private final Duration defaultTimeout;
    private final ZLinkOneWayCalls oneWayCalls;
    private final CompletionStage<Void> runtimeReady;
    private final java.util.concurrent.atomic.AtomicInteger
        runtimeReadyWaiters = new java.util.concurrent.atomic.AtomicInteger();

    public ZLinkActorClientRuntime(
        java.util.function.Supplier<ZLinkInternalSpotNode> spotNode,
        ZLinkStoreLocationResolvers locations,
        ZLinkMessageSerializer serializer,
        Duration defaultTimeout) {
        this(
            spotNode,
            locations,
            serializer,
            defaultTimeout,
            (ignoredBackend, ignoredKey) -> (ignoredSubmission, ignoredCleanup) ->
                CompletableFuture.failedFuture(new IllegalStateException(
                    "one-way admission factory is required")),
            CompletableFuture.completedFuture(null));
    }

    public ZLinkActorClientRuntime(
        java.util.function.Supplier<ZLinkInternalSpotNode> spotNode,
        ZLinkStoreLocationResolvers locations,
        ZLinkMessageSerializer serializer,
        Duration defaultTimeout,
        java.util.function.BiFunction<
            systems.zlink.framework.runtime.internal.backend.ZLinkBackendObject,
            ZLinkBackendAdmissionKey,
            java.util.function.BiFunction<
                java.util.function.Supplier<Boolean>,
                Runnable,
                CompletionStage<Void>>> admission) {
        this(
            spotNode,
            locations,
            serializer,
            defaultTimeout,
            admission,
            CompletableFuture.completedFuture(null));
    }

    public ZLinkActorClientRuntime(
        java.util.function.Supplier<ZLinkInternalSpotNode> spotNode,
        ZLinkStoreLocationResolvers locations,
        ZLinkMessageSerializer serializer,
        Duration defaultTimeout,
        java.util.function.BiFunction<
            systems.zlink.framework.runtime.internal.backend.ZLinkBackendObject,
            ZLinkBackendAdmissionKey,
            java.util.function.BiFunction<
                java.util.function.Supplier<Boolean>,
                Runnable,
                CompletionStage<Void>>> admission,
        CompletionStage<Void> runtimeReady) {
        this.spotNode = java.util.Objects.requireNonNull(spotNode, "spotNode");
        this.locations = java.util.Objects.requireNonNull(locations, "locations");
        this.serializer = java.util.Objects.requireNonNull(serializer, "serializer");
        this.defaultTimeout = defaultTimeout == null ? Duration.ZERO : defaultTimeout;
        this.oneWayCalls = new ZLinkOneWayCalls(admission);
        this.runtimeReady = java.util.Objects.requireNonNull(
            runtimeReady,
            "runtimeReady");
    }

    @Override
    public ZLinkActorSendCall sendToActor(String actorId, Object message) {
        return new SendCall(actorId, message);
    }

    @Override
    public ZLinkActorRequestCall requestToActor(String actorId, Object request) {
        return new RequestCall(actorId, request);
    }

    private <TReply> CompletionStage<TReply> requestAsync(
        String actorId,
        String packetName,
        Object request,
        Map<String, String> metadata,
        Duration timeout,
        Class<TReply> replyType) {
        return resolveActorAddress(actorId, timeout)
            .thenCompose(actor -> submitRequestWithRouteRetry(
                actor, packetName, request, metadata, timeout, replyType))
            .whenComplete((ignored, error) -> {
                if (error != null && isStaleActorError(error)) {
                    locations.invalidateActorRoute(actorId);
                }
            });
    }

    private <TReply> CompletionStage<TReply> submitRequestWithRouteRetry(
        ZLinkBackendActorRef actor,
        String packetName,
        Object request,
        Map<String, String> metadata,
        Duration timeout,
        Class<TReply> replyType) {
        Duration effectiveTimeout = timeout == null ? defaultTimeout : timeout;
        return ZLinkActorRetryScheduler.retryRouteUntil(
                routeRetryTimeout(effectiveTimeout),
                () -> submitRequest(
                    actor, packetName, request, metadata, timeout, replyType),
                ZLinkActorClientRuntime::isRouteNotConnected)
            .exceptionallyCompose(error -> failed(unwrap(error)));
    }

    private CompletionStage<ZLinkBackendActorRef> resolveActorAddress(
        String actorId,
        Duration readinessTimeout) {
        return awaitRuntimeReady(readinessTimeout)
            .thenCompose(ignored -> locations.resolveActor(actorId))
            .thenApply(row -> {
                if (row == null || row.actorRef() == null) {
                    throw new ZLinkFrameworkException(
                        ZLinkFrameworkErrorKind.NOT_FOUND,
                        "Actor route '" + actorId + "' was not found.");
                }
                return rememberAuthority(row);
            });
    }

    private ZLinkBackendActorRef rememberAuthority(
        ActorRoute row) {
        ZLinkBackendActorRef actor = toBackendActorRef(row);
        spotNode.get().rememberActorAuthority(
            actor,
            row.authorityOwnerGeneration(),
            row.ownerLeaseGeneration());
        return actor;
    }

    private CompletionStage<Void>
        submitSendResult(
        String actorId,
        String packetName,
        Object message,
        Map<String, String> metadata) {
        return resolveActorAddress(actorId, defaultTimeout).thenCompose(actor -> {
            List<Message> parts = createPacketParts(
                ZLinkStreamMessageKind.SEND,
                Optional.empty(),
                packetName,
                message,
                metadata);
            ZLinkInternalSpotNode node = spotNode.get();
            return oneWayCalls.submitOneWay(
                    node,
                    ZLinkBackendAdmissionKey.actor(
                        actor.nodeRid(), actor.actorId(), actor.generation()),
                    () -> node.sendToActor(
                        actor, parts, SendFlags.DONT_WAIT),
                    () -> closeAll(parts))
                .whenComplete((ignored, error) -> {
                    if (error != null && isStaleActorError(error)) {
                        locations.invalidateActorRoute(actorId);
                    }
                });
        });
    }

    private CompletionStage<Void> awaitRuntimeReady(Duration timeout) {
        CompletableFuture<Void> readyFuture = runtimeReady.toCompletableFuture();
        if (readyFuture.isDone()) {
            return runtimeReady;
        }
        int waiters = runtimeReadyWaiters.incrementAndGet();
        if (waiters > MAX_RUNTIME_READY_WAITERS) {
            runtimeReadyWaiters.decrementAndGet();
            return failed(new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.CAPACITY_EXCEEDED,
                "framework startup admission capacity is exhausted"));
        }
        CompletableFuture<Void> result = new CompletableFuture<>();
        java.util.concurrent.atomic.AtomicBoolean released =
            new java.util.concurrent.atomic.AtomicBoolean();
        Runnable release = () -> {
            if (released.compareAndSet(false, true)) {
                runtimeReadyWaiters.decrementAndGet();
            }
        };
        runtimeReady.whenComplete((ignored, failure) -> {
            release.run();
            if (failure == null) {
                result.complete(null);
            } else {
                result.completeExceptionally(unwrap(failure));
            }
        });
        Duration effectiveTimeout = timeout == null ? defaultTimeout : timeout;
        if (effectiveTimeout != null
            && !effectiveTimeout.isZero()
            && !effectiveTimeout.isNegative()) {
            CompletableFuture.delayedExecutor(
                effectiveTimeout.toNanos(),
                java.util.concurrent.TimeUnit.NANOSECONDS).execute(() -> {
                    release.run();
                    result.completeExceptionally(new ZLinkFrameworkException(
                        ZLinkFrameworkErrorKind.DEADLINE_EXCEEDED,
                        "framework runtime did not become ready before the actor operation deadline"));
                });
        }
        return result;
    }

    private <TReply> CompletionStage<TReply> submitRequest(
        ZLinkBackendActorRef actor,
        String packetName,
        Object request,
        Map<String, String> metadata,
        Duration timeout,
        Class<TReply> replyType) {
        List<Message> parts = createPacketParts(
            ZLinkStreamMessageKind.REQUEST,
            Optional.of(1L),
            packetName,
            request,
            metadata);
        CompletionStage<List<Message>> replyStage;
        try {
            replyStage = spotNode.get().requestToActor(
                actor,
                parts,
                SendFlags.NONE,
                timeout == null ? defaultTimeout : timeout);
        } catch (RuntimeException error) {
            closeAll(parts);
            return failed(mapBackendException(error, "Actor request"));
        }
        closeAll(parts);
        return replyStage.handle((replyParts, error) -> {
            if (error != null) {
                throw new CompletionException(mapBackendException(unwrap(error), "Actor request"));
            }
            try {
                return decodeReply(replyParts, replyType);
            } finally {
                closeAll(replyParts);
            }
        });
    }

    private List<Message> createPacketParts(
        ZLinkStreamMessageKind kind,
        Optional<Long> requestSeq,
        String packetName,
        Object message,
        Map<String, String> metadata) {
        ZLinkStreamHeader header = new ZLinkStreamHeader(
            kind,
            ZLinkStreamCodec.JSON,
            EnumSet.noneOf(ZLinkStreamHeaderFlag.class),
            requestSeq,
            packetName,
            Map.copyOf(metadata));
        Message headerPart = Message.from(ZLinkStreamHeaderCodec.encode(header));
        Message payloadPart = ZLinkMessagePayloads.message(
            systems.zlink.framework.messaging.ZLinkMessage.of(message),
            serializer);
        return List.of(headerPart, payloadPart);
    }

    private <TReply> TReply decodeReply(List<Message> reply, Class<TReply> replyType) {
        if (reply == null || reply.isEmpty()) {
            throw new ZLinkConfigurationException("Actor request reply is empty.");
        }
        if (reply.size() == 1) {
            byte[] frame = reply.get(0).toByteArray();
            Optional<ZLinkStreamFrameCodec.DecodedFrame> decoded = ZLinkStreamFrameCodec.tryDecode(frame);
            if (decoded.isPresent()) {
                ZLinkStreamFrameCodec.DecodedFrame decodedFrame = decoded.get();
                Message payload = Message.from(decodedFrame.body());
                try {
                    return decodePayload(
                        ZLinkStreamHeaderCodec.decodeOrPlain(decodedFrame.header()),
                        payload,
                        replyType);
                } finally {
                    payload.close();
                }
            }
        }
        if (reply.size() < 2) {
            throw new ZLinkConfigurationException("Actor request reply payload is missing.");
        }
        ZLinkStreamHeader header = ZLinkStreamHeaderCodec.decodeOrPlain(reply.get(0).toByteArray());
        return decodePayload(header, reply.get(1), replyType);
    }

    private <TReply> TReply decodePayload(
        ZLinkStreamHeader header,
        Message payload,
        Class<TReply> replyType) {
        if (header.kind() == ZLinkStreamMessageKind.ERROR) {
            try {
                ZLinkStreamErrorPayload.Decoded error =
                    ZLinkStreamErrorPayload.decode(payload.toByteArray());
                ZLinkFrameworkErrorKind kind = error.frameworkKind() == null
                    ? ZLinkFrameworkErrorKind.INTERNAL_FAILURE
                    : error.frameworkKind();
                throw new ZLinkFrameworkException(kind, error.message());
            } catch (IllegalArgumentException invalidPayload) {
                throw new ZLinkFrameworkException(
                    ZLinkFrameworkErrorKind.PROTOCOL_ERROR,
                    "Actor request error reply payload is invalid.",
                    invalidPayload);
            }
        }
        return ZLinkMessagePayloads.deserialize(serializer, payload, replyType);
    }

    private RuntimeException mapBackendException(Throwable error, String operationName) {
        Throwable unwrapped = unwrap(error);
        if (unwrapped instanceof ZlinkRequestException request) {
            return switch (request.getResult()) {
                case NOT_CONNECTED -> new ZLinkFrameworkException(
                    ZLinkFrameworkErrorKind.UNAVAILABLE,
                    operationName + " failed because the target route is not connected.",
                    request);
                case NOT_FOUND -> new ZLinkFrameworkException(
                    ZLinkFrameworkErrorKind.NOT_FOUND,
                    operationName + " failed because the actor route was not found.",
                    request);
                case CONFLICT -> new ZLinkFrameworkException(
                    ZLinkFrameworkErrorKind.UNAVAILABLE,
                    operationName + " failed because the actor location is stale.",
                    request);
                default -> request;
            };
        }
        if (unwrapped instanceof ZlinkSubmitException submit) {
            return switch (submit.getResult()) {
                case NOT_CONNECTED -> new ZLinkFrameworkException(
                    ZLinkFrameworkErrorKind.UNAVAILABLE,
                    operationName + " failed because the target route is not connected.",
                    submit);
                case NOT_FOUND -> new ZLinkFrameworkException(
                    ZLinkFrameworkErrorKind.NOT_FOUND,
                    operationName + " failed because the actor route was not found.",
                    submit);
                default -> submit;
            };
        }
        String text = unwrapped.getMessage() == null ? "" : unwrapped.getMessage();
        if (text.contains("NOT_CONNECTED")) {
            return new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.UNAVAILABLE,
                operationName + " failed because the target route is not connected.",
                unwrapped);
        }
        if (text.contains("NOT_FOUND")) {
            return new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.NOT_FOUND,
                operationName + " failed because the actor route was not found.",
                unwrapped);
        }
        if (text.contains("CONFLICT")) {
            return new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.UNAVAILABLE,
                operationName + " failed because the actor location is stale.",
                unwrapped);
        }
        if (unwrapped instanceof RuntimeException runtimeException) {
            return runtimeException;
        }
        return new ZLinkFrameworkException(
            ZLinkFrameworkErrorKind.INTERNAL_FAILURE,
            operationName + " failed.",
            unwrapped);
    }

    private static ZLinkBackendActorRef toBackendActorRef(ActorRoute row) {
        return new ZLinkBackendActorRef(
            row.actorRef().nodeRid(),
            row.actorRef().actorId(),
            row.actorRef().objectGeneration());
    }

    private static boolean isStaleActorError(Throwable error) {
        Throwable unwrapped = unwrap(error);
        return unwrapped instanceof ZLinkFrameworkException frameworkError
            && (frameworkError.kind() == ZLinkFrameworkErrorKind.NOT_FOUND
                || frameworkError.kind() == ZLinkFrameworkErrorKind.UNAVAILABLE);
    }

    private static boolean isRouteNotConnected(Throwable error) {
        Throwable unwrapped = unwrap(error);
        if (!(unwrapped instanceof ZLinkFrameworkException frameworkError)
            || frameworkError.kind() != ZLinkFrameworkErrorKind.UNAVAILABLE) {
            return false;
        }
        Throwable cause = frameworkError.getCause();
        if (cause instanceof ZlinkRequestException request) {
            return request.getResult() == systems.zlink.contracts.sockets.RequestResult.NOT_CONNECTED;
        }
        if (cause instanceof ZlinkSubmitException submit) {
            return submit.getResult() == systems.zlink.contracts.sockets.SubmitResult.NOT_CONNECTED;
        }
        return false;
    }

    private static Duration routeRetryTimeout(Duration timeout) {
        return timeout == null || timeout.isZero() || timeout.isNegative()
            ? FALLBACK_ROUTE_RETRY_TIMEOUT
            : timeout;
    }

    private static Throwable unwrap(Throwable error) {
        Throwable current = error;
        while (current instanceof CompletionException && current.getCause() != null) {
            current = current.getCause();
        }
        return current;
    }

    private static <T> CompletableFuture<T> failed(Throwable error) {
        CompletableFuture<T> future = new CompletableFuture<>();
        future.completeExceptionally(error);
        return future;
    }

    private static void closeAll(List<Message> parts) {
        if (parts == null) {
            return;
        }
        for (Message part : parts) {
            part.close();
        }
    }

    private final class SendCall implements ZLinkActorSendCall {
        private final java.util.concurrent.atomic.AtomicBoolean submitGate =
            new java.util.concurrent.atomic.AtomicBoolean();
        private final String actorId;
        private final Object message;
        private String packetName;
        private final Map<String, String> metadata = new java.util.LinkedHashMap<>();

        SendCall(String actorId, Object message) {
            this.actorId = requireActorId(actorId);
            this.message = message;
            this.packetName = ZLinkPacketNames.resolve(message);
        }

        public ZLinkActorSendCall packetName(String packetName) {
            this.packetName = packetName;
            return this;
        }

        @Override
        public ZLinkActorSendCall metadata(String key, String value) {
            metadata.put(
                java.util.Objects.requireNonNull(key, "key"),
                java.util.Objects.requireNonNull(value, "value"));
            return this;
        }

        @Override
        public CompletionStage<Void> submit() {
            CompletionStage<Void> duplicate =
                ZLinkOneWayCalls.beginOneWay(submitGate);
            if (duplicate != null) {
                return duplicate;
            }
            return submitSendResult(actorId, packetName, message, metadata);
        }
    }

    private final class RequestCall implements ZLinkActorRequestCall {
        private final String actorId;
        private final Object request;
        private String packetName;
        private final Map<String, String> metadata = new java.util.LinkedHashMap<>();
        private Duration timeout;

        RequestCall(String actorId, Object request) {
            this.actorId = requireActorId(actorId);
            this.request = request;
            this.packetName = ZLinkPacketNames.resolve(request);
        }

        public ZLinkActorRequestCall packetName(String packetName) {
            this.packetName = packetName;
            return this;
        }

        @Override
        public ZLinkActorRequestCall metadata(String key, String value) {
            metadata.put(
                java.util.Objects.requireNonNull(key, "key"),
                java.util.Objects.requireNonNull(value, "value"));
            return this;
        }

        @Override
        public ZLinkActorRequestCall timeout(Duration timeout) {
            this.timeout = timeout;
            return this;
        }

        @Override
        public <TReply> CompletionStage<TReply> submit(Class<TReply> replyType) {
            systems.zlink.framework.runtime.internal.handlers
                .ZLinkSuspendInvocationContext.rejectSameActorWait(
                    actorId);
            return systems.zlink.framework.execution.ZLinkAsyncSerialQueue.manageCurrent(
                requestAsync(
                    actorId, packetName, request, metadata, timeout, replyType));
        }

        @Override
        public <TReply> CompletionStage<TReply> yield(Class<TReply> replyType) {
            systems.zlink.framework.runtime.internal.handlers
                .ZLinkSuspendInvocationContext.requireYieldAllowed("Actor request");
            return systems.zlink.framework.execution.ZLinkAsyncSerialQueue
                .yieldCurrent(submit(replyType));
        }
    }

    private static String requireActorId(String actorId) {
        java.util.Objects.requireNonNull(actorId, "actorId");
        if (actorId.isBlank() || actorId.indexOf('\0') >= 0) {
            throw new IllegalArgumentException("actorId is required");
        }
        return actorId;
    }
}
