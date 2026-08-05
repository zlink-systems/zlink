package systems.zlink.framework.runtime.actors;

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
import systems.zlink.contracts.sockets.SendFlags;
import systems.zlink.framework.ZLinkMessageSerializer;
import systems.zlink.framework.actors.ZLinkActor;
import systems.zlink.framework.actors.ZLinkBoundSession;
import systems.zlink.framework.actors.ZLinkBoundSessionSendCall;
import systems.zlink.framework.errors.ZLinkConfigurationException;
import systems.zlink.framework.runtime.messaging.ZLinkPayloadEncoding;

import systems.zlink.framework.streams.ZLinkStreamCodec;
import systems.zlink.framework.runtime.streams.ZLinkStreamHeader;
import systems.zlink.framework.runtime.streams.ZLinkStreamHeaderFlag;
import systems.zlink.framework.streams.ZLinkStreamMessageKind;

final class ZLinkBoundSessionRuntime implements ZLinkBoundSession {
    private static final java.time.Duration DEFAULT_TIMEOUT = java.time.Duration.ofSeconds(30);
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
        java.time.Duration timeout) {
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
            .thenCompose(ignored -> relayBoundSessionBindWithRetry(header, timeout))
            .thenRun(() -> rebindListener.accept(targetActor));
    }

    private CompletionStage<Void> awaitRouteReady(
        ZLinkBackendActorRef targetActor,
        java.time.Duration timeout) {
        return ZLinkActorRetryScheduler.waitUntilRelay(
            timeout,
            () -> routeReady.test(targetActor.nodeRid()),
            () -> {},
            () -> new TimeoutException(
                "remote bound session route was not ready before timeout: "
                    + actorId));
    }

    private CompletionStage<Void> relayBoundSessionBindWithRetry(
        ZLinkStreamHeader header,
        java.time.Duration timeout) {
        return ZLinkActorRetryScheduler.submitRelayUntilAccepted(
            timeout,
            () -> {
                try (Message body = Message.from(new byte[0])) {
                    return stream.relayBoundActor(
                        sessionRid,
                        actorId,
                        header,
                        List.of(body),
                        SendFlags.DONT_WAIT);
                }
            },
            () -> new TimeoutException(
                "remote bound session bind relay was not ready before timeout: "
                    + actorId));
    }

    static CompletionStage<Void> bindActorWithRetry(
        ZLinkBackendStreamSocket stream,
        RoutingId sessionRid,
        ZLinkBackendActorRef targetActor,
        java.time.Duration timeout) {
        return ZLinkActorRetryScheduler.bindRelayUntilAccepted(
            timeout,
            () -> stream.bindActor(sessionRid, targetActor)
                .submit(java.time.Duration.ofSeconds(2)),
            ZLinkActorSubmitFaults::alreadyBound,
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
        ZLinkPayloadEncoding.EncodedPayload encoded =
            ZLinkPayloadEncoding.encode(serializer, message);
        return new SendCall(
            stream,
            sessionRid,
            actorId,
            encoded.payload(),
            ZLinkBoundSessionSendOptions.create(encoded.packetName(), defaultCodec),
            metadataPolicy,
            actorRuntime.oneWayCalls());
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
        Message payload,
        ZLinkBoundSessionSendOptions options,
        ZLinkRelayMetadataPolicy metadataPolicy,
        ZLinkOneWayCalls oneWayCalls,
        java.util.concurrent.atomic.AtomicBoolean submitGate)
        implements ZLinkBoundSessionSendCall {
        SendCall(
            ZLinkBackendStreamSocket stream,
            RoutingId sessionRid,
            String actorId,
            Message payload,
            ZLinkBoundSessionSendOptions options,
            ZLinkRelayMetadataPolicy metadataPolicy,
            ZLinkOneWayCalls oneWayCalls) {
            this(stream, sessionRid, actorId, payload, options, metadataPolicy, oneWayCalls,
                new java.util.concurrent.atomic.AtomicBoolean());
        }
        public ZLinkBoundSessionSendCall packetName(String packetName) {
            return new SendCall(
                stream,
                sessionRid,
                actorId,
                payload,
                options.withPacketName(packetName),
                metadataPolicy,
                oneWayCalls);
        }

        @Override
        public ZLinkBoundSessionSendCall metadata(String key, String value) {
            return new SendCall(
                stream,
                sessionRid,
                actorId,
                payload,
                options.withMetadata(key, value),
                metadataPolicy,
                oneWayCalls);
        }

        @Override
        public CompletionStage<Void> submit() {
            CompletionStage<Void> duplicate =
                ZLinkOneWayCalls.beginOneWay(submitGate);
            if (duplicate != null) {
                return duplicate;
            }
            byte[] payloadBytes;
            try {
                payloadBytes = payload.toByteArray();
            } finally {
                payload.close();
            }
            ZLinkStreamHeader header = metadataPolicy.actorToSession(options).header();
            Message payloadPart = Message.from(payloadBytes);
            return oneWayCalls.submitOneWay(
                stream,
                ZLinkBackendAdmissionKey.socket(),
                () -> stream.send(
                    sessionRid,
                    header,
                    List.of(payloadPart),
                    SendFlags.DONT_WAIT),
                payloadPart::close);
        }

    }

}
