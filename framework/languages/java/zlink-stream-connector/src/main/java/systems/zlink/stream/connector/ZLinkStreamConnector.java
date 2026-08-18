package systems.zlink.stream.connector;

import java.util.Objects;

public interface ZLinkStreamConnector {
    boolean isConnected();

    ZLinkStreamConnectionState state();

    ZLinkStreamConnectorOptions options();

    /**
     * Current diagnostics level. Application can read this without
     * recreating the connector: server spec 26 §4.1 runtime-control
     * requirement, applied to the STREAM connector via common connector
     * spec §13. The value returned by {@link #options()}{@code
     * .diagnosticsLevel()} always agrees with this method.
     */
    ZLinkStreamDiagnosticsLevel diagnosticsLevel();

    /**
     * Atomically installs a new diagnostics level without recreating the
     * connector (server spec 26 §4.1). The change applies to processing
     * points that read the level after this call returns; frames already
     * built under the previous level are never retroactively changed, and
     * each processing point reads the level exactly once so a flip mid-way
     * through one send/receive never produces an inconsistent decision.
     */
    void setDiagnosticsLevel(ZLinkStreamDiagnosticsLevel level);

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
