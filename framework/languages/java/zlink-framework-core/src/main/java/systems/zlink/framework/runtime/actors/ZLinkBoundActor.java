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
import systems.zlink.contracts.messaging.Message;
import systems.zlink.framework.ZLinkMessageSerializer;
import systems.zlink.framework.actors.ActorRef;
import systems.zlink.framework.actors.ZLinkActor;
import systems.zlink.framework.errors.ZLinkConfigurationException;
import systems.zlink.framework.errors.ZLinkFrameworkErrorKind;
import systems.zlink.framework.errors.ZLinkFrameworkException;
import systems.zlink.framework.messaging.ZLinkMessage;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendActorRef;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendStreamSocket;
import systems.zlink.framework.runtime.messaging.ZLinkMessagePayloads;

import systems.zlink.framework.runtime.diagnostics.ZLinkMessageFlowTracer;
import systems.zlink.framework.runtime.streams.ZLinkStreamHeader;
import systems.zlink.framework.runtime.streams.ZLinkStreamHeaderFlag;
import systems.zlink.framework.runtime.streams.ZLinkStreamFrameCodec;
import systems.zlink.framework.runtime.streams.ZLinkStreamHeaderCodec;
import systems.zlink.framework.streams.ZLinkSessionActor;
import systems.zlink.framework.streams.ZLinkSessionDispatchContext;
import systems.zlink.framework.streams.ZLinkStreamCodec;
import systems.zlink.framework.streams.ZLinkStreamMessageKind;

