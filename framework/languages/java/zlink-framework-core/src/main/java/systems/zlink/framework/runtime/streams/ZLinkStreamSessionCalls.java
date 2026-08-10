package systems.zlink.framework.runtime.streams;
import java.util.concurrent.atomic.AtomicBoolean;

import systems.zlink.framework.runtime.internal.calls.ZLinkOneWayCalls;

import java.util.HashMap;
import java.util.List;
import java.util.Map;
import java.util.Optional;
import java.time.Duration;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.contracts.sockets.SendFlags;
import systems.zlink.framework.ZLinkMessageSerializer;
import systems.zlink.framework.errors.ZLinkConfigurationException;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendStreamSocket;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendAdmissionKey;
import systems.zlink.framework.runtime.messaging.ZLinkPayloadEncoding;

import systems.zlink.framework.streams.ZLinkSessionClient;
import systems.zlink.framework.streams.ZLinkSessionReplyCall;
import systems.zlink.framework.streams.ZLinkSessionSendCall;
import systems.zlink.framework.streams.ZLinkStreamCodec;
import systems.zlink.framework.streams.ZLinkStreamCompressionCodec;
import systems.zlink.framework.streams.ZLinkStreamMessageKind;

final class ZLinkStreamSessionClient implements ZLinkSessionClient {
    private final ZLinkBackendStreamSocket stream;
    private final RoutingId routingId;
    private final ZLinkStreamSessionContextState context;
    private final ZLinkMessageSerializer serializer;
    private final ZLinkStreamCodec defaultCodec;
    private final ZLinkStreamCompressionCodec compressionCodec;
    private final ZLinkOneWayCalls oneWayCalls;

    ZLinkStreamSessionClient(
        ZLinkBackendStreamSocket stream,
        RoutingId routingId,
        ZLinkStreamSessionContextState context,
        ZLinkMessageSerializer serializer,
        ZLinkStreamCodec defaultCodec,
        ZLinkStreamCompressionCodec compressionCodec,
        ZLinkOneWayCalls oneWayCalls) {
        this.stream = stream;
        this.routingId = routingId;
        this.context = context;
        this.serializer = serializer;
        this.defaultCodec = defaultCodec;
        this.compressionCodec = compressionCodec;
        this.oneWayCalls = oneWayCalls;
    }

    @Override
    public ZLinkSessionSendCall send(Object message) {
        ZLinkStreamCodec codec = ZLinkPayloadEncoding.streamCodec(
            serializer, message, defaultCodec);
        ZLinkPayloadEncoding.EncodedPayload encoded =
            ZLinkPayloadEncoding.encode(serializer, message);
        return new ZLinkStreamSessionSendCall(
            stream,
            routingId,
            encoded.payload(),
            encoded.packetName(),
            Map.of(),
            false,
            codec,
            compressionCodec,
            oneWayCalls);
    }

    @Override
    public ZLinkSessionReplyCall reply(Object message) {
        ZLinkStreamCodec codec = ZLinkPayloadEncoding.streamCodec(
            serializer, message, defaultCodec);
        ZLinkPayloadEncoding.EncodedPayload encoded =
            ZLinkPayloadEncoding.encode(serializer, message);
        return new ZLinkStreamSessionReplyCall(
            stream,
            routingId,
            encoded.payload(),
            context,
            encoded.packetName(),
            false,
            codec,
            compressionCodec);
    }
}

