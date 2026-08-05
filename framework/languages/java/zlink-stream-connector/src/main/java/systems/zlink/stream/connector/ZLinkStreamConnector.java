package systems.zlink.stream.connector;

import java.util.Objects;

public interface ZLinkStreamConnector {
    boolean isConnected();

    ZLinkStreamConnectionState state();

    ZLinkStreamConnectorOptions options();

    int pendingDispatchCount();

    int receivedCount(String name);

    ZLinkStreamLifecycleCall connect();

    ZLinkStreamLifecycleCall close();

    ZLinkStreamLifecycleCall dispatch();

    ZLinkStreamSendCall send(ZLinkStreamEncodedPayload payload);

    default ZLinkTypedStreamSendCall send(Object payload) {
        Objects.requireNonNull(payload, "payload");
        return new ZLinkTypedStreamConnectorSendCall(send(encodeTypedPayload(payload)));
    }

    ZLinkStreamRequestCall request(ZLinkStreamEncodedPayload payload);

    default ZLinkTypedStreamRequestCall request(Object payload) {
        Objects.requireNonNull(payload, "payload");
        return new ZLinkTypedStreamConnectorRequestCall(request(encodeTypedPayload(payload)));
    }

    AutoCloseable observeInbound(ZLinkStreamInboundObserver observer);

    default ZLinkStreamWaitCall waitFor(String name) {
        return new DefaultZLinkStreamWaitCall(
            this,
            name,
            options().waitTimeout(),
            options().typedCodec());
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
            this,
            name,
            options().waitTimeout(),
            options().typedCodec());
    }

    default ZLinkStreamSequenceCall waitForSequence(Class<?> payloadType) {
        Objects.requireNonNull(payloadType, "payloadType");
        return waitForSequence(options().nameResolver().resolve(payloadType));
    }

    AutoCloseable on(
        String name,
        ZLinkStreamMessageHandler<ZLinkStreamEncodedPayload> handler);

    default <TPayload> AutoCloseable on(
        Class<TPayload> payloadType,
        ZLinkStreamMessageHandler<TPayload> handler) {
        return on(options().nameResolver().resolve(payloadType), payloadType, handler);
    }

    default <TPayload> AutoCloseable on(
        String name,
        Class<TPayload> payloadType,
        ZLinkStreamMessageHandler<TPayload> handler) {
        Objects.requireNonNull(payloadType, "payloadType");
        Objects.requireNonNull(handler, "handler");
        ZLinkStreamTypedCodec codec = requireTypedCodec();
        return on(name, message -> handler.handleAsync(new ZLinkStreamMessage<>(
            message.packetName(),
            codec.decode(message.payload(), payloadType),
            message.metadata(),
            message.flowId(),
            message.flowOrigin())));
    }

    AutoCloseable onErrorReceived(ZLinkStreamErrorHandler handler);

    AutoCloseable onDisconnected(ZLinkStreamDisconnectedHandler handler);

    AutoCloseable onConnectionStateChanged(ZLinkStreamConnectionStateHandler handler);

    private ZLinkStreamTypedCodec requireTypedCodec() {
        ZLinkStreamTypedCodec codec = options().typedCodec();
        if (codec == null) {
            throw new IllegalStateException(
                "typed stream payload API requires ZLinkStreamConnectorOptions.typedCodec");
        }
        return codec;
    }

    private ZLinkStreamEncodedPayload encodeTypedPayload(Object payload) {
        if (payload instanceof ZLinkStreamEncodedPayload) {
            throw new IllegalArgumentException(
                "raw encoded payload must use the ZLinkStreamEncodedPayload overload");
        }
        return requireTypedCodec().encode(
            options().nameResolver().resolve(payload.getClass()),
            payload);
    }

}
