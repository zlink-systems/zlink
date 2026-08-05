package systems.zlink.framework.runtime.channels;

import systems.zlink.framework.runtime.internal.calls.ZLinkOneWayCalls;

import systems.zlink.framework.runtime.internal.backend.*;

import com.fasterxml.jackson.databind.JsonNode;
import com.fasterxml.jackson.databind.ObjectMapper;
import java.lang.reflect.Method;
import java.nio.charset.StandardCharsets;
import java.time.Duration;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.List;
import java.util.Map;
import java.util.Objects;
import java.util.Optional;
import java.util.Set;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionException;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.ConcurrentHashMap;
import java.util.concurrent.Executor;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.ScheduledExecutorService;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.TimeoutException;
import java.util.concurrent.locks.LockSupport;
import java.util.function.Supplier;
import java.util.logging.Logger;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.errors.ZlinkCloseException;
import systems.zlink.contracts.errors.ZlinkRecvException;
import systems.zlink.contracts.errors.ZlinkRequestException;
import systems.zlink.contracts.errors.ZlinkSubmitException;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.contracts.sockets.SendFlags;
import systems.zlink.contracts.sockets.SubmitResult;
import systems.zlink.framework.ZLinkMessageContext;
import systems.zlink.framework.ZLinkHandlerFilter;
import systems.zlink.framework.ZLinkMessageSerializer;
import systems.zlink.framework.channels.ZLinkClient;
import systems.zlink.framework.channels.ZLinkFanoutClient;
import systems.zlink.framework.channels.ZLinkChannelRuntimeOptions;
import systems.zlink.framework.channels.ZLinkClientServerChannelRuntimeOptions;
import systems.zlink.framework.channels.ZLinkPublishCall;
import systems.zlink.framework.channels.ZLinkPublishMessageContext;
import systems.zlink.framework.channels.ZLinkRouteClient;
import systems.zlink.framework.channels.ZLinkRequestCall;
import systems.zlink.framework.channels.ZLinkRouteMessageContext;
import systems.zlink.framework.channels.ZLinkSendCall;
import systems.zlink.framework.channels.ZLinkSocketRuntimeOptions;
import systems.zlink.framework.configuration.ZLinkDispatchErrorAction;
import systems.zlink.framework.configuration.ZLinkDispatchErrorReason;
import systems.zlink.framework.configuration.ZLinkDispatchErrorSurface;
import systems.zlink.framework.configuration.ZLinkDispatchMessageKind;
import systems.zlink.framework.configuration.ZLinkMessageFlowEvent;
import systems.zlink.framework.configuration.ZLinkMessageFlowOutcome;
import systems.zlink.framework.configuration.ZLinkDispatchFailure;
import systems.zlink.framework.errors.ZLinkConfigurationException;
import systems.zlink.framework.errors.ZLinkFrameworkErrorKind;
import systems.zlink.framework.errors.ZLinkFrameworkException;
import systems.zlink.framework.execution.ZLinkAsyncSerialQueue;
import systems.zlink.framework.runtime.internal.locations.ZLinkAutoConnectType;
import systems.zlink.framework.locations.ZLinkLocationRole;
import systems.zlink.framework.spots.SpotHandle;
import systems.zlink.framework.runtime.internal.spots.SpotTransportAddress;
import systems.zlink.framework.runtime.internal.spots.SpotTransportAddressResolver;
import systems.zlink.framework.runtime.internal.spots.ZLinkInstanceSpotCallRuntime;
import systems.zlink.framework.runtime.messaging.ZLinkApplicationMetadata;
import systems.zlink.framework.runtime.internal.monitoring.ZLinkRuntimeEventDispatcher;
import systems.zlink.framework.runtime.internal.configuration.ZLinkCodecRegistration;
import systems.zlink.framework.runtime.configuration.ZLinkFrameworkRegistration;
import systems.zlink.framework.runtime.diagnostics.ZLinkDispatchErrorReporter;
import systems.zlink.framework.runtime.handlers.ZLinkHandlerScanner;
import systems.zlink.framework.runtime.internal.handlers.ZLinkHandlerActivator;
import systems.zlink.framework.runtime.handlers.ZLinkHandlerMethodInvoker;
import systems.zlink.framework.runtime.handlers.ZLinkHandlerStages;
import systems.zlink.framework.runtime.handlers.ZLinkScannedHandler;
import systems.zlink.framework.runtime.handlers.ZLinkScannedHandlerCatalog;
import systems.zlink.framework.runtime.handlers.ZLinkScannedHandlerKind;
import systems.zlink.framework.runtime.handlers.ZLinkScannedHandlerSurface;
import systems.zlink.framework.runtime.internal.handlers.ZLinkSuspendInvocationAdapter;
import systems.zlink.framework.runtime.messaging.ZLinkPayloadEncoding;
import systems.zlink.framework.runtime.messaging.ZLinkMessagePayloads;


