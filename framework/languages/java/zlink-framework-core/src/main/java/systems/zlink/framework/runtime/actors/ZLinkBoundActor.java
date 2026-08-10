package systems.zlink.framework.runtime.actors;
import java.util.Objects;
import java.util.logging.Logger;
import systems.zlink.framework.runtime.internal.diagnostics.ZLinkDispatchErrorSurface;
import systems.zlink.framework.runtime.internal.diagnostics.ZLinkDispatchMessageKind;
import systems.zlink.framework.runtime.internal.diagnostics.ZLinkMessageFlowEvent;
import systems.zlink.framework.runtime.internal.diagnostics.ZLinkMessageFlowOutcome;

import java.time.Duration;
import systems.zlink.framework.runtime.internal.calls.ZLinkOneWayCalls;

import java.util.EnumSet;
import java.util.List;
import java.util.Map;
import java.util.Optional;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionException;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.TimeoutException;
import java.util.function.BooleanSupplier;
import java.util.function.Predicate;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.errors.ZlinkSubmitException;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.contracts.sockets.SendFlags;
import systems.zlink.framework.ZLinkMessageSerializer;
import systems.zlink.framework.actors.ActorRef;
import systems.zlink.framework.actors.ZLinkActor;
import systems.zlink.framework.errors.ZLinkConfigurationException;
import systems.zlink.framework.messaging.ZLinkMessage;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendActorRef;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendStreamSocket;
import systems.zlink.framework.runtime.messaging.ZLinkMessagePayloads;

import systems.zlink.framework.runtime.diagnostics.ZLinkMessageFlowTracer;
import systems.zlink.framework.runtime.streams.ZLinkStreamHeader;
import systems.zlink.framework.runtime.streams.ZLinkStreamHeaderFlag;
import systems.zlink.framework.streams.ZLinkSessionActor;
import systems.zlink.framework.streams.ZLinkSessionDispatchContext;
import systems.zlink.framework.streams.ZLinkStreamCodec;
import systems.zlink.framework.streams.ZLinkStreamMessageKind;

final class ZLinkBoundActor implements ZLinkSessionActor {
    private final ZLinkBackendStreamSocket stream;
    private final RoutingId sessionRid;
    private volatile ZLinkBackendActorRef ref;
    private final String meshName;
    private final Optional<ZLinkActor> managedActor;
    private final ZLinkActorRuntime actors;
    private final ZLinkMessageSerializer serializer;
    private final long bindingToken;
    private final long bindingGeneration;
    private final Predicate<RoutingId> routeReady;
    private final ZLinkSessionActorsRuntime.LocalActorDispatcher localActorDispatcher;
    private final boolean nativeSessionRelayAttached;
    private final ZLinkStreamCodec defaultCodec;
    private final ZLinkSessionRelayHeaders relayHeaders;
    private final ZLinkMessageFlowTracer flow;
    private final BooleanSupplier currentBinding;
    private final ZLinkSessionActorsRuntime.IngressAdmission ingressAdmission;
    private final ZLinkRelayMetadataPolicy metadataPolicy;
    private Runnable unbindListener = () -> {};
    private volatile boolean nativeRebound;
    private CompletableFuture<Void> disconnect;

    ZLinkBoundActor(
        ZLinkBackendStreamSocket stream,
        RoutingId sessionRid,
        ZLinkBackendActorRef ref,
        String meshName,
        Optional<ZLinkActor> managedActor,
        ZLinkActorRuntime actors,
        ZLinkMessageSerializer serializer,
        long bindingToken,
        long bindingGeneration,
        Predicate<RoutingId> routeReady,
        ZLinkSessionActorsRuntime.LocalActorDispatcher localActorDispatcher,
        boolean nativeSessionRelayAttached,
        ZLinkStreamCodec defaultCodec,
        ZLinkSessionRelayHeaders relayHeaders,
        ZLinkMessageFlowTracer flow,
        BooleanSupplier currentBinding,
        ZLinkSessionActorsRuntime.IngressAdmission ingressAdmission,
        ZLinkRelayMetadataPolicy metadataPolicy) {
        this.stream = stream;
        this.sessionRid = sessionRid;
        this.ref = ref;
        this.meshName = Objects.requireNonNull(meshName, "meshName");
        this.managedActor = managedActor;
        this.actors = actors;
        this.serializer = Objects.requireNonNull(serializer, "serializer");
        this.bindingToken = bindingToken;
        if (bindingGeneration <= 0) {
            throw new IllegalArgumentException(
                "bound Session binding generation must be positive");
        }
        this.bindingGeneration = bindingGeneration;
        this.routeReady = routeReady == null ? ignored -> true : routeReady;
        this.localActorDispatcher = localActorDispatcher;
        this.nativeSessionRelayAttached = nativeSessionRelayAttached;
        this.defaultCodec = defaultCodec == null ? ZLinkStreamCodec.JSON : defaultCodec;
        this.relayHeaders = relayHeaders;
        this.flow = flow;
        this.currentBinding = currentBinding == null ? () -> true : currentBinding;
        this.ingressAdmission = ingressAdmission == null
            ? operation -> operation.apply(0)
            : ingressAdmission;
        this.metadataPolicy =
            metadataPolicy == null ? ZLinkRelayMetadataPolicy.EMPTY : metadataPolicy;
    }

