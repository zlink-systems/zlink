package systems.zlink.framework.runtime.actors;

import systems.zlink.contracts.errors.ZlinkRequestException;
import systems.zlink.contracts.errors.ZlinkSubmitException;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.contracts.sockets.SendFlags;
import systems.zlink.framework.ZLinkMessageSerializer;
import systems.zlink.framework.actors.ZLinkActorClient;
import systems.zlink.framework.actors.ZLinkActorRequestCall;
import systems.zlink.framework.actors.ZLinkActorSendCall;
import systems.zlink.framework.errors.ZLinkFrameworkErrorKind;
import systems.zlink.framework.errors.ZLinkFrameworkException;
import systems.zlink.framework.messaging.ZLinkMessage;
import systems.zlink.framework.monitoring.ZLinkFlowOrigin;
import systems.zlink.framework.runtime.diagnostics.ZLinkMessageFlowTracer;
import systems.zlink.framework.runtime.internal.ZLinkCompletionBridge;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendActorRef;
import systems.zlink.framework.runtime.internal.backend.ZLinkInternalSpotNode;
import systems.zlink.framework.runtime.internal.calls.ZLinkOneWayCalls;
import systems.zlink.framework.runtime.internal.diagnostics.ZLinkDispatchErrorAction;
import systems.zlink.framework.runtime.internal.diagnostics.ZLinkDispatchErrorReason;
import systems.zlink.framework.runtime.internal.diagnostics.ZLinkDispatchErrorSurface;
import systems.zlink.framework.runtime.internal.diagnostics.ZLinkDispatchMessageKind;
import systems.zlink.framework.runtime.internal.diagnostics.ZLinkFlowContext;
import systems.zlink.framework.runtime.internal.diagnostics.ZLinkMessageFlowEvent;
import systems.zlink.framework.runtime.internal.diagnostics.ZLinkMessageFlowOutcome;
import systems.zlink.framework.runtime.internal.metrics.ZLinkRequestMetrics;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceM6BWireCodec;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceOperationIds;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceOperationRegistry;
import systems.zlink.framework.runtime.locations.ZLinkStoreLocationResolvers;
import systems.zlink.framework.runtime.locations.ZLinkStoreLocationResolvers.ActorRoute;
import systems.zlink.framework.runtime.messaging.ZLinkMessagePayloads;
import systems.zlink.framework.runtime.messaging.ZLinkPayloadEncoding;
import systems.zlink.framework.runtime.streams.ZLinkStreamHeader;
import systems.zlink.framework.runtime.streams.ZLinkStreamHeaderCodec;
import systems.zlink.framework.runtime.streams.ZLinkStreamHeaderFlag;
import systems.zlink.framework.streams.ZLinkStreamCodec;
import systems.zlink.framework.streams.ZLinkStreamMessageKind;

import java.time.Duration;
import java.util.EnumSet;
import java.util.List;
import java.util.Map;
import java.util.Objects;
import java.util.Optional;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionException;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.function.BooleanSupplier;
import java.util.function.Supplier;

public final class ZLinkActorClientRuntime implements ZLinkActorClient {

    private final ZLinkActorClientCalls callFactories = new ZLinkActorClientCalls(() -> this);
    final Supplier<ZLinkInternalSpotNode> spotNode;
    private final ZLinkStoreLocationResolvers locations;
    private final ZLinkMessageSerializer serializer;
    private final Duration defaultTimeout;
    private final CompletionStage<Void> runtimeReady;
    private final BooleanSupplier flowCaptureEnabled;
    private ZLinkMessageFlowTracer flowTracer;
    private final AtomicInteger runtimeReadyWaiters = new AtomicInteger();

    int runtimeReadyWaiterCountForTest() {
        return runtimeReadyWaiters.get();
    }

    public ZLinkActorClientRuntime(
            Supplier<ZLinkInternalSpotNode> spotNode,
            ZLinkStoreLocationResolvers locations,
            ZLinkMessageSerializer serializer,
            Duration defaultTimeout) {
        this(
                spotNode,
                locations,
                serializer,
                defaultTimeout,
                CompletableFuture.completedFuture(null),
                () -> false);
    }

    public ZLinkActorClientRuntime(
            Supplier<ZLinkInternalSpotNode> spotNode,
            ZLinkStoreLocationResolvers locations,
            ZLinkMessageSerializer serializer,
            Duration defaultTimeout,
            CompletionStage<Void> runtimeReady) {
        this(spotNode, locations, serializer, defaultTimeout, runtimeReady, () -> false);
    }