final class RouteSpotSendCall
    implements systems.zlink.framework.spots.ZLinkSpotSendCall {
    private final java.util.concurrent.atomic.AtomicBoolean submitGate =
        new java.util.concurrent.atomic.AtomicBoolean();
    private final ZLinkChannelCallRuntime runtime;
    private final String channelName;
    private final SpotTransportAddressResolver resolver;
    private final Supplier<ZLinkInstanceSpotCallRuntime> instanceSpots;
    private final String target;
    private final Message payload;
    private final Optional<String> packetName;
    private final String contentType;
    private final boolean instanceIntent;
    private final String stableType;
    private final String selectedMesh;
    private final ZLinkApplicationMetadata metadata;

    RouteSpotSendCall(
        ZLinkChannelCallRuntime runtime,
        String channelName,
        SpotTransportAddressResolver resolver,
        Supplier<ZLinkInstanceSpotCallRuntime> instanceSpots,
        String target,
        Message payload,
        Optional<String> packetName) {
        this(runtime, channelName, resolver, instanceSpots, target, payload,
            packetName, ZLinkChannelContentTypeFrame.DEFAULT_CONTENT_TYPE,
            false, null, null, ZLinkApplicationMetadata.empty());
    }

    RouteSpotSendCall(
        ZLinkChannelCallRuntime runtime,
        String channelName,
        SpotTransportAddressResolver resolver,
        Supplier<ZLinkInstanceSpotCallRuntime> instanceSpots,
        String target,
        Message payload,
        Optional<String> packetName,
        String contentType,
        boolean instanceIntent,
        String stableType,
        String selectedMesh,
        ZLinkApplicationMetadata metadata) {
        this.runtime = runtime;
        this.channelName = channelName;
        this.resolver = resolver;
        this.instanceSpots = instanceSpots;
        this.target = target;
        this.payload = payload;
        this.packetName = packetName;
        this.contentType = contentType;
        this.instanceIntent = instanceIntent;
        this.stableType = stableType;
        this.selectedMesh = selectedMesh;
        this.metadata = metadata;
    }

    public ZLinkSendCall packetName(String packetName) {
        return new RouteSpotSendCall(
            runtime,
            channelName,
            resolver,
            instanceSpots,
            target,
            payload,
            Optional.of(packetName),
            contentType,
            instanceIntent, stableType, selectedMesh, metadata);
    }

    @Override public systems.zlink.framework.spots.ZLinkSpotSendCall instanceSpot() {
        return withInstanceType(null);
    }
    @Override public systems.zlink.framework.spots.ZLinkSpotSendCall instanceSpot(String value) {
        return withInstanceType(SpotCallAddresses.requireText(value));
    }
    private RouteSpotSendCall withInstanceType(String value) {
        if (instanceIntent) throw new IllegalStateException("instanceSpot was already set");
        return new RouteSpotSendCall(runtime, channelName, resolver, instanceSpots,
            target, payload, packetName, contentType, true, value, selectedMesh, metadata);
    }
    @Override public systems.zlink.framework.spots.ZLinkSpotSendCall inMesh(String value) {
        if (selectedMesh != null) throw new IllegalStateException("inMesh was already set");
        return new RouteSpotSendCall(runtime, channelName, resolver, instanceSpots,
            target, payload, packetName, contentType, instanceIntent, stableType,
            SpotCallAddresses.requireText(value), metadata);
    }
    @Override public systems.zlink.framework.spots.ZLinkSpotSendCall metadata(String key, String value) {
        return new RouteSpotSendCall(runtime, channelName, resolver, instanceSpots,
            target, payload, packetName, contentType, instanceIntent, stableType, selectedMesh,
            metadata.with(key, value));
    }
    @Override public systems.zlink.framework.spots.ZLinkSpotSendCall metadata(Map<String, String> values) {
        return new RouteSpotSendCall(runtime, channelName, resolver, instanceSpots,
            target, payload, packetName, contentType, instanceIntent, stableType, selectedMesh,
            metadata.withAll(values));
    }

    @Override
    public CompletionStage<Void> submit() {
        CompletionStage<Void> duplicate =
            ZLinkOneWayCalls.beginOneWay(submitGate);
        if (duplicate != null) {
            return duplicate;
        }
        ZLinkInstanceSpotCallRuntime activation = instanceSpots == null
            ? null : instanceSpots.get();
        return SpotCallAddresses.resolve(resolver, target).handle((address, failure) -> {
            if (failure == null) {
                return submitExistingOrActivate(address, activation);
            }
            if (!instanceIntent || activation == null) {
                return CompletableFuture.<Void>failedFuture(
                    SpotCallAddresses.unwrap(failure));
            }
            return activation.send(target, stableType, selectedMesh, payload,
                packetName, contentType, metadata.values());
        }).thenCompose(java.util.function.Function.identity());
    }

    private CompletionStage<Void> submitExistingOrActivate(
        SpotTransportAddress address,
        ZLinkInstanceSpotCallRuntime activation) {
        return submitExisting(address).handle((ignored, failure) -> {
            if (failure == null) {
                return CompletableFuture.<Void>completedFuture(null);
            }
            RuntimeException error = SpotCallAddresses.unwrap(failure);
            invalidateStaleRoute(error);
            return shouldReactivate(address, error, activation)
                .thenCompose(reactivate -> reactivate
                    ? activation.send(
                        target, stableType, selectedMesh, payload,
                        packetName, contentType, metadata.values())
                    : CompletableFuture.<Void>failedFuture(error));
        }).thenCompose(java.util.function.Function.identity());
    }

    private void invalidateStaleRoute(RuntimeException failure) {
        if (resolver != null && SpotCallAddresses.isStaleRoute(failure)) {
            resolver.invalidate(target);
        }
    }

    private CompletionStage<Boolean> shouldReactivate(
        SpotTransportAddress address,
        RuntimeException failure,
        ZLinkInstanceSpotCallRuntime activation) {
        if (!instanceIntent || activation == null) {
            return CompletableFuture.completedFuture(false);
        }
        if (SpotCallAddresses.isStaleRoute(failure)) {
            return CompletableFuture.completedFuture(true);
        }
        return activation.isStaleRoute(target, address)
            .exceptionally(ignored -> false);
    }

    private CompletionStage<Void> submitExisting(SpotTransportAddress address) {
            List<Message> sendParts = ZLinkChannelCallRuntime.copyParts(
                packetName, payload, contentType);
            try {
                return runtime.sendToSpot(
                    address.routerChannelId(),
                    address.targetNodeRid(),
                    address.spotId(),
                    address.spotGeneration(),
                    sendParts).whenComplete((ignored, error) -> sendParts.forEach(Message::close));
            } catch (RuntimeException error) {
                sendParts.forEach(Message::close);
                throw error;
            }
    }
}

