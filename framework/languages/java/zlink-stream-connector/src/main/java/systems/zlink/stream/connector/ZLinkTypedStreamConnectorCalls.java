package systems.zlink.stream.connector;

import java.time.Duration;
import java.util.HashMap;
import java.util.Map;
import java.util.Objects;
import java.util.concurrent.CompletionStage;

record ZLinkTypedStreamConnectorSendCall(
        ZLinkStreamConnector connector,
        ZLinkStreamActor actor,
        Object payload,
        String name,
        Map<String, String> metadata,
        boolean compressed)
        implements ZLinkTypedStreamSendCall {
    ZLinkTypedStreamConnectorSendCall(
            ZLinkStreamConnector connector, ZLinkStreamActor actor, Object payload, String name) {
        this(connector, actor, payload, name, Map.of(), false);
    }

    ZLinkTypedStreamConnectorSendCall {
        Objects.requireNonNull(connector, "connector");
        Objects.requireNonNull(payload, "payload");
        if (payload instanceof ZLinkStreamEncodedPayload) {
            throw ZLinkStreamException.validationFailed(
                    "raw encoded payload must use the ZLinkStreamEncodedPayload overload");
        }
    }

    @Override
    public ZLinkTypedStreamSendCall packetName(String name) {
        return new ZLinkTypedStreamConnectorSendCall(
                connector,
                actor,
                payload,
                DefaultZLinkStreamConnector.validatePacketName(name),
                metadata,
                compressed);
    }

    @Override
    public ZLinkTypedStreamSendCall metadata(String key, String value) {
        Map<String, String> updated = new HashMap<>(metadata);
        updated.put(key, value);
        return metadata(updated);
    }

    @Override
    public ZLinkTypedStreamSendCall metadata(Map<String, String> metadata) {
        return new ZLinkTypedStreamConnectorSendCall(
                connector, actor, payload, name, Map.copyOf(metadata), compressed);
    }

    @Override
    public ZLinkTypedStreamSendCall compress() {
        return new ZLinkTypedStreamConnectorSendCall(
                connector, actor, payload, name, metadata, true);
    }

    @Override
    public CompletionStage<Void> submit() {
        ZLinkStreamEncodedPayload encoded =
                ZLinkStreamCallPayload.encodeTyped(connector, payload, name);
        ZLinkStreamSendCall call = actor == null ? connector.send(encoded) : actor.send(encoded);
        if (!metadata.isEmpty()) {
            call = call.metadata(metadata);
        }
        return (compressed ? call.compress() : call).submit();
    }
}

record ZLinkTypedStreamConnectorRequestCall(
        ZLinkStreamConnector connector,
        ZLinkStreamActor actor,
        Object payload,
        String name,
        Map<String, String> metadata,
        boolean compressed,
        Duration timeout)
        implements ZLinkTypedStreamRequestCall {
    ZLinkTypedStreamConnectorRequestCall(
            ZLinkStreamConnector connector, ZLinkStreamActor actor, Object payload, String name) {
        this(connector, actor, payload, name, Map.of(), false, null);
    }

    ZLinkTypedStreamConnectorRequestCall {
        Objects.requireNonNull(connector, "connector");
        Objects.requireNonNull(payload, "payload");
        if (payload instanceof ZLinkStreamEncodedPayload) {
            throw ZLinkStreamException.validationFailed(
                    "raw encoded payload must use the ZLinkStreamEncodedPayload overload");
        }
    }

    @Override
    public ZLinkTypedStreamRequestCall packetName(String name) {
        return new ZLinkTypedStreamConnectorRequestCall(
                connector,
                actor,
                payload,
                DefaultZLinkStreamConnector.validatePacketName(name),
                metadata,
                compressed,
                timeout);
    }

    @Override
    public ZLinkTypedStreamRequestCall metadata(String key, String value) {
        Map<String, String> updated = new HashMap<>(metadata);
        updated.put(key, value);
        return metadata(updated);
    }

    @Override
    public ZLinkTypedStreamRequestCall metadata(Map<String, String> metadata) {
        return new ZLinkTypedStreamConnectorRequestCall(
                connector, actor, payload, name, Map.copyOf(metadata), compressed, timeout);
    }

    @Override
    public ZLinkTypedStreamRequestCall timeout(Duration timeout) {
        if (timeout == null || timeout.isZero() || timeout.isNegative()) {
            throw ZLinkStreamException.validationFailed("timeout must be positive");
        }
        return new ZLinkTypedStreamConnectorRequestCall(
                connector, actor, payload, name, metadata, compressed, timeout);
    }

    @Override
    public ZLinkTypedStreamRequestCall compress() {
        return new ZLinkTypedStreamConnectorRequestCall(
                connector, actor, payload, name, metadata, true, timeout);
    }

    @Override
    public <TReply> CompletionStage<TReply> submit(Class<TReply> replyType) {
        ZLinkStreamEncodedPayload encoded =
                ZLinkStreamCallPayload.encodeTyped(connector, payload, name);
        ZLinkStreamRequestCall call =
                actor == null ? connector.request(encoded) : actor.request(encoded);
        if (!metadata.isEmpty()) {
            call = call.metadata(metadata);
        }
        if (compressed) {
            call = call.compress();
        }
        if (timeout != null) {
            call = call.timeout(timeout);
        }
        return call.submit(replyType);
    }
}
