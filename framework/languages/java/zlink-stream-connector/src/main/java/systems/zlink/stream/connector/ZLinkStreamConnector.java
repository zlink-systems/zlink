package systems.zlink.stream.connector;

import java.util.Objects;
import java.util.Optional;
import java.util.concurrent.CompletionStage;

public interface ZLinkStreamConnector {
    boolean isConnected();

    ZLinkStreamConnectionState state();

    ZLinkStreamConnectorOptions options();

    /**
     * The reason the connection last ended, or empty when it has never
     * ended (common connector spec 32 6.2). A failed first connect also
     * leaves a reason here, and reconnecting does not clear it. The
     * {@code closeReason()} an {@link ZLinkStreamDisconnected} event carries
     * adds to this surface rather than replacing it.
     */
    Optional<ZLinkStreamCloseReason> closeReason();

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
    /**
     * Changes the diagnostics level without waiting (common connector spec
     * 32 13). Changing a level is a single value write, so there is no
     * completion for a caller to wait on; implementing this on top of the
     * asynchronous pair would make a call inside a receive callback wait for
     * its own completion. Safe to call from a handler or callback.
     */
    void setDiagnosticsLevel(ZLinkStreamDiagnosticsLevel level);

    /**
     * The asynchronous pair of {@link #setDiagnosticsLevel}, returning the
     * same kind of terminal every other operation returns. It changes the
     * same value and does not replace the synchronous surface.
     */
    CompletionStage<Void> setDiagnosticsLevelAsync(
        ZLinkStreamDiagnosticsLevel level);

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
            //  A missing codec is a disagreement between the option set and
            //  the API being used, so spec 32 9 calls it ConfigurationError.
            throw new ZLinkStreamException(new ZLinkStreamError(
                ZLinkStreamErrorCode.CONFIGURATION_ERROR,
                "typed stream payload API requires ZLinkStreamConnectorOptions.typedCodec"));
        }
        return codec;
    }

    private ZLinkStreamEncodedPayload encodeTypedPayload(Object payload) {
        if (payload instanceof ZLinkStreamEncodedPayload) {
            throw new ZLinkStreamException(new ZLinkStreamError(
                ZLinkStreamErrorCode.VALIDATION_FAILED,
                "raw encoded payload must use the ZLinkStreamEncodedPayload overload"));
        }
        return requireTypedCodec().encode(
            options().nameResolver().resolve(payload.getClass()),
            payload);
    }

}