final class RouteSpotRequestCall
    implements systems.zlink.framework.spots.ZLinkSpotRequestCall {
    private final ZLinkChannelCallRuntime runtime;
    private final String channelName;
    private final SpotTransportAddressResolver resolver;
    private final Supplier<ZLinkInstanceSpotCallRuntime> instanceSpots;
    private final String target;
    private final Message payload;
    private final Optional<String> packetName;
    private final Duration timeout;
    private final String contentType;
    private final boolean instanceIntent;
    private final String stableType;
    private final String selectedMesh;
    private final ZLinkApplicationMetadata metadata;

    RouteSpotRequestCall(
        ZLinkChannelCallRuntime runtime,
        String channelName,
        SpotTransportAddressResolver resolver,
        Supplier<ZLinkInstanceSpotCallRuntime> instanceSpots,
        String target,
        Message payload,
        Optional<String> packetName,
        Duration timeout) {
        this(runtime, channelName, resolver, instanceSpots, target, payload,
            packetName, timeout, ZLinkChannelContentTypeFrame.DEFAULT_CONTENT_TYPE,
            false, null, null,
            ZLinkApplicationMetadata.empty());
    }

    RouteSpotRequestCall(
        ZLinkChannelCallRuntime runtime,
        String channelName,
        SpotTransportAddressResolver resolver,
        Supplier<ZLinkInstanceSpotCallRuntime> instanceSpots,
        String target,
        Message payload,
        Optional<String> packetName,
        Duration timeout,
        String contentType,
        boolean instanceIntent,
        String stableType,
        String selectedMesh,
        ZLinkApplicationMetadata metadata) {
        this.runtime = runtime;
        this.channelName = channelName;
        this.resolver = resolver;
        this.instanceSpots = instanceSpots;
        this.target = target;
        this.payload = payload;
        this.packetName = packetName;
        this.timeout = timeout;
        this.contentType = contentType;
        this.instanceIntent = instanceIntent;
        this.stableType = stableType;
        this.selectedMesh = selectedMesh;
        this.metadata = metadata;
    }

    public ZLinkRequestCall packetName(String packetName) {
        return new RouteSpotRequestCall(
            runtime,
            channelName,
            resolver,
            instanceSpots,
            target,
            payload,
            Optional.of(packetName),
            timeout,
            contentType,
            instanceIntent, stableType, selectedMesh, metadata);
    }

    @Override public systems.zlink.framework.spots.ZLinkSpotRequestCall instanceSpot() {
        return withInstanceType(null);
    }
    @Override public systems.zlink.framework.spots.ZLinkSpotRequestCall instanceSpot(String value) {
        return withInstanceType(SpotCallAddresses.requireText(value));
    }
    private RouteSpotRequestCall withInstanceType(String value) {
        if (instanceIntent) throw new IllegalStateException("instanceSpot was already set");
        return new RouteSpotRequestCall(runtime, channelName, resolver, instanceSpots,
            target, payload, packetName, timeout, contentType, true, value, selectedMesh, metadata);
    }
    @Override public systems.zlink.framework.spots.ZLinkSpotRequestCall inMesh(String value) {
        if (selectedMesh != null) throw new IllegalStateException("inMesh was already set");
        return new RouteSpotRequestCall(runtime, channelName, resolver, instanceSpots,
            target, payload, packetName, timeout, contentType, instanceIntent, stableType,
            SpotCallAddresses.requireText(value), metadata);
    }
    @Override public systems.zlink.framework.spots.ZLinkSpotRequestCall metadata(String key, String value) {
        return new RouteSpotRequestCall(runtime, channelName, resolver, instanceSpots,
            target, payload, packetName, timeout, contentType, instanceIntent, stableType,
            selectedMesh, metadata.with(key, value));
    }
    @Override public systems.zlink.framework.spots.ZLinkSpotRequestCall metadata(Map<String, String> values) {
        return new RouteSpotRequestCall(runtime, channelName, resolver, instanceSpots,
            target, payload, packetName, timeout, contentType, instanceIntent, stableType,
            selectedMesh, metadata.withAll(values));
    }

    @Override
    public systems.zlink.framework.spots.ZLinkSpotRequestCall timeout(
        Duration timeout) {
        return new RouteSpotRequestCall(
            runtime,
            channelName,
            resolver,
            instanceSpots,
            target,
            payload,
            packetName,
            timeout,
            contentType,
            instanceIntent, stableType, selectedMesh, metadata);
    }

    @Override
    public <TReply> CompletionStage<TReply> submit(Class<TReply> replyType) {
        systems.zlink.framework.runtime.internal.handlers
            .ZLinkSuspendInvocationContext.rejectSameSpotWait(target);
        ZLinkInstanceSpotCallRuntime activation = instanceSpots == null
            ? null : instanceSpots.get();
        CompletionStage<TReply> stage = SpotCallAddresses.resolve(resolver, target).handle((address, failure) -> {
            if (failure == null) {
                return submitExistingOrActivate(address, replyType, activation);
            }
            if (!instanceIntent || activation == null) {
                return CompletableFuture.<TReply>failedFuture(
                    SpotCallAddresses.unwrap(failure));
            }
            return activateRequest(activation, replyType);
        }).thenCompose(java.util.function.Function.identity());
        return systems.zlink.framework.execution.ZLinkAsyncSerialQueue.manageCurrent(stage);
    }

    private <TReply> CompletionStage<TReply> submitExistingOrActivate(
        SpotTransportAddress address,
        Class<TReply> replyType,
        ZLinkInstanceSpotCallRuntime activation) {
        return submitExisting(address, replyType).handle((reply, failure) -> {
            if (failure == null) {
                return CompletableFuture.completedFuture(reply);
            }
            RuntimeException error = SpotCallAddresses.unwrap(failure);
            invalidateStaleRoute(error);
            return shouldReactivate(address, error, activation)
                .thenCompose(reactivate -> reactivate
                    ? activateRequest(activation, replyType)
                    : CompletableFuture.<TReply>failedFuture(error));
        }).thenCompose(java.util.function.Function.identity());
    }

    private void invalidateStaleRoute(RuntimeException failure) {
        if (resolver != null && SpotCallAddresses.isStaleRoute(failure)) {
            resolver.invalidate(target);
        }
    }

    private CompletionStage<Boolean> shouldReactivate(
        SpotTransportAddress address,
        RuntimeException failure,
        ZLinkInstanceSpotCallRuntime activation) {
        if (!instanceIntent || activation == null) {
            return CompletableFuture.completedFuture(false);
        }
        if (SpotCallAddresses.isStaleRoute(failure)) {
            return CompletableFuture.completedFuture(true);
        }
        return activation.isStaleRoute(target, address)
            .exceptionally(ignored -> false);
    }

    private <TReply> CompletionStage<TReply> activateRequest(
        ZLinkInstanceSpotCallRuntime activation,
        Class<TReply> replyType) {
        return activation.request(target, stableType, selectedMesh, payload,
                packetName, contentType, metadata.values(), timeout)
            .thenApply(parts -> {
                try {
                    return runtime.decodeSpotReply(parts, replyType);
                } finally {
                    parts.forEach(Message::close);
                }
            });
    }

    private <TReply> CompletionStage<TReply> submitExisting(
        SpotTransportAddress address,
        Class<TReply> replyType) {
            List<Message> requestParts = ZLinkChannelCallRuntime.copyParts(
                packetName, payload, contentType);
            return runtime.requestToSpot(
                address.routerChannelId(),
                address.targetNodeRid(),
                address.spotId(),
                address.spotGeneration(),
                requestParts,
                timeout)
                .thenApply(replyParts -> {
                    try {
                        return runtime.decodeSpotReply(replyParts, replyType);
                    } finally {
                        replyParts.forEach(Message::close);
                    }
                }).whenComplete((ignored, error) -> requestParts.forEach(Message::close));
    }

    @Override
    public <TReply> CompletionStage<TReply> yield(Class<TReply> replyType) {
        systems.zlink.framework.runtime.internal.handlers
            .ZLinkSuspendInvocationContext.requireYieldAllowed("Spot request");
        return systems.zlink.framework.execution.ZLinkAsyncSerialQueue
            .yieldCurrent(submit(replyType));
    }

}

