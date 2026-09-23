package systems.zlink.stream.connector;

import java.util.List;
import java.util.Objects;
import java.util.Optional;

public interface ZLinkStreamConnector {
    boolean isConnected();

    ZLinkStreamConnectionState state();

    ZLinkStreamConnectorOptions options();

    /**
     * The reason the connection last ended, or empty when it has never ended (common connector spec
     * 32 6.2). A failed first connect also leaves a reason here, and reconnecting does not clear
     * it. The {@code closeReason()} an {@link ZLinkStreamDisconnected} event carries adds to this
     * surface rather than replacing it.
     */
    Optional<ZLinkStreamCloseReason> closeReason();

    int pendingDispatchCount();

    int receivedCount(String name);

    ZLinkStreamLifecycleCall connect();

    ZLinkStreamLifecycleCall close();

    ZLinkStreamLifecycleCall dispatch();

    ZLinkStreamSendCall send(ZLinkStreamEncodedPayload payload);

    default ZLinkTypedStreamSendCall send(Object payload) {
        return new ZLinkTypedStreamConnectorSendCall(this, null, payload, null);
    }

    default ZLinkTypedStreamSendCall send(String name, Object payload) {
        return new ZLinkTypedStreamConnectorSendCall(
                this, null, payload, DefaultZLinkStreamConnector.validatePacketName(name));
    }

    ZLinkStreamRequestCall request(ZLinkStreamEncodedPayload payload);

    default ZLinkTypedStreamRequestCall request(Object payload) {
        return new ZLinkTypedStreamConnectorRequestCall(this, null, payload, null);
    }

    default ZLinkTypedStreamRequestCall request(String name, Object payload) {
        return new ZLinkTypedStreamConnectorRequestCall(
                this, null, payload, DefaultZLinkStreamConnector.validatePacketName(name));
    }

    default ZLinkStreamWaitCall waitFor(String name) {
        return new DefaultZLinkStreamWaitCall(
                this, name, options().waitTimeout(), options().typedCodec());
    }

    default ZLinkStreamWaitCall waitFor(Class<?> payloadType) {
        Objects.requireNonNull(payloadType, "payloadType");
        return waitFor(options().nameResolver().resolve(payloadType));
    }

    default ZLinkStreamExpectNoneCall expectNone(String name) {
        return new DefaultZLinkStreamExpectNoneCall(this, name);
    }

    default ZLinkStreamExpectNoneCall expectNone(Class<?> payloadType) {
        Objects.requireNonNull(payloadType, "payloadType");
        return expectNone(options().nameResolver().resolve(payloadType));
    }

    default ZLinkStreamSequenceCall waitForSequence(String name) {
        return new DefaultZLinkStreamSequenceCall(
                this, name, options().waitTimeout(), options().typedCodec());
    }

    default ZLinkStreamSequenceCall waitForSequence(Class<?> payloadType) {
        Objects.requireNonNull(payloadType, "payloadType");
        return waitForSequence(options().nameResolver().resolve(payloadType));
    }

    AutoCloseable on(String name, ZLinkStreamMessageHandler<ZLinkStreamEncodedPayload> handler);

    default <TPayload> AutoCloseable on(
            Class<TPayload> payloadType, ZLinkStreamMessageHandler<TPayload> handler) {
        return on(options().nameResolver().resolve(payloadType), payloadType, handler);
    }

    default <TPayload> AutoCloseable on(
            String name, Class<TPayload> payloadType, ZLinkStreamMessageHandler<TPayload> handler) {
        Objects.requireNonNull(payloadType, "payloadType");
        Objects.requireNonNull(handler, "handler");
        ZLinkStreamTypedCodec codec = requireTypedCodec();
        return on(
                name,
                message ->
                        handler.handleAsync(
                                new ZLinkStreamMessage<>(
                                        message.packetName(),
                                        codec.decode(message.payload(), payloadType),
                                        message.metadata(),
                                        message.actorId())));
    }

    AutoCloseable onErrorReceived(ZLinkStreamErrorHandler handler);

    AutoCloseable onRequestSending(ZLinkStreamRequestSendingHandler handler);

    AutoCloseable onReplyReceived(ZLinkStreamReplyReceivedHandler handler);

    AutoCloseable onDisconnected(ZLinkStreamDisconnectedHandler handler);

    AutoCloseable onConnectionStateChanged(ZLinkStreamConnectionStateHandler handler);

    List<ZLinkStreamActor> actors();

    Optional<ZLinkStreamActor> actor(String actorId);

    AutoCloseable onActorBound(ZLinkStreamActorHandler handler);

    AutoCloseable onActorUnbound(ZLinkStreamActorHandler handler);

    private ZLinkStreamTypedCodec requireTypedCodec() {
        ZLinkStreamTypedCodec codec = options().typedCodec();
        if (codec == null) {
            //  A missing codec is a disagreement between the option set and
            //  the API being used, so spec 32 9 calls it ConfigurationError.
            throw new ZLinkStreamException(
                    new ZLinkStreamError(
                            ZLinkStreamErrorCode.CONFIGURATION_ERROR,
                            "typed stream payload API requires"
                                    + " ZLinkStreamConnectorOptions.typedCodec"));
        }
        return codec;
    }
}