    public ZLinkActorClientRuntime(
            Supplier<ZLinkInternalSpotNode> spotNode,
            ZLinkStoreLocationResolvers locations,
            ZLinkMessageSerializer serializer,
            Duration defaultTimeout,
            CompletionStage<Void> runtimeReady,
            BooleanSupplier flowCaptureEnabled) {
        this.spotNode = Objects.requireNonNull(spotNode, "spotNode");
        this.locations = Objects.requireNonNull(locations, "locations");
        this.serializer = Objects.requireNonNull(serializer, "serializer");
        this.defaultTimeout = defaultTimeout == null ? Duration.ZERO : defaultTimeout;
        this.runtimeReady = Objects.requireNonNull(runtimeReady, "runtimeReady");
        this.flowCaptureEnabled = Objects.requireNonNull(flowCaptureEnabled, "flowCaptureEnabled");
    }

    public void setFlowTracer(ZLinkMessageFlowTracer tracer) {
        this.flowTracer = Objects.requireNonNull(tracer, "tracer");
    }

    ZLinkFlowContext.Scope enterApplicationFlow() {
        return ZLinkFlowContext.enterCurrentOrCreate(
                ZLinkFlowOrigin.APPLICATION, flowCaptureEnabled.getAsBoolean());
    }

    @Override
    public ZLinkActorSendCall sendToActor(String actorId, Object message) {
        return callFactories.sendToActor(actorId, message);
    }

    @Override
    public ZLinkActorRequestCall requestToActor(String actorId, Object request) {
        return callFactories.requestToActor(actorId, request);
    }

    <TReply> CompletionStage<TReply> requestAsync(
            String actorId,
            String packetName,
            Object request,
            Map<String, String> metadata,
            Duration timeout,
            Class<TReply> replyType,
            ZLinkRequestMetrics.Series metric,
            long started,
            CompletableFuture<Void> requestAdmission) {
        ZLinkFlowContext.State operationFlow = ZLinkFlowContext.current();
        Duration requestTimeout = timeout == null ? defaultTimeout : timeout;
        var node = spotNode.get();
        var operations = Objects.requireNonNull(node.operationRegistry(), "Actor operation owner");
        var result =
                ZLinkServiceOperationRegistry.<TReply, TReply>prepareRequestOperation(
                        ZLinkServiceOperationIds.next(),
                        java.util.function.Function.identity(),
                        null,
                        failure -> mapBackendException(failure, "Actor request"),
                        (receiver, failure) -> {
                            ZLinkRequestMetrics.complete(
                                    metric,
                                    started == ZLinkRequestMetrics.NO_START
                                            ? -1L
                                            : ZLinkRequestMetrics.elapsed(
                                                    started, System.nanoTime()),
                                    failure);
                            if (flowTracer == null) return;
                            var terminal = flowTracer.beginRequestTerminal(failure, receiver);
                            if (terminal != null) {
                                ZLinkFlowContext.call(
                                        operationFlow,
                                        () -> {
                                            terminal.trace(
                                                    new ZLinkMessageFlowEvent(
                                                            ZLinkMessageFlowOutcome.COMPLETED,
                                                            ZLinkDispatchErrorSurface.SPOT_ACTOR,
                                                            ZLinkDispatchMessageKind.ACTOR_REQUEST,
                                                            packetName,
                                                            null,
                                                            null,
                                                            null,
                                                            null,
                                                            null,
                                                            actorId,
                                                            null,
                                                            null,
                                                            null,
                                                            failure == null
                                                                    ? null
                                                                    : failure.getClass().getName(),
                                                            failure == null
                                                                    ? null
                                                                    : failure.getMessage(),
                                                            null,
                                                            null));
                                            return null;
                                        });
                            }
                        });
        operations.<TReply>submitTerminal(
                result.id(),
                requestTimeout,
                terminal -> {
                    CompletionStage<ActorRoute> route = resolveActorAddress(actorId);
                    result.whenComplete(
                            (ignored, failure) -> route.toCompletableFuture().cancel(false));
                    route.whenComplete(
                            (row, error) -> {
                                if (!operations.isPending(result.id())) return;
                                if (error != null) {
                                    terminal.accept(null, unwrap(error));
                                    return;
                                }
                                try {
                                    ZLinkFlowContext.call(
                                            operationFlow,
                                            () -> {
                                                submitRequest(
                                                        node,
                                                        actorRouteFence(row, actorId),
                                                        packetName,
                                                        request,
                                                        metadata,
                                                        operations.remainingTimeout(result.id()),
                                                        replyType,
                                                        requestAdmission,
                                                        result);
                                                return null;
                                            });
                                } catch (RuntimeException | Error failure) {
                                    terminal.accept(null, failure);
                                }
                            });
                    return route;
                },
                ignored -> {},
                result);
        return result;
    }