final class SpotCallAddresses {
    private SpotCallAddresses() {
    }

    static CompletionStage<SpotTransportAddress> resolve(
        SpotTransportAddressResolver resolver,
        String target) {
        if (resolver == null) {
            return CompletableFuture.failedFuture(new ZLinkConfigurationException(
                "SpotHandle resolver is not configured"));
        }
        return resolver.resolve(target).thenCompose(address -> address
            .map(CompletableFuture::completedFuture)
            .orElseGet(() -> CompletableFuture.failedFuture(
                new ZLinkFrameworkException(
                    ZLinkFrameworkErrorKind.NOT_FOUND,
                    "SpotHandle route is stale or unavailable"))));
    }

    static RuntimeException unwrap(Throwable failure) {
        Throwable current = failure;
        while (current instanceof java.util.concurrent.CompletionException
            && current.getCause() != null) current = current.getCause();
        return current instanceof RuntimeException runtime
            ? runtime : new RuntimeException(current);
    }

    static boolean isStaleRoute(Throwable failure) {
        return failure instanceof ZLinkFrameworkException error
            && error.kind() == ZLinkFrameworkErrorKind.NOT_FOUND;
    }

    static String requireText(String value) {
        if (value == null || value.isBlank() || value.indexOf('\0') >= 0
            || value.getBytes(StandardCharsets.UTF_8).length > 255) {
            throw new IllegalArgumentException(
                "value must be 1..255 UTF-8 bytes without NUL");
        }
        return value;
    }
}
