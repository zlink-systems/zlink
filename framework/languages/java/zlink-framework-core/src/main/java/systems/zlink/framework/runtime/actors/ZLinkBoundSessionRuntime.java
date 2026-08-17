package systems.zlink.framework.runtime.actors;
import java.time.Duration;
import java.util.concurrent.atomic.AtomicBoolean;

import systems.zlink.framework.runtime.internal.calls.ZLinkOneWayCalls;

import systems.zlink.framework.runtime.internal.backend.ZLinkInternalSpotNode;

import systems.zlink.framework.runtime.internal.backend.*;

import java.util.EnumSet;
import java.util.List;
import java.util.Map;
import java.util.Optional;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.TimeoutException;
import java.util.function.Predicate;
import java.util.function.Consumer;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.framework.ZLinkMessageSerializer;
import systems.zlink.framework.actors.ZLinkActor;
import systems.zlink.framework.actors.ZLinkBoundSession;
import systems.zlink.framework.actors.ZLinkBoundSessionSendCall;
import systems.zlink.framework.errors.ZLinkConfigurationException;
import systems.zlink.framework.runtime.messaging.ZLinkPayloadEncoding;

import systems.zlink.framework.streams.ZLinkStreamCodec;
import systems.zlink.framework.runtime.internal.diagnostics.ZLinkFlowContext;
import systems.zlink.framework.runtime.streams.ZLinkStreamHeader;
import systems.zlink.framework.runtime.streams.ZLinkStreamHeaderFlag;
import systems.zlink.framework.streams.ZLinkStreamMessageKind;

final class ZLinkBoundSessionRuntime implements ZLinkBoundSession {
    private static final Duration DEFAULT_TIMEOUT = Duration.ofSeconds(30);
    static final String REMOTE_BOUND_SESSION_BIND_PACKET_NAME =
        "zlink.framework.actor.bound_session.bind";
    private final ZLinkBackendStreamSocket stream;
    private final ZLinkInternalSpotNode spotNode;
    private final RoutingId sessionRid;
    private final String actorId;
    private final ZLinkMessageSerializer serializer;
    private final ZLinkActorRuntime actorRuntime;
    private final ZLinkActor actor;
    private final ZLinkStreamCodec defaultCodec;
    private final Predicate<RoutingId> routeReady;
    private final ZLinkRelayMetadataPolicy metadataPolicy;
    private long bindingToken;
    private Runnable unbindListener = () -> {};
    private Consumer<ZLinkBackendActorRef> rebindListener = ignored -> {};

    ZLinkBoundSessionRuntime(
        ZLinkBackendStreamSocket stream,
        ZLinkInternalSpotNode spotNode,
        RoutingId sessionRid,
        String actorId,
        ZLinkMessageSerializer serializer,
        ZLinkActorRuntime actorRuntime,
        ZLinkActor actor,
        ZLinkStreamCodec defaultCodec,
        Predicate<RoutingId> routeReady,
        ZLinkRelayMetadataPolicy metadataPolicy) {
        this.stream = stream;
        this.spotNode = spotNode;
        this.sessionRid = sessionRid;
        this.actorId = actorId;
        this.serializer = serializer;
        this.actorRuntime = actorRuntime;
        this.actor = actor;
        this.defaultCodec = defaultCodec == null ? ZLinkStreamCodec.JSON : defaultCodec;
        this.routeReady = routeReady == null ? ignored -> true : routeReady;
        this.metadataPolicy =
            metadataPolicy == null ? ZLinkRelayMetadataPolicy.EMPTY : metadataPolicy;
    }

    void setBindingToken(long bindingToken) {
        this.bindingToken = bindingToken;
    }

    void setUnbindListener(Runnable unbindListener) {
        this.unbindListener = unbindListener == null ? () -> {} : unbindListener;
    }

    void setRebindListener(Consumer<ZLinkBackendActorRef> rebindListener) {
        this.rebindListener = rebindListener == null ? ignored -> {} : rebindListener;
    }

    void markNativeRebound(ZLinkBackendActorRef targetActor) {
        rebindListener.accept(targetActor);
    }

    CompletionStage<Void> rebindNativeActor(
        ZLinkBackendActorRef targetActor,
        Duration timeout) {
        if (!actorId.equals(targetActor.actorId())) {
            return CompletableFuture.failedFuture(new ZLinkConfigurationException(
                "bound session actor id mismatch: " + actorId));
        }
        ZLinkBackendActorRef sourceActor = actorRuntime.refFor(actor);
        if (sourceActor.generation() != targetActor.generation()) {
            return CompletableFuture.failedFuture(new ZLinkConfigurationException(
                "relocation cannot rebind bound session actor " + actorId
                    + " from generation " + sourceActor.generation()
                    + " to " + targetActor.generation()
                    + "; a new actor incarnation requires an explicit bind"));
        }
        ZLinkStreamHeader header = new ZLinkStreamHeader(
            ZLinkStreamMessageKind.SEND,
            ZLinkStreamCodec.RAW,
            EnumSet.noneOf(ZLinkStreamHeaderFlag.class),
            Optional.empty(),
            REMOTE_BOUND_SESSION_BIND_PACKET_NAME,
            Map.of());
        return ignoreMissingBinding(stream.unbindActor(sessionRid, actorId).submit(timeout))
            .thenCompose(unbound -> awaitRouteReady(targetActor, timeout))
            .thenCompose(ignored -> bindActorWithRetry(stream, sessionRid, targetActor, timeout))
            .thenCompose(ignored -> relayBoundSessionBind(header))
            .thenRun(() -> rebindListener.accept(targetActor));
    }