    @Override
    public String actorId() {
        return ref.actorId();
    }

    @Override
    public ActorRef ref() {
        ZLinkBackendActorRef current = ref;
        return new ActorRef(
            current.actorId(),
            current.generation(),
            meshName,
            current.nodeRid());
    }

    void rebindNativeActor(ZLinkBackendActorRef targetActor) {
        if (!ref.actorId().equals(targetActor.actorId())) {
            throw new ZLinkConfigurationException(
                "bound session actor id mismatch: " + targetActor.actorId());
        }
        if (ref.generation() != targetActor.generation()) {
            throw new ZLinkConfigurationException(
                "relocation cannot rebind bound session actor "
                    + targetActor.actorId() + " from generation "
                    + ref.generation() + " to " + targetActor.generation()
                    + "; a new actor incarnation requires an explicit bind");
        }
        ref = targetActor;
        nativeRebound = true;
    }

    long bindingGeneration() {
        return bindingGeneration;
    }

    CompletionStage<Void> prepareNativeActorRoute(
        ZLinkBackendActorRef targetActor,
        Duration timeout) {
        if (!ref.actorId().equals(targetActor.actorId())
            || ref.generation() != targetActor.generation()) {
            return CompletableFuture.failedFuture(
                new ZLinkConfigurationException(
                    "relocation route switch requires the same Actor "
                        + "identity and generation"));
        }
        return ZLinkActorRetryScheduler.waitUntilRelay(
                timeout,
                () -> routeReady.test(targetActor.nodeRid()),
                () -> {},
                () -> new TimeoutException(
                    "remote bound session route was not ready before timeout: "
                        + targetActor.actorId()))
            .thenRun(() -> relocationTrace("route-ready", targetActor))
            .thenCompose(ignored -> stream.relocateBoundActor(
                    sessionRid,
                    ref.actorId(),
                    bindingGeneration,
                    targetActor,
                    timeout))
            .thenRun(() -> relocationTrace("bind-complete", targetActor))
            .thenCompose(ignored -> notifyRemoteBoundSession());
    }

    void commitPreparedNativeActorRoute(ZLinkBackendActorRef targetActor) {
        rebindNativeActor(targetActor);
    }

    CompletionStage<Void> compensatePreparedNativeActorRoute(
        ZLinkBackendActorRef sourceActor,
        Duration timeout) {
        return stream.relocateBoundActor(
                sessionRid,
                sourceActor.actorId(),
                bindingGeneration,
                sourceActor,
                timeout)
            .thenCompose(ignored -> notifyRemoteBoundSession());
    }

    private static void relocationTrace(
        String stage,
        ZLinkBackendActorRef targetActor) {
        if ("1".equals(System.getenv("ZLINK_JAVA_STREAM_TRACE"))) {
            Logger.getLogger(
                    ZLinkBoundActor.class.getName())
                .warning("[zlink-java-stream-trace] relocation " + stage
                    + " actor=" + targetActor.actorId()
                    + " target=" + targetActor.nodeRid()
                    + " generation=" + targetActor.generation());
        }
    }

    void setUnbindListener(Runnable unbindListener) {
        this.unbindListener = unbindListener == null ? () -> {} : unbindListener;
    }

    @Override
    public CompletionStage<Void> relay(
        ZLinkMessage payload) {
        return relay(relayHeaders.current(), payload);
    }

    @Override
    public CompletionStage<Void> relay(
        ZLinkSessionDispatchContext dispatch,
        ZLinkMessage payload) {
        return relay(relayHeaders.find(dispatch).or(relayHeaders::current), payload);
    }