    private CompletionStage<ActorRoute> resolveActorAddress(String actorId) {
        CompletionStage<Void> readiness = awaitRuntimeReady();
        CompletableFuture<ActorRoute> route = new CompletableFuture<>();
        ZLinkCompletionBridge.forwardCancellation(route, readiness);
        readiness.whenComplete(
                (ignored, error) -> {
                    if (route.isDone()) return;
                    if (error != null) {
                        route.completeExceptionally(unwrap(error));
                        return;
                    }
                    try {
                        CompletionStage<ActorRoute> resolved = locations.resolveActor(actorId);
                        ZLinkCompletionBridge.forwardCancellation(route, resolved);
                        resolved.whenComplete(
                                (row, failure) -> {
                                    if (failure != null)
                                        route.completeExceptionally(unwrap(failure));
                                    else route.complete(row);
                                });
                    } catch (RuntimeException | Error failure) {
                        route.completeExceptionally(failure);
                    }
                });
        return route;
    }

    private ZLinkServiceM6BWireCodec.ActorRouteFence actorRouteFence(
            ActorRoute row, String actorId) {
        if (row == null || row.actorRef() == null) {
            throw new ZLinkFrameworkException(
                    ZLinkFrameworkErrorKind.NOT_FOUND,
                    "Actor route '" + actorId + "' was not found.");
        }
        return new ZLinkServiceM6BWireCodec.ActorRouteFence(
                toBackendActorRef(row),
                row.targetNodeGeneration(),
                row.authorityOwnerGeneration(),
                row.ownerLeaseGeneration());
    }

    CompletionStage<Void> submitSendResult(
            String actorId, String packetName, Object message, Map<String, String> metadata) {
        ZLinkFlowContext.State operationFlow = ZLinkFlowContext.current();
        return ZLinkOneWayCalls.adaptOneWay(
                resolveActorAddress(actorId)
                        .thenCompose(
                                row ->
                                        ZLinkFlowContext.call(
                                                operationFlow,
                                                () -> {
                                                    ZLinkServiceM6BWireCodec.ActorRouteFence actor =
                                                            actorRouteFence(row, actorId);
                                                    byte[] header =
                                                            encodePacketHeader(
                                                                    ZLinkStreamMessageKind.SEND,
                                                                    Optional.empty(),
                                                                    packetName,
                                                                    message,
                                                                    metadata);
                                                    List<Message> parts;
                                                    try {
                                                        parts = createPacketParts(header, message);
                                                    } catch (RuntimeException failure) {
                                                        if (flowTracer != null) {
                                                            flowTracer.reportFailure(
                                                                    ZLinkDispatchErrorSurface
                                                                            .SPOT_ACTOR,
                                                                    ZLinkDispatchMessageKind
                                                                            .ACTOR_SEND,
                                                                    ZLinkDispatchErrorReason
                                                                            .PAYLOAD_ENCODE_FAILED,
                                                                    ZLinkDispatchErrorAction
                                                                            .FAIL_CALLER,
                                                                    packetName,
                                                                    actorId,
                                                                    failure);
                                                        }
                                                        throw failure;
                                                    }
                                                    ZLinkInternalSpotNode node = spotNode.get();
                                                    CompletionStage<Void> submission;
                                                    try {
                                                        submission =
                                                                node.sendToActorAsync(actor, parts);
                                                    } catch (RuntimeException failure) {
                                                        closeAll(parts);
                                                        return CompletableFuture.failedFuture(
                                                                mapActorFailure(
                                                                        actor,
                                                                        failure,
                                                                        "Actor send"));
                                                    }
                                                    return ZLinkOneWayCalls.adaptOneWay(
                                                            ZLinkOneWayCalls.closeAfterAdmission(
                                                                    submission, parts),
                                                            false,
                                                            failure ->
                                                                    mapActorFailure(
                                                                            actor,
                                                                            failure,
                                                                            "Actor send"));
                                                })),
                false);
    }

    private CompletionStage<Void> awaitRuntimeReady() {
        if (runtimeReady.toCompletableFuture().isDone()) return runtimeReady;
        runtimeReadyWaiters.incrementAndGet();
        CompletableFuture<Void> result = new CompletableFuture<>();
        result.whenComplete((ignored, failure) -> runtimeReadyWaiters.decrementAndGet());
        runtimeReady.whenComplete(
                (ignored, failure) -> {
                    if (failure == null) result.complete(null);
                    else result.completeExceptionally(unwrap(failure));
                });
        return result;
    }