    private CompletionStage<Void> awaitRouteReady(
        ZLinkBackendActorRef targetActor,
        Duration timeout) {
        return ZLinkActorRetryScheduler.waitUntilRelay(
            timeout,
            () -> routeReady.test(targetActor.nodeRid()),
            () -> {},
            () -> new TimeoutException(
                "remote bound session route was not ready before timeout: "
                    + actorId));
    }

    private CompletionStage<Void> relayBoundSessionBind(
        ZLinkStreamHeader header) {
        Message body = Message.from(new byte[0]);
        CompletionStage<Void> submission;
        try {
            submission = stream.relayBoundActorAsync(
                sessionRid,
                actorId,
                header,
                List.of(body));
        } catch (RuntimeException failure) {
            body.close();
            return CompletableFuture.failedFuture(failure);
        }
        return submission.whenComplete((ignored, failure) -> body.close());
    }

    static CompletionStage<Void> bindActorWithRetry(
        ZLinkBackendStreamSocket stream,
        RoutingId sessionRid,
        ZLinkBackendActorRef targetActor,
        Duration timeout) {
        return ZLinkActorRetryScheduler.bindRelayUntilAccepted(
            timeout,
            () -> stream.bindActor(sessionRid, targetActor)
                .submit(Duration.ofSeconds(2)),
            ignored -> false,
            ZLinkActorSubmitFaults::retryableBoundSessionBindFailure);
    }

    static CompletionStage<Void> ignoreMissingBinding(CompletionStage<Void> stage) {
        return stage.handle((ignored, error) -> {
            if (error == null || ZLinkActorSubmitFaults.requestNotFound(error)) {
                return CompletableFuture.<Void>completedFuture(null);
            }
            return CompletableFuture.<Void>failedFuture(error);
        }).thenCompose(result -> result);
    }

    @Override
    public ZLinkBoundSessionSendCall send(Object message) {
        ZLinkBoundSessionSendOptions options =
            ZLinkBoundSessionSendOptions.createForPayload(
                serializer,
                message,
                ZLinkPayloadEncoding.resolvePacketName(message),
                defaultCodec);
        ZLinkPayloadEncoding.EncodedPayload encoded =
            ZLinkPayloadEncoding.encode(serializer, message);
        return new SendCall(
            stream,
            sessionRid,
            actorId,
            actorRuntime,
            encoded.payload(),
            options,
            metadataPolicy);
    }

    @Override
    public CompletionStage<Void> disconnect() {
        return stream.unbindActor(sessionRid, actorId)
            .submit(DEFAULT_TIMEOUT)
            .thenRun(() -> {
                actorRuntime.clearSessionBinding(actor, bindingToken);
                unbindListener.run();
            });
    }

    private record SendCall(
        ZLinkBackendStreamSocket stream,
        RoutingId sessionRid,
        String actorId,
        ZLinkActorRuntime actorRuntime,
        Message payload,
        ZLinkBoundSessionSendOptions options,
        ZLinkRelayMetadataPolicy metadataPolicy,
        AtomicBoolean submitGate)
        implements ZLinkBoundSessionSendCall {
        SendCall(
            ZLinkBackendStreamSocket stream,
            RoutingId sessionRid,
            String actorId,
            ZLinkActorRuntime actorRuntime,
            Message payload,
            ZLinkBoundSessionSendOptions options,
            ZLinkRelayMetadataPolicy metadataPolicy) {
            this(stream, sessionRid, actorId, actorRuntime, payload, options, metadataPolicy,
                new AtomicBoolean());
        }
        public ZLinkBoundSessionSendCall packetName(String packetName) {
            return new SendCall(
                stream,
                sessionRid,
                actorId,
                actorRuntime,
                payload,
                options.withPacketName(packetName),
                metadataPolicy,
                submitGate);
        }

        @Override
        public ZLinkBoundSessionSendCall metadata(String key, String value) {
            return new SendCall(
                stream,
                sessionRid,
                actorId,
                actorRuntime,
                payload,
                options.withMetadata(key, value),
                metadataPolicy,
                submitGate);
        }

        @Override
        public CompletionStage<Void> submit() {
            CompletionStage<Void> duplicate =
                ZLinkOneWayCalls.beginOneWay(submitGate);
            if (duplicate != null) {
                return duplicate;
            }
            try (ZLinkFlowContext.Scope flowScope = actorRuntime != null
                ? actorRuntime.enterApplicationFlow()
                : null) {
            ZLinkStreamHeader header = metadataPolicy.actorToSession(options).header();
            Message payloadPart;
            try {
                payloadPart = Message.from(payload);
            } finally {
                payload.close();
            }
            CompletionStage<Void> submission;
            try {
                submission = stream.sendAsync(
                    sessionRid,
                    header,
                    List.of(payloadPart));
            } catch (RuntimeException failure) {
                payloadPart.close();
                return CompletableFuture.failedFuture(failure);
            }
            return ZLinkOneWayCalls.adaptOneWay(submission)
                .whenComplete((ignored, failure) -> payloadPart.close());
            }
        }

    }

}