    private CompletionStage<Void> relay(
        Optional<ZLinkStreamHeader> currentHeader,
        ZLinkMessage payload) {
        if (payload == null) {
            return CompletableFuture.failedFuture(new IllegalArgumentException(
                "payload is required"));
        }
        if (currentHeader.isEmpty()) {
            return CompletableFuture.failedFuture(new IllegalStateException(
                "Session actor relay requires an active stream dispatch."));
        }
        ZLinkStreamHeader header = metadataPolicy.sessionToActor(currentHeader.get());
        traceRelay(header);
        Message message = ZLinkMessagePayloads.message(payload, serializer);
        byte[] payloadBytes = message.toByteArray();
        message.close();
        return ZLinkOneWayCalls.adaptOneWay(ingressAdmission.submit(
            sourceSessionSequence -> {
            if (managedActor.isPresent()
                && localActorDispatcher != null
                && !nativeRebound) {
                return relayLocal(
                    header,
                    payloadBytes,
                    sourceSessionSequence);
            }
            return relayUsingStoredBinding(
                header,
                payloadBytes,
                sourceSessionSequence);
        }));
    }

    private void traceRelay(ZLinkStreamHeader header) {
        if (flow == null
            || !flow.enabled(ZLinkMessageFlowOutcome.SENT)) {
            return;
        }
        flow.trace(new ZLinkMessageFlowEvent(
            ZLinkMessageFlowOutcome.SENT,
            ZLinkDispatchErrorSurface.SPOT_ACTOR,
            header.requestSequence().isPresent()
                ? ZLinkDispatchMessageKind.ACTOR_REQUEST
                : ZLinkDispatchMessageKind.ACTOR_SEND,
            header.packetName(),
            null,
            null,
            header.correlationId().orElse(null),
            null,
            null,
            ref.actorId(),
            null,
            null, null, null, null,
            header.flowId().orElse(null),
            header.flowOrigin().orElse(null)));
    }

    private CompletionStage<Void> relayLocal(
        ZLinkStreamHeader header,
        byte[] payloadBytes,
        long sourceSessionSequence) {
        if (localActorDispatcher == null) {
            return CompletableFuture.failedFuture(new ZLinkConfigurationException(
                "local actor dispatch requires a Spot runtime"));
        }
        Message payload = Message.from(payloadBytes);
        return localActorDispatcher.dispatch(
                ref,
                sourceSessionSequence,
                header,
                payload)
            .thenCompose(reply -> {
                if (reply.isEmpty()) {
                    return CompletableFuture.completedFuture(null);
                }
                return replyLocal(header, reply.get());
            })
            .whenComplete((ignored, error) -> payload.close());
    }

    private CompletionStage<Void> replyLocal(
        ZLinkStreamHeader header,
        ZLinkSessionActorsRuntime.LocalActorReply reply) {
        try {
            if (header.requestSequence().isEmpty()) {
                return CompletableFuture.failedFuture(new ZLinkConfigurationException(
                    "actor reply requires a stream request sequence: "
                        + header.packetName()));
            }
            ZLinkStreamHeader replyHeader = ZLinkStreamHeader.createResponse(
                header,
                reply.codec(),
                EnumSet.noneOf(ZLinkStreamHeaderFlag.class),
                header.packetName(),
                Map.of());
            byte[] replyBytes = reply.payload().toByteArray();
            return ZLinkActorRetryScheduler.submitRelayUntilAccepted(
                ZLinkSessionActorsRuntime.RELAY_SUBMIT_TIMEOUT,
                () -> {
                    try (Message attemptReply = Message.from(replyBytes)) {
                        return stream.reply(
                            sessionRid,
                            replyHeader,
                            List.of(attemptReply),
                            SendFlags.DONT_WAIT);
                    }
                },
                () -> new TimeoutException(
                    "local actor session reply was not ready before timeout: "
                        + ref.actorId()));
        } finally {
            reply.payload().close();
        }
    }

    private CompletionStage<Void> awaitRouteReady() {
        return ZLinkActorRetryScheduler.waitUntilRelayOrContinue(
            ZLinkSessionActorsRuntime.RELAY_SUBMIT_TIMEOUT,
            () -> routeReady.test(ref.nodeRid()));
    }

    private CompletionStage<Void> relayUsingStoredBinding(
        ZLinkStreamHeader header,
        byte[] payloadBytes,
        long sourceSessionSequence) {
        return ZLinkActorRetryScheduler.submitStoredRelayUntilAccepted(
            ZLinkSessionActorsRuntime.RELAY_SUBMIT_TIMEOUT,
            () -> {
                try (Message payloadPart = Message.from(payloadBytes)) {
                    return stream.relayBoundActor(
                        sessionRid,
                        ref.actorId(),
                        sourceSessionSequence,
                        header,
                        List.of(payloadPart),
                        SendFlags.DONT_WAIT);
                }
            },
            () -> new TimeoutException(
                "session relay route was not ready before timeout: "
                    + ref.actorId()));
    }