record ZLinkStreamSessionSendCall(
    ZLinkBackendStreamSocket stream,
    RoutingId routingId,
    Message payload,
    String packetName,
    Map<String, String> metadata,
    boolean compressed,
    ZLinkStreamCodec codec,
    ZLinkStreamCompressionCodec compressionCodec,
    ZLinkOneWayCalls oneWayCalls,
    Duration timeout,
    AtomicBoolean submitGate)
    implements ZLinkSessionSendCall {
    ZLinkStreamSessionSendCall(
        ZLinkBackendStreamSocket stream,
        RoutingId routingId,
        Message payload,
        String packetName,
        Map<String, String> metadata,
        boolean compressed,
        ZLinkStreamCodec codec,
        ZLinkStreamCompressionCodec compressionCodec,
        ZLinkOneWayCalls oneWayCalls) {
        this(stream, routingId, payload, packetName, metadata, compressed, codec,
            compressionCodec, oneWayCalls, null,
            new AtomicBoolean());
    }
    @Override
    public ZLinkSessionSendCall metadata(String key, String value) {
        Map<String, String> next = new HashMap<>(metadata);
        next.put(key, value);
        return new ZLinkStreamSessionSendCall(
            stream,
            routingId,
            payload,
            packetName,
            Map.copyOf(next),
            compressed,
            codec,
            compressionCodec,
            oneWayCalls,
            timeout,
            submitGate);
    }

    public ZLinkSessionSendCall packetName(String messageName) {
        if (messageName == null || messageName.isBlank()) {
            throw new IllegalArgumentException("messageName is required");
        }
        return new ZLinkStreamSessionSendCall(
            stream,
            routingId,
            payload,
            messageName,
            metadata,
            compressed,
            codec,
            compressionCodec,
            oneWayCalls,
            timeout,
            submitGate);
    }

    @Override
    public ZLinkSessionSendCall compress() {
        return new ZLinkStreamSessionSendCall(
            stream,
            routingId,
            payload,
            packetName,
            metadata,
            true,
            codec,
            compressionCodec,
            oneWayCalls,
            timeout,
            submitGate);
    }

    @Override
    public ZLinkSessionSendCall timeout(Duration value) {
        if (value == null || value.isZero() || value.isNegative()) {
            throw new IllegalArgumentException("timeout must be positive");
        }
        return new ZLinkStreamSessionSendCall(
            stream,
            routingId,
            payload,
            packetName,
            metadata,
            compressed,
            codec,
            compressionCodec,
            oneWayCalls,
            value,
            submitGate);
    }

    @Override
    public CompletionStage<Void> submit() {
        CompletionStage<Void> duplicate =
            ZLinkOneWayCalls.beginOneWay(submitGate);
        if (duplicate != null) {
            return duplicate;
        }
        ZLinkStreamPayloadCodec.Encoded encoded = ZLinkStreamPayloadCodec.encode(
            payload,
            compressed,
            compressionCodec);
        ZLinkStreamHeader header = new ZLinkStreamHeader(
            ZLinkStreamMessageKind.SEND,
            codec,
            encoded.flags(),
            Optional.empty(),
            packetName,
            metadata,
            Optional.of(ZLinkStreamCorrelation.next()));
        List<Message> parts = List.of(Message.from(encoded.payload()));
        return oneWayCalls.submitOneWay(
            stream,
            ZLinkBackendAdmissionKey.socket(),
            () -> stream.send(routingId, header, parts, SendFlags.DONT_WAIT),
            () -> {
                parts.forEach(Message::close);
                payload.close();
            },
            timeout);
    }
}

record ZLinkStreamSessionReplyCall(
    ZLinkBackendStreamSocket stream,
    RoutingId routingId,
    Message payload,
    ZLinkStreamSessionContextState context,
    String packetName,
    boolean compressed,
    ZLinkStreamCodec codec,
    ZLinkStreamCompressionCodec compressionCodec,
    AtomicBoolean submitGate)
    implements ZLinkSessionReplyCall {
    ZLinkStreamSessionReplyCall(
        ZLinkBackendStreamSocket stream,
        RoutingId routingId,
        Message payload,
        ZLinkStreamSessionContextState context,
        String packetName,
        boolean compressed,
        ZLinkStreamCodec codec,
        ZLinkStreamCompressionCodec compressionCodec) {
        this(stream, routingId, payload, context, packetName, compressed, codec, compressionCodec,
            new AtomicBoolean());
    }
    @Override
    public ZLinkSessionReplyCall compress() {
        return new ZLinkStreamSessionReplyCall(
            stream,
            routingId,
            payload,
            context,
            packetName,
            true,
            codec,
            compressionCodec,
            submitGate);
    }

    @Override
    public CompletionStage<Void> submit() {
        Optional<ZLinkStreamHeader> currentHeader = context.currentDispatchHeader();
        if (currentHeader.isEmpty() || currentHeader.get().requestSequence().isEmpty()) {
            CompletionStage<Void> duplicate =
                ZLinkOneWayCalls.beginOneWay(submitGate);
            if (duplicate != null) {
                return duplicate;
            }
            payload.close();
            return CompletableFuture.failedFuture(new IllegalStateException(
                "Reply is only available while handling a request packet."));
        }
        CompletionStage<Void> duplicate =
            ZLinkOneWayCalls.beginOneWay(submitGate);
        if (duplicate != null) {
            return duplicate;
        }
        if (!context.claimReplyHeader(currentHeader.get())) {
            payload.close();
            return CompletableFuture.failedFuture(
                new IllegalStateException("The request reply token has already been used."));
        }
        ZLinkStreamPayloadCodec.Encoded encoded = ZLinkStreamPayloadCodec.encode(
            payload,
            compressed,
            compressionCodec);
        List<Message> parts = List.of(Message.from(encoded.payload()));
        ZLinkStreamHeader current = currentHeader.get();
        ZLinkStreamHeader replyHeader = ZLinkStreamHeader.createResponse(
            current,
            codec,
            encoded.flags(),
            packetName,
            Map.of());
        return context.oneWayCalls().submitOneWay(
            stream,
            ZLinkBackendAdmissionKey.socket(),
            () -> {
                boolean accepted = stream.reply(
                    routingId, replyHeader, parts, SendFlags.DONT_WAIT);
                if (accepted) {
                    context.traceStreamReplied(current);
                }
                return accepted;
            },
            () -> {
                parts.forEach(Message::close);
                payload.close();
            });
    }
}
