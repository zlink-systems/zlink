package systems.zlink.framework.runtime.actors;

import systems.zlink.framework.runtime.internal.calls.ZLinkOneWayCalls;

import systems.zlink.framework.runtime.internal.backend.ZLinkInternalSpotNode;

import systems.zlink.framework.runtime.internal.backend.*;

import java.time.Duration;
import java.util.List;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
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

final class ZLinkNativeBoundSessionRuntime implements ZLinkBoundSession {
    private final ZLinkInternalSpotNode spotNode;
    private ZLinkBackendActorRef actorRef;
    private final ZLinkMessageSerializer serializer;
    private final ZLinkActorRuntime actorRuntime;
    private final ZLinkActor actor;
    private final RoutingId sourceNodeRid;
    private final RoutingId sourceSessionRid;
    private final Duration timeout;
    private final ZLinkStreamCodec defaultCodec;
    private final ZLinkRelayMetadataPolicy metadataPolicy;
    private long bindingToken;

    ZLinkNativeBoundSessionRuntime(
        ZLinkInternalSpotNode spotNode,
        ZLinkBackendActorRef actorRef,
        ZLinkMessageSerializer serializer,
        ZLinkActorRuntime actorRuntime,
        ZLinkActor actor,
        RoutingId sourceNodeRid,
        RoutingId sourceSessionRid,
        Duration timeout,
        ZLinkStreamCodec defaultCodec,
        ZLinkRelayMetadataPolicy metadataPolicy) {
        this.spotNode = spotNode;
        this.actorRef = actorRef;
        this.serializer = serializer;
        this.actorRuntime = actorRuntime;
        this.actor = actor;
        this.sourceNodeRid = sourceNodeRid;
        this.sourceSessionRid = sourceSessionRid;
        this.timeout = timeout;
        this.defaultCodec = defaultCodec == null ? ZLinkStreamCodec.JSON : defaultCodec;
        this.metadataPolicy =
            metadataPolicy == null ? ZLinkRelayMetadataPolicy.EMPTY : metadataPolicy;
    }

    void setBindingToken(long bindingToken) {
        this.bindingToken = bindingToken;
    }

    void updateActorRef(ZLinkBackendActorRef actorRef) {
        this.actorRef = actorRef;
    }

    @Override
    public ZLinkBoundSessionSendCall send(Object message) {
        ZLinkPayloadEncoding.EncodedPayload encoded =
            ZLinkPayloadEncoding.encode(serializer, message);
        return new SendCall(
            spotNode,
            actorRuntime,
            actor,
            sourceNodeRid,
            sourceSessionRid,
            encoded.payload(),
            timeout,
            ZLinkBoundSessionSendOptions.create(encoded.packetName(), defaultCodec),
            metadataPolicy);
    }

    CompletionStage<Void> sendFrame(byte[] frameBytes) {
        ZLinkBackendActorRef currentActorRef = currentActorRef();
        Message frame = Message.from(frameBytes);
        return actorRuntime.oneWayCalls().submitOneWay(
            spotNode,
            ZLinkBackendAdmissionKey.boundSession(
                currentActorRef.nodeRid(),
                currentActorRef.actorId(),
                currentActorRef.generation()),
            () -> spotNode.sendActorBoundSession(
                currentActorRef, List.of(frame), SendFlags.DONT_WAIT),
            frame::close).thenApply(ignored -> null);
    }

    @Override
    public CompletionStage<Void> disconnect() {
        return CompletableFuture.runAsync(() -> spotNode.closeActorBoundSession(currentActorRef(), timeout))
            .thenRun(() -> actorRuntime.clearSessionBinding(actor, bindingToken));
    }

    private ZLinkBackendActorRef currentActorRef() {
        return actorRuntime.actorRef(actor);
    }

    private record SendCall(
        ZLinkInternalSpotNode spotNode,
        ZLinkActorRuntime actorRuntime,
        ZLinkActor actor,
        RoutingId sourceNodeRid,
        RoutingId sourceSessionRid,
        Message payload,
        Duration timeout,
        ZLinkBoundSessionSendOptions options,
        ZLinkRelayMetadataPolicy metadataPolicy,
        java.util.concurrent.atomic.AtomicBoolean submitGate)
        implements ZLinkBoundSessionSendCall {
        SendCall(
            ZLinkInternalSpotNode spotNode,
            ZLinkActorRuntime actorRuntime,
            ZLinkActor actor,
            RoutingId sourceNodeRid,
            RoutingId sourceSessionRid,
            Message payload,
            Duration timeout,
            ZLinkBoundSessionSendOptions options,
            ZLinkRelayMetadataPolicy metadataPolicy) {
            this(spotNode, actorRuntime, actor, sourceNodeRid, sourceSessionRid, payload,
                timeout, options, metadataPolicy,
                new java.util.concurrent.atomic.AtomicBoolean());
        }
        public ZLinkBoundSessionSendCall packetName(String packetName) {
            return new SendCall(
                spotNode,
                actorRuntime,
                actor,
                sourceNodeRid,
                sourceSessionRid,
                payload,
                timeout,
                options.withPacketName(packetName),
                metadataPolicy);
        }

        @Override
        public ZLinkBoundSessionSendCall metadata(String key, String value) {
            return new SendCall(
                spotNode,
                actorRuntime,
                actor,
                sourceNodeRid,
                sourceSessionRid,
                payload,
                timeout,
                options.withMetadata(key, value),
                metadataPolicy);
        }

        @Override
        public CompletionStage<Void> submit() {
            CompletionStage<Void> duplicate =
                ZLinkOneWayCalls.beginOneWay(submitGate);
            if (duplicate != null) {
                return duplicate;
            }
            ZLinkBackendActorRef currentActorRef = actorRuntime.actorRef(actor);
            byte[] frameBytes;
            try {
                frameBytes = metadataPolicy.actorToSession(options).encodeFrame(payload);
            } finally {
                payload.close();
            }
            Message frame = Message.from(frameBytes);
            return actorRuntime.oneWayCalls().submitOneWay(
                spotNode,
                ZLinkBackendAdmissionKey.boundSession(
                    currentActorRef.nodeRid(),
                    currentActorRef.actorId(),
                    currentActorRef.generation()),
                () -> spotNode.sendActorBoundSession(
                    currentActorRef, List.of(frame), SendFlags.DONT_WAIT),
                frame::close);
        }

    }

}
