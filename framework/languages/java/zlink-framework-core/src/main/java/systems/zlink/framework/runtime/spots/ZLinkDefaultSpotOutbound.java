package systems.zlink.framework.runtime.spots;

import systems.zlink.framework.runtime.internal.calls.ZLinkOneWayCalls;

import java.time.Duration;
import java.util.Map;
import java.util.Optional;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.ZLinkMessageSerializer;
import systems.zlink.framework.channels.ZLinkPublishCall;
import systems.zlink.framework.channels.ZLinkSendCall;
import systems.zlink.framework.channels.ZLinkRequestCall;
import systems.zlink.framework.errors.ZLinkConfigurationException;
import systems.zlink.framework.errors.ZLinkFrameworkErrorKind;
import systems.zlink.framework.errors.ZLinkFrameworkException;
import systems.zlink.framework.spots.SpotHandle;
import systems.zlink.framework.runtime.messaging.ZLinkApplicationMetadata;

import systems.zlink.framework.runtime.internal.spots.SpotTransportAddress;
import systems.zlink.framework.runtime.internal.spots.SpotTransportAddressResolver;
import systems.zlink.framework.runtime.internal.spots.ZLinkInstanceSpotCallRuntime;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.function.Supplier;
import java.util.function.Function;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendSpot;
import systems.zlink.framework.runtime.channels.ZLinkChannelRuntime;
import systems.zlink.framework.runtime.messaging.ZLinkPayloadEncoding;
import systems.zlink.framework.spots.ZLinkSpotOutbound;

final class DefaultSpotOutbound implements ZLinkSpotOutbound {
    private final ZLinkBackendSpot backendSpot;
    private final String meshName;
    private final String publisherChannelName;
    private final ZLinkMessageSerializer serializer;
    private final Function<Class<?>, String> contentTypeResolver;
    private final ZLinkSpotRouteMessages routeMessages;
    private final ZLinkSpotRoutedOutbound routed;
    private final ZLinkSpotDirectOutbound direct;
    private final ZLinkSpotPublisherRuntime publishers;
    private final ZLinkChannelRuntime channels;
    private final boolean routeMeshEnabled;
    private final Duration defaultRequestTimeout;
    private final Supplier<SpotTransportAddressResolver> spotAddressResolver;
    private final ZLinkInstanceSpotCallRuntime instanceSpots;

    DefaultSpotOutbound(
        ZLinkBackendSpot backendSpot,
        String meshName,
        String publisherChannelName,
        ZLinkMessageSerializer serializer,
        Function<Class<?>, String> contentTypeResolver,
        ZLinkSpotRoutedOutbound routed,
        ZLinkSpotDirectOutbound direct,
        ZLinkSpotPublisherRuntime publishers,
        ZLinkChannelRuntime channels,
        boolean routeMeshEnabled,
        Duration defaultRequestTimeout,
        Supplier<SpotTransportAddressResolver> spotAddressResolver,
        ZLinkInstanceSpotCallRuntime instanceSpots) {
        this.backendSpot = backendSpot;
        this.meshName = meshName;
        this.publisherChannelName = publisherChannelName;
        this.serializer = serializer;
        this.contentTypeResolver = contentTypeResolver;
        this.routeMessages = new ZLinkSpotRouteMessages(serializer);
        this.routed = routed;
        this.direct = direct;
        this.publishers = publishers;
        this.channels = channels;
        this.routeMeshEnabled = routeMeshEnabled;
        this.defaultRequestTimeout = defaultRequestTimeout;
        this.spotAddressResolver = spotAddressResolver;
        this.instanceSpots = instanceSpots;
    }

    @Override
    public systems.zlink.framework.spots.ZLinkSpotSendCall sendToSpot(
        String spotId,
        Object message) {
        rejectAfterRelocationReady("sendToSpot");
        ZLinkPayloadEncoding.EncodedPayload encoded =
            ZLinkPayloadEncoding.encode(
                serializer,
                message,
                contentTypeResolver.apply(message == null ? null : message.getClass()));
        return new DeferredSpotSendCall(
            spotId,
            encoded.payload(),
            Optional.of(encoded.packetName()),
            encoded.contentType());
    }