final class ZLinkBoundActor implements ZLinkSessionActor {
    private static final Logger LOGGER =
        Logger.getLogger(ZLinkBoundActor.class.getName());
    private static final boolean STREAM_TRACE =
        "1".equals(System.getenv("ZLINK_JAVA_STREAM_TRACE"));

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
        CompletionStage<Void> authorityReady = actors == null
            ? CompletableFuture.completedFuture(null)
            : actors.prepareRemoteSessionBinding(targetActor);
        return authorityReady.thenCompose(ignored ->
            ZLinkActorRetryScheduler.waitUntilRelay(
                timeout,
                () -> routeReady.test(targetActor.nodeRid()),
                () -> {},
                () -> {
                    String message =
                        "remote bound session route was not ready before"
                            + " timeout: " + targetActor.actorId();
                    //  Spec 32-framework-error-model:90 — a route wait past
                    //  its deadline is DeadlineExceeded, not a raw language
                    //  timeout. The TimeoutException cause is kept for
                    //  diagnostics.
                    return new ZLinkFrameworkException(
                        ZLinkFrameworkErrorKind.DEADLINE_EXCEEDED,
                        message,
                        new TimeoutException(message));
                }))
            .thenRun(() -> relocationTrace("route-ready", targetActor))
            .thenCompose(ignored -> stream.relocateBoundActor(
                    sessionRid,
                    ref.actorId(),
                    bindingGeneration,
                    targetActor,
                    timeout))
            // Specs 44/52 make command 44 one-way: target restoration already
            // installed the bound-Session context before publishing the route
            // update. Waiting for another Actor-mailbox request here can
            // deadlock behind the application turn whose relocation is being
            // completed and lets the Session seal deadline win.
            .thenRun(() -> relocationTrace("bind-complete", targetActor));
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
                timeout);
    }

    private static void relocationTrace(
        String stage,
        ZLinkBackendActorRef targetActor) {
        if (STREAM_TRACE) {
            LOGGER.warning("[zlink-java-stream-trace] relocation " + stage
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
        CompletionStage<Void> submission = ingressAdmission.submit(
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
        });
        return header.requestSequence().isPresent()
            ? submission
            : ZLinkOneWayCalls.adaptOneWay(submission);
    }

    private void traceRelay(ZLinkStreamHeader header) {
        ZLinkMessageFlowTracer.TracePoint tracePoint = flow == null
            ? null : flow.begin(ZLinkMessageFlowOutcome.SENT);
        if (tracePoint == null) {
            return;
        }
        tracePoint.trace(new ZLinkMessageFlowEvent(
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
            Message replyPart = Message.from(reply.payload());
            CompletionStage<Void> submission;
            try {
                submission = stream.replyAsync(
                    sessionRid,
                    replyHeader,
                    List.of(replyPart));
            } catch (RuntimeException failure) {
                replyPart.close();
                return CompletableFuture.failedFuture(failure);
            }
            return submission.whenComplete(
                (ignored, failure) -> replyPart.close());
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
        if (header.requestSequence().isPresent()) {
            return requestUsingStoredBinding(
                header, payloadBytes, sourceSessionSequence);
        }
        Message payloadPart = Message.from(payloadBytes);
        CompletionStage<Void> submission;
        try {
            submission = stream.relayBoundActorAsync(
                sessionRid,
                ref.actorId(),
                sourceSessionSequence,
                header,
                List.of(payloadPart));
        } catch (RuntimeException failure) {
            payloadPart.close();
            return CompletableFuture.failedFuture(failure);
        }
        submission.whenComplete((ignored, failure) -> payloadPart.close());
        return submission;
    }

    private CompletionStage<Void> requestUsingStoredBinding(
        ZLinkStreamHeader requestHeader,
        byte[] payloadBytes,
        long sourceSessionSequence) {
        Message payloadPart = Message.from(payloadBytes);
        CompletionStage<List<Message>> request;
        try {
            request = stream.requestBoundActor(
                sessionRid,
                ref.actorId(),
                sourceSessionSequence,
                requestHeader,
                List.of(payloadPart),
                ZLinkSessionActorsRuntime.RELAY_SUBMIT_TIMEOUT);
        } catch (RuntimeException failure) {
            payloadPart.close();
            return CompletableFuture.failedFuture(failure);
        }
        return request.thenCompose(reply -> replyRemote(requestHeader, reply))
            .whenComplete((ignored, failure) -> payloadPart.close());
    }

    private CompletionStage<Void> replyRemote(
        ZLinkStreamHeader requestHeader,
        List<Message> reply) {
        Message body = null;
        try {
            if (reply == null || reply.size() != 1) {
                throw new IllegalArgumentException(
                    "bound Actor request reply requires one STREAM frame");
            }
            ZLinkStreamFrameCodec.DecodedFrame decoded =
                ZLinkStreamFrameCodec.tryDecode(
                        reply.getFirst().toByteArray())
                    .orElseThrow(() -> new IllegalArgumentException(
                        "bound Actor request reply is not a STREAM frame"));
            ZLinkStreamHeader remoteHeader =
                ZLinkStreamHeaderCodec.decodeOrPlain(decoded.header());
            //  Flow fields are observation-only (spec 27 §2/§7): a reply from
            //  an Off peer legitimately carries none, so the fence compares
            //  them only when both sides carry a pair.
            if ((remoteHeader.kind() != ZLinkStreamMessageKind.RESPONSE
                    && remoteHeader.kind() != ZLinkStreamMessageKind.ERROR)
                || !remoteHeader.correlationId().equals(
                    requestHeader.correlationId())
                || (remoteHeader.flowId().isPresent()
                    && requestHeader.flowId().isPresent()
                    && (!remoteHeader.flowId().equals(requestHeader.flowId())
                        || !remoteHeader.flowOrigin().equals(
                            requestHeader.flowOrigin())))) {
                throw new IllegalArgumentException(
                    "bound Actor request reply does not match its request");
            }
            ZLinkStreamHeader clientHeader = new ZLinkStreamHeader(
                remoteHeader.kind(),
                remoteHeader.codec(),
                remoteHeader.flags(),
                requestHeader.requestSequence(),
                "",
                remoteHeader.metadata(),
                requestHeader.correlationId(),
                requestHeader.flowId(),
                requestHeader.flowOrigin());
            body = Message.from(decoded.body());
            Message replyBody = body;
            return stream.replyAsync(
                    sessionRid,
                    clientHeader,
                    List.of(replyBody))
                .whenComplete((ignored, failure) -> replyBody.close());
        } catch (RuntimeException failure) {
            if (body != null) {
                body.close();
            }
            return CompletableFuture.failedFuture(failure);
        } finally {
            if (reply != null) {
                reply.forEach(Message::close);
            }
        }
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
