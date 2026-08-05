package systems.zlink.stream.connector;

import java.time.Duration;
import java.util.HashMap;
import java.util.Map;
import java.util.Objects;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import systems.zlink.contracts.messaging.Message;

record ZLinkStreamConnectorSendCall(
    DefaultZLinkStreamConnector connector,
    ZLinkStreamEncodedPayload payload,
    boolean compressed) implements ZLinkStreamSendCall {
    @Override
    public ZLinkStreamSendCall packetName(String name) {
        return new ZLinkStreamConnectorSendCall(
            connector,
            ZLinkStreamCallPayload.withPacketName(payload, name),
            compressed);
    }

    @Override
    public ZLinkStreamSendCall metadata(String key, String value) {
        Map<String, String> metadata = new HashMap<>(payload.metadata());
        metadata.put(key, value);
        return metadata(metadata);
    }

    @Override
    public ZLinkStreamSendCall metadata(Map<String, String> metadata) {
        return new ZLinkStreamConnectorSendCall(connector, new ZLinkStreamEncodedPayload(
            payload.packetName(),
            payload.payload(),
            Map.copyOf(Objects.requireNonNull(metadata, "metadata")),
            payload.codec()), compressed);
    }

    @Override
    public ZLinkStreamSendCall compress() {
        return new ZLinkStreamConnectorSendCall(connector, payload, true);
    }

    @Override
    public CompletionStage<Void> submit() {
        return connector.submit(payload, compressed);
    }
}

record ZLinkStreamConnectorRequestCall(
    DefaultZLinkStreamConnector connector,
    ZLinkStreamEncodedPayload payload,
    Duration timeout,
    boolean compressed) implements ZLinkStreamRequestCall {
    @Override
    public ZLinkStreamRequestCall packetName(String name) {
        return new ZLinkStreamConnectorRequestCall(
            connector,
            ZLinkStreamCallPayload.withPacketName(payload, name),
            timeout,
            compressed);
    }

    @Override
    public ZLinkStreamRequestCall metadata(String key, String value) {
        Map<String, String> metadata = new HashMap<>(payload.metadata());
        metadata.put(key, value);
        return metadata(metadata);
    }

    @Override
    public ZLinkStreamRequestCall metadata(Map<String, String> metadata) {
        return new ZLinkStreamConnectorRequestCall(connector, new ZLinkStreamEncodedPayload(
            payload.packetName(),
            payload.payload(),
            Map.copyOf(Objects.requireNonNull(metadata, "metadata")),
            payload.codec()), timeout, compressed);
    }

    @Override
    public ZLinkStreamRequestCall compress() {
        return new ZLinkStreamConnectorRequestCall(connector, payload, timeout, true);
    }

    @Override
    public ZLinkStreamRequestCall timeout(Duration timeout) {
        requirePositive(timeout, "timeout");
        return new ZLinkStreamConnectorRequestCall(connector, payload, timeout, compressed);
    }

    @Override
    public CompletionStage<ZLinkStreamEncodedPayload> submit() {
        return connector.submitRequest(payload, timeout, compressed);
    }

    @Override
    public <TReply> CompletionStage<TReply> submit(Class<TReply> replyType) {
        Objects.requireNonNull(replyType, "replyType");
        ZLinkStreamTypedCodec codec = connector.options().typedCodec();
        if (codec == null) {
            throw new IllegalStateException("typed stream reply API requires ZLinkStreamConnectorOptions.typedCodec");
        }
        CompletionStage<ZLinkStreamEncodedPayload> source = submit();
        CompletableFuture<TReply> result = new CompletableFuture<>();
        source.whenComplete((reply, error) -> {
            if (error != null) {
                result.completeExceptionally(error);
                return;
            }
            try {
                result.complete(decodeReply(codec, replyType, reply));
            } catch (Throwable failure) {
                result.completeExceptionally(failure);
            } finally {
                reply.payload().close();
            }
        });
        result.whenComplete((ignored, error) -> {
            if (result.isCancelled()) {
                source.toCompletableFuture().cancel(false);
            }
        });
        return result;
    }

    private <TReply> TReply decodeReply(
        ZLinkStreamTypedCodec codec,
        Class<TReply> replyType,
        ZLinkStreamEncodedPayload reply) {
        try {
            return codec.decode(reply, replyType);
        } catch (RuntimeException ex) {
            throw new IllegalStateException(
                "failed to decode stream reply request="
                    + payload.packetName()
                    + " reply="
                    + reply.packetName()
                    + " as "
                    + replyType.getName()
                    + " payload="
                    + new String(reply.payload().toByteArray(), java.nio.charset.StandardCharsets.UTF_8),
                ex);
        }
    }

    private static void requirePositive(Duration value, String name) {
        Objects.requireNonNull(value, name);
        if (value.isZero() || value.isNegative()) {
            throw new IllegalArgumentException(name + " must be positive");
        }
    }

}

final class ZLinkStreamCallPayload {
    private ZLinkStreamCallPayload() { }

    static ZLinkStreamEncodedPayload withPacketName(
        ZLinkStreamEncodedPayload payload,
        String name) {
        return new ZLinkStreamEncodedPayload(
            DefaultZLinkStreamConnector.validatePacketName(name),
            payload.payload(),
            payload.metadata(),
            payload.codec());
    }
}