    private <TReply> void submitRequest(
            ZLinkInternalSpotNode node,
            ZLinkServiceM6BWireCodec.ActorRouteFence actor,
            String packetName,
            Object request,
            Map<String, String> metadata,
            Duration timeout,
            Class<TReply> replyType,
            CompletableFuture<Void> requestAdmission,
            systems.zlink.framework.runtime.internal.service.ZLinkServiceOperationRegistry
                                    .Operation<
                            TReply>
                    completion) {
        List<Message> parts =
                createPacketParts(
                        encodePacketHeader(
                                ZLinkStreamMessageKind.REQUEST,
                                Optional.of(1L),
                                packetName,
                                request,
                                metadata),
                        request);
        try {
            node.requestToActor(
                    actor,
                    parts,
                    SendFlags.NONE,
                    timeout,
                    requestAdmission,
                    replyParts -> {
                        try {
                            return ZLinkActorReplyDecoder.decode(replyParts, serializer, replyType);
                        } finally {
                            closeAll(replyParts);
                        }
                    },
                    ignored -> {},
                    failure -> mapActorFailure(actor, failure, "Actor request"),
                    completion);
        } catch (RuntimeException error) {
            node.operationRegistry()
                    .completeExceptionally(
                            completion.id(), mapActorFailure(actor, error, "Actor request"));
        } finally {
            closeAll(parts);
        }
    }

    private RuntimeException mapActorFailure(
            ZLinkServiceM6BWireCodec.ActorRouteFence actor,
            Throwable failure,
            String operationName) {
        RuntimeException mapped = mapBackendException(failure, operationName);
        if (isStaleActorError(mapped))
            locations.invalidateRouteIfMatches(ZLinkActorRuntime.messageFollowRoute(actor));
        return mapped;
    }

    private byte[] encodePacketHeader(
            ZLinkStreamMessageKind kind,
            Optional<Long> requestSeq,
            String packetName,
            Object message,
            Map<String, String> metadata) {
        ZLinkStreamHeader header =
                new ZLinkStreamHeader(
                        kind,
                        ZLinkPayloadEncoding.streamCodec(
                                serializer, message, ZLinkStreamCodec.JSON),
                        EnumSet.noneOf(ZLinkStreamHeaderFlag.class),
                        requestSeq,
                        packetName,
                        Map.copyOf(metadata));
        return ZLinkStreamHeaderCodec.encode(header);
    }

    private List<Message> createPacketParts(byte[] header, Object message) {
        Message headerPart = Message.from(header);
        try {
            return List.of(
                    headerPart, ZLinkMessagePayloads.message(ZLinkMessage.of(message), serializer));
        } catch (RuntimeException | Error failure) {
            headerPart.close();
            throw failure;
        }
    }

    private RuntimeException mapBackendException(Throwable error, String operationName) {
        Throwable unwrapped = unwrap(error);
        if (unwrapped instanceof ZlinkRequestException request) {
            return new ZLinkFrameworkException(
                    systems.zlink.framework.runtime.internal.backend.ZLinkBackendRequestResult
                            .fromWireTerminal(request.getResult().value())
                            .toFrameworkErrorKind(),
                    operationName + " failed: " + request.getResult(),
                    request);
        }
        if (unwrapped instanceof ZlinkSubmitException submit) {
            return switch (submit.getResult()) {
                case NOT_CONNECTED ->
                        new ZLinkFrameworkException(
                                ZLinkFrameworkErrorKind.UNAVAILABLE,
                                operationName
                                        + " failed because the target route is not connected.",
                                submit);
                case NOT_FOUND ->
                        new ZLinkFrameworkException(
                                ZLinkFrameworkErrorKind.NOT_FOUND,
                                operationName + " failed because the actor route was not found.",
                                submit);
                default -> submit;
            };
        }
        if (unwrapped instanceof RuntimeException runtimeException) {
            return runtimeException;
        }
        return new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.INTERNAL_FAILURE, operationName + " failed.", unwrapped);
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

    private static Throwable unwrap(Throwable error) {
        Throwable current = error;
        while (current instanceof CompletionException && current.getCause() != null) {
            current = current.getCause();
        }
        return current;
    }

    private static void closeAll(List<Message> parts) {
        Message.closeAll(parts);
    }

    static String requireActorId(String actorId) {
        Objects.requireNonNull(actorId, "actorId");
        if (actorId.isBlank() || actorId.indexOf('\0') >= 0) {
            throw new IllegalArgumentException("actorId is required");
        }
        return actorId;
    }
}