    @Override
    public CompletionStage<Void> notifyDisconnected() {
        return notifyDisconnected(ZLinkSessionActorsRuntime.RELAY_SUBMIT_TIMEOUT);
    }

    CompletionStage<Void> notifyDisconnected(Duration timeout) {
        Objects.requireNonNull(timeout, "timeout");
        CompletableFuture<Void> result;
        boolean bindingCurrent;
        synchronized (this) {
            if (disconnect != null) {
                return disconnect;
            }
            result = new CompletableFuture<>();
            disconnect = result;
        }
        try {
            bindingCurrent = currentBinding.getAsBoolean();
        } catch (RuntimeException failure) {
            result.completeExceptionally(failure);
            return result;
        }
        if (!bindingCurrent) {
            result.complete(null);
            return result;
        }
        CompletionStage<Void> notification = managedActor.isPresent() && !nativeRebound
            ? (actors.clearSessionBinding(managedActor.get(), bindingToken)
                ? actors.notifyDisconnected(managedActor.get())
                : CompletableFuture.completedFuture(null))
            : notifyRemoteDisconnected();
        notification.toCompletableFuture()
            .orTimeout(
                timeout.toMillis(),
                TimeUnit.MILLISECONDS)
            .handle((ignored, notifyError) -> notifyError)
            //  The disconnect notification can complete inline inside the
            //  transport's reply callback, still inside the native router
            //  submit that carried it. Submitting the unbind request from that
            //  frame re-enters the same socket while its non-reentrant public
            //  API sync is held and livelocks the service pump (observed as a
            //  shutdown hang that ends in SIGKILL). Always hop off the
            //  completing thread before the unbind submit.
            .thenComposeAsync(notifyError -> stream.unbindActor(sessionRid, ref.actorId())
                .submit(timeout)
                .handle((ignored, unbindError) -> {
                    if (notifyError != null) {
                        throw new CompletionException(notifyError);
                    }
                    if (unbindError != null) {
                        throw new CompletionException(unbindError);
                    }
                    return null;
                }))
            .whenComplete((ignored, error) -> {
                Throwable terminal = error;
                try {
                    unbindListener.run();
                } catch (RuntimeException listenerFailure) {
                    if (terminal == null) {
                        terminal = listenerFailure;
                    } else {
                        terminal.addSuppressed(listenerFailure);
                    }
                }
                if (terminal == null) {
                    result.complete(null);
                } else {
                    result.completeExceptionally(terminal);
                }
            });
        return result;
    }

    private CompletionStage<Void> notifyRemoteDisconnected() {
        if (!nativeSessionRelayAttached) {
            return CompletableFuture.completedFuture(null);
        }
        ZLinkStreamHeader header = new ZLinkStreamHeader(
            ZLinkStreamMessageKind.SEND,
            defaultCodec,
            EnumSet.noneOf(ZLinkStreamHeaderFlag.class),
            Optional.empty(),
            ZLinkActorSpotRoutePackets.SESSION_DISCONNECTED_PACKET_NAME,
            Map.of());
        try (Message payloadPart = Message.from(new byte[0])) {
            return stream.requestExactActor(
                    ref,
                    header,
                    List.of(payloadPart),
                    ZLinkSessionActorsRuntime.RELAY_SUBMIT_TIMEOUT)
                .thenAccept(reply -> reply.forEach(Message::close));
        }
    }

    CompletionStage<Void> notifyRemoteBoundSession() {
        if (!nativeSessionRelayAttached || managedActor.isPresent()) {
            return CompletableFuture.completedFuture(null);
        }
        ZLinkStreamHeader header = new ZLinkStreamHeader(
            ZLinkStreamMessageKind.SEND,
            ZLinkStreamCodec.RAW,
            EnumSet.noneOf(ZLinkStreamHeaderFlag.class),
            Optional.empty(),
            ZLinkBoundSessionRuntime.REMOTE_BOUND_SESSION_BIND_PACKET_NAME,
            Map.of());
        try (Message payloadPart = Message.from(new byte[0])) {
            // Binding completes only after the target Framework installs the
            // native session context and acknowledges this internal request.
            return stream.requestBoundActor(
                    sessionRid,
                    ref.actorId(),
                    header,
                    List.of(payloadPart),
                    ZLinkSessionActorsRuntime.RELAY_SUBMIT_TIMEOUT)
                .thenAccept(reply -> reply.forEach(Message::close));
        }
    }
}