    @Override
    public systems.zlink.framework.spots.ZLinkSpotRequestCall requestToSpot(
        String spotId,
        Object request) {
        rejectAfterRelocationReady("requestToSpot");
        ZLinkPayloadEncoding.EncodedPayload encoded =
            ZLinkPayloadEncoding.encode(
                serializer,
                request,
                contentTypeResolver.apply(request == null ? null : request.getClass()));
        return new DeferredSpotRequestCall(
            spotId,
            encoded.payload(),
            Optional.of(encoded.packetName()),
            defaultRequestTimeout,
            encoded.contentType());
    }

    @Override
    public ZLinkPublishCall publish(
        String channelName,
        String topic,
        Object message) {
        rejectAfterRelocationReady("publish");
        ZLinkPayloadEncoding.EncodedPayload encoded =
            ZLinkPayloadEncoding.encode(
                serializer,
                message,
                contentTypeResolver.apply(message == null ? null : message.getClass()));
        if (publisherChannelName != null && publishers.contains(publisherChannelName)) {
            return publishers.call(
                publisherChannelName,
                channelName,
                topic,
                encoded.payload(),
                Optional.of(encoded.packetName()),
                encoded.contentType());
        }
        return direct.publish(
            backendSpot,
            channelName,
            topic,
            encoded.payload(),
            Optional.of(encoded.packetName()),
            encoded.contentType());
    }

    @Override
    public ZLinkSendCall sendToChannel(String channelName, Object message) {
        rejectAfterRelocationReady("sendToChannel");
        requireChannels(channelName);
        return channels.sendToChannel(channelName, message);
    }

    @Override
    public ZLinkRequestCall requestToChannel(String channelName, Object request) {
        rejectAfterRelocationReady("requestToChannel");
        requireChannels(channelName);
        return channels.requestToChannel(channelName, request);
    }

    private void requireChannels(String channelName) {
        if (channels == null || meshName == null) {
            throw new ZLinkConfigurationException(
                "channel client is not configured: " + channelName);
        }
    }

    private static void requireRoutingId(RoutingId routingId) {
        if (routingId == null || routingId.size() == 0) {
            throw new ZLinkConfigurationException("routing id is required");
        }
    }

    private CompletionStage<SpotTransportAddress> resolve(String spotId) {
        SpotTransportAddressResolver resolver;
        try {
            resolver = spotAddressResolver == null ? null : spotAddressResolver.get();
        } catch (RuntimeException ignored) {
            resolver = null;
        }
        if (resolver == null) {
            return CompletableFuture.failedFuture(new ZLinkConfigurationException(
                "SpotHandle resolver is not configured"));
        }
        return resolver.resolve(spotId).thenCompose(value -> value
            .map(CompletableFuture::completedFuture)
            .orElseGet(() -> CompletableFuture.failedFuture(
                new ZLinkFrameworkException(
                    ZLinkFrameworkErrorKind.NOT_FOUND,
                    "SpotHandle route is stale or unavailable"))));
    }

    private void invalidate(String spotId) {
        SpotTransportAddressResolver resolver;
        try {
            resolver = spotAddressResolver == null ? null : spotAddressResolver.get();
        } catch (RuntimeException ignored) {
            resolver = null;
        }
        if (resolver != null) {
            resolver.invalidate(spotId);
        }
    }

    private final class DeferredSpotSendCall
        implements systems.zlink.framework.spots.ZLinkSpotSendCall {
        private final java.util.concurrent.atomic.AtomicBoolean submitGate =
            new java.util.concurrent.atomic.AtomicBoolean();
        private final String target;
        private final systems.zlink.contracts.messaging.Message payload;
        private final Optional<String> packetName;
        private final String contentType;
        private final ZLinkApplicationMetadata metadata;
        private final boolean instanceIntent;
        private final String stableType;
        private final String selectedMesh;

        private DeferredSpotSendCall(String target, systems.zlink.contracts.messaging.Message payload,
            Optional<String> packetName) {
            this(
                target,
                payload,
                packetName,
                null,
                ZLinkApplicationMetadata.empty(),
                false,
                null,
                null);
        }

        private DeferredSpotSendCall(
            String target,
            systems.zlink.contracts.messaging.Message payload,
            Optional<String> packetName,
            String contentType) {
            this(
                target,
                payload,
                packetName,
                contentType,
                ZLinkApplicationMetadata.empty(),
                false,
                null,
                null);
        }

        private DeferredSpotSendCall(
            String target,
            systems.zlink.contracts.messaging.Message payload,
            Optional<String> packetName,
            String contentType,
            ZLinkApplicationMetadata metadata,
            boolean instanceIntent,
            String stableType,
            String selectedMesh) {
            this.target = java.util.Objects.requireNonNull(target, "target");
            this.payload = payload;
            this.packetName = packetName;
            this.contentType = contentType;
            this.metadata = metadata;
            this.instanceIntent = instanceIntent;
            this.stableType = stableType;
            this.selectedMesh = selectedMesh;
        }

        @Override
        public systems.zlink.framework.spots.ZLinkSpotSendCall instanceSpot() {
            return withInstanceType(null);
        }

        @Override
        public systems.zlink.framework.spots.ZLinkSpotSendCall instanceSpot(
            String value) {
            return withInstanceType(requireStableType(value));
        }

        private DeferredSpotSendCall withInstanceType(String value) {
            if (instanceIntent) {
                throw new IllegalStateException("instanceSpot was already set");
            }
            return new DeferredSpotSendCall(
                target, payload, packetName, contentType, metadata, true, value, selectedMesh);
        }

        @Override
        public systems.zlink.framework.spots.ZLinkSpotSendCall inMesh(
            String value) {
            if (selectedMesh != null) {
                throw new IllegalStateException("inMesh was already set");
            }
            return new DeferredSpotSendCall(
                target, payload, packetName, contentType, metadata, instanceIntent,
                stableType, requireStableType(value));
        }

        @Override
        public systems.zlink.framework.spots.ZLinkSpotSendCall metadata(
            String key,
            String value) {
            return new DeferredSpotSendCall(
                target, payload, packetName, contentType, metadata.with(key, value),
                instanceIntent, stableType, selectedMesh);
        }

        @Override
        public systems.zlink.framework.spots.ZLinkSpotSendCall metadata(
            Map<String, String> values) {
            return new DeferredSpotSendCall(
                target, payload, packetName, contentType, metadata.withAll(values),
                instanceIntent, stableType, selectedMesh);
        }

        @Override public CompletionStage<Void> submit() {
            rejectAfterRelocationReady("Spot send submit");
            CompletionStage<Void> duplicate =
                ZLinkOneWayCalls.beginOneWay(submitGate);
            if (duplicate != null) {
                return duplicate;
            }
            CompletionStage<Void> stage = resolve(target).handle((address, failure) -> {
                if (failure == null) {
                    return sendExistingOrActivate(address);
                }
                if (!instanceIntent || instanceSpots == null) {
                    return CompletableFuture.<Void>failedFuture(unwrap(failure));
                }
                return instanceSpots.send(
                    target, stableType, selectedMesh, copyPayload(), packetName,
                    contentType, metadata.values());
            }).thenCompose(java.util.function.Function.identity());
            return stage.whenComplete((ignored, failure) -> payload.close());
        }

        private CompletionStage<Void> sendExisting(SpotTransportAddress address) {
                backendSpot.rememberSpotAuthority(
                    address.targetNodeRid(),
                    address.spotId(),
                    address.spotGeneration(),
                    address.authorityOwnerGeneration(),
                    address.ownerLeaseGeneration());
                systems.zlink.contracts.messaging.Message transportPayload = copyPayload();
                ZLinkSendCall call = routeMeshEnabled
                    ? routed.send(address.routerChannelId(), address.targetNodeRid(), address.spotId(),
                        address.spotGeneration(), transportPayload, packetName, contentType)
                    : direct.send(backendSpot, address.targetNodeRid(), address.spotId(),
                        address.spotGeneration(), transportPayload, packetName, contentType);
                call = call.metadata(metadata.values());
                try {
                    return call.submit();
                } catch (RuntimeException failure) {
                    transportPayload.close();
                    throw failure;
                }
        }

        private systems.zlink.contracts.messaging.Message copyPayload() {
            return systems.zlink.contracts.messaging.Message.from(payload.toByteArray());
        }

        private CompletionStage<Void> sendExistingOrActivate(
            SpotTransportAddress address) {
            return sendExisting(address).handle((ignored, failure) -> {
                if (failure == null) {
                    return CompletableFuture.<Void>completedFuture(null);
                }
                RuntimeException error = unwrap(failure);
                if (isStaleRoute(error)) {
                    invalidate(target);
                }
                return shouldReactivate(address).thenCompose(reactivate -> {
                    if (reactivate) {
                        return instanceSpots.send(
                            target, stableType, selectedMesh, payload, packetName,
                            contentType, metadata.values());
                    }
                    return CompletableFuture.<Void>failedFuture(error);
                });
            }).thenCompose(java.util.function.Function.identity());
        }

        private CompletionStage<Boolean> shouldReactivate(
            SpotTransportAddress address) {
            if (!instanceIntent || instanceSpots == null) {
                return CompletableFuture.completedFuture(false);
            }
            return instanceSpots.isStaleRoute(target, address)
                .exceptionally(ignored -> false);
        }
    }

    private final class DeferredSpotRequestCall
        implements systems.zlink.framework.spots.ZLinkSpotRequestCall {
        private final String target;
        private final systems.zlink.contracts.messaging.Message payload;
        private final Optional<String> packetName;
        private final String contentType;
        private final Duration timeout;
        private final ZLinkApplicationMetadata metadata;
        private final boolean instanceIntent;
        private final String stableType;
        private final String selectedMesh;

        private DeferredSpotRequestCall(String target, systems.zlink.contracts.messaging.Message payload,
            Optional<String> packetName, Duration timeout) {
            this(
                target,
                payload,
                packetName,
                timeout,
                null,
                ZLinkApplicationMetadata.empty(), false, null, null);
        }

        private DeferredSpotRequestCall(
            String target,
            systems.zlink.contracts.messaging.Message payload,
            Optional<String> packetName,
            Duration timeout,
            String contentType) {
            this(
                target,
                payload,
                packetName,
                timeout,
                contentType,
                ZLinkApplicationMetadata.empty(),
                false,
                null,
                null);
        }

        private DeferredSpotRequestCall(
            String target,
            systems.zlink.contracts.messaging.Message payload,
            Optional<String> packetName,
            Duration timeout,
            String contentType,
            ZLinkApplicationMetadata metadata,
            boolean instanceIntent,
            String stableType,
            String selectedMesh) {
            this.target = java.util.Objects.requireNonNull(target, "target");
            this.payload = payload;
            this.packetName = packetName;
            this.contentType = contentType;
            this.timeout = timeout;
            this.metadata = metadata;
            this.instanceIntent = instanceIntent;
            this.stableType = stableType;
            this.selectedMesh = selectedMesh;
        }

        @Override
        public systems.zlink.framework.spots.ZLinkSpotRequestCall instanceSpot() {
            return withInstanceType(null);
        }

        @Override
        public systems.zlink.framework.spots.ZLinkSpotRequestCall instanceSpot(
            String value) {
            return withInstanceType(requireStableType(value));
        }

        private DeferredSpotRequestCall withInstanceType(String value) {
            if (instanceIntent) {
                throw new IllegalStateException("instanceSpot was already set");
            }
            return new DeferredSpotRequestCall(
                target, payload, packetName, timeout, contentType, metadata, true, value,
                selectedMesh);
        }

        @Override
        public systems.zlink.framework.spots.ZLinkSpotRequestCall inMesh(
            String value) {
            if (selectedMesh != null) {
                throw new IllegalStateException("inMesh was already set");
            }
            return new DeferredSpotRequestCall(
                target, payload, packetName, timeout, contentType, metadata, instanceIntent,
                stableType, requireStableType(value));
        }

        @Override
        public systems.zlink.framework.spots.ZLinkSpotRequestCall metadata(
            String key,
            String value) {
            return new DeferredSpotRequestCall(
                target, payload, packetName, timeout, contentType, metadata.with(key, value),
                instanceIntent, stableType, selectedMesh);
        }

        @Override
        public systems.zlink.framework.spots.ZLinkSpotRequestCall metadata(
            Map<String, String> values) {
            return new DeferredSpotRequestCall(
                target, payload, packetName, timeout, contentType, metadata.withAll(values),
                instanceIntent, stableType, selectedMesh);
        }

        @Override
        public systems.zlink.framework.spots.ZLinkSpotRequestCall timeout(
            Duration value) {
            return new DeferredSpotRequestCall(
                target, payload, packetName, value, contentType, metadata, instanceIntent,
                stableType, selectedMesh);
        }
        @Override public <TReply> CompletionStage<TReply> submit(Class<TReply> replyType) {
            rejectAfterRelocationReady("Spot request submit");
            systems.zlink.framework.runtime.internal.handlers
                .ZLinkSuspendInvocationContext.rejectSameSpotWait(target);
            CompletionStage<TReply> stage = resolve(target).handle((address, failure) -> {
                if (failure == null) {
                    return requestExistingOrActivate(address, replyType);
                }
                if (!instanceIntent || instanceSpots == null) {
                    return CompletableFuture.<TReply>failedFuture(unwrap(failure));
                }
                return activateRequest(replyType);
            }).thenCompose(java.util.function.Function.identity());
            return systems.zlink.framework.execution.ZLinkAsyncSerialQueue.manageCurrent(
                stage.whenComplete((ignored, failure) -> payload.close()));
        }

        private <TReply> CompletionStage<TReply> requestExistingOrActivate(
            SpotTransportAddress address,
            Class<TReply> replyType) {
            return requestExisting(address, replyType).handle((reply, failure) -> {
                if (failure == null) {
                    return CompletableFuture.completedFuture(reply);
                }
                RuntimeException error = unwrap(failure);
                if (isStaleRoute(error)) {
                    invalidate(target);
                }
                return shouldReactivate(address, error).thenCompose(reactivate ->
                    reactivate
                        ? activateRequest(replyType)
                        : CompletableFuture.<TReply>failedFuture(error));
            }).thenCompose(java.util.function.Function.identity());
        }

        private CompletionStage<Boolean> shouldReactivate(
            SpotTransportAddress address,
            RuntimeException failure) {
            if (!instanceIntent || instanceSpots == null) {
                return CompletableFuture.completedFuture(false);
            }
            if (isStaleRoute(failure)) {
                return CompletableFuture.completedFuture(true);
            }
            return instanceSpots.isStaleRoute(target, address)
                .exceptionally(ignored -> false);
        }

        private <TReply> CompletionStage<TReply> activateRequest(
            Class<TReply> replyType) {
            return instanceSpots.request(
                    target, stableType, selectedMesh, copyPayload(), packetName,
                    contentType, metadata.values(), timeout)
                .thenApply(parts -> {
                    try {
                        return routeMessages.decodeReply(parts, replyType);
                    } finally {
                        parts.forEach(systems.zlink.contracts.messaging.Message::close);
                    }
                });
        }

        private <TReply> CompletionStage<TReply> requestExisting(
            SpotTransportAddress address,
            Class<TReply> replyType) {
                backendSpot.rememberSpotAuthority(
                    address.targetNodeRid(),
                    address.spotId(),
                    address.spotGeneration(),
                    address.authorityOwnerGeneration(),
                    address.ownerLeaseGeneration());
                systems.zlink.contracts.messaging.Message transportPayload = copyPayload();
                ZLinkRequestCall call = routeMeshEnabled
                    ? routed.request(address.routerChannelId(), address.targetNodeRid(), address.spotId(),
                        address.spotGeneration(), transportPayload, packetName, contentType, timeout)
                    : direct.request(backendSpot, address.targetNodeRid(), address.spotId(),
                        address.spotGeneration(), transportPayload, packetName, contentType, timeout);
                try {
                    return call.metadata(metadata.values()).submit(replyType);
                } catch (RuntimeException failure) {
                    transportPayload.close();
                    throw failure;
                }
        }

        private systems.zlink.contracts.messaging.Message copyPayload() {
            return systems.zlink.contracts.messaging.Message.from(payload.toByteArray());
        }

        @Override
        public <TReply> CompletionStage<TReply> yield(Class<TReply> replyType) {
            systems.zlink.framework.runtime.internal.handlers
                .ZLinkSuspendInvocationContext.requireYieldAllowed("Spot request");
            return systems.zlink.framework.execution.ZLinkAsyncSerialQueue
                .yieldCurrent(submit(replyType));
        }
    }

    private static RuntimeException unwrap(Throwable failure) {
        Throwable current = failure;
        while ((current instanceof java.util.concurrent.CompletionException
                || current instanceof java.util.concurrent.ExecutionException)
            && current.getCause() != null) {
            current = current.getCause();
        }
        return current instanceof RuntimeException runtime
            ? runtime : new RuntimeException(current);
    }

    private static boolean isStaleRoute(Throwable failure) {
        return failure instanceof ZLinkFrameworkException error
            && error.kind() == ZLinkFrameworkErrorKind.NOT_FOUND;
    }

    private static String requireStableType(String value) {
        if (value == null || value.isBlank()
            || value.indexOf('\0') >= 0
            || value.getBytes(java.nio.charset.StandardCharsets.UTF_8).length > 255) {
            throw new IllegalArgumentException(
                "value must be 1..255 UTF-8 bytes without NUL");
        }
        return value;
    }

    private static void rejectAfterRelocationReady(String operation) {
        systems.zlink.framework.runtime.internal.handlers
            .ZLinkSuspendInvocationContext.rejectAfterRelocationReady(
                operation);
    }

}
