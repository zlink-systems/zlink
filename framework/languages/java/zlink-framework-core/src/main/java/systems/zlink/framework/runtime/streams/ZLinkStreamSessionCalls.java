package systems.zlink.framework.runtime.streams;

import systems.zlink.contracts.messaging.Message;
import systems.zlink.framework.ZLinkMessageSerializer;
import systems.zlink.framework.monitoring.ZLinkFlowOrigin;
import systems.zlink.framework.runtime.diagnostics.ZLinkMessageFlowTracer;
import systems.zlink.framework.runtime.internal.calls.ZLinkOneWayCalls;
import systems.zlink.framework.runtime.internal.diagnostics.ZLinkFlowContext;
import systems.zlink.framework.runtime.internal.service.ZLinkFrameworkMultipartView;
import systems.zlink.framework.runtime.messaging.ZLinkPayloadEncoding;
import systems.zlink.framework.streams.ZLinkSessionClient;
import systems.zlink.framework.streams.ZLinkSessionReplyCall;
import systems.zlink.framework.streams.ZLinkSessionSendCall;
import systems.zlink.framework.streams.ZLinkStreamCodec;
import systems.zlink.framework.streams.ZLinkStreamCompressionCodec;
import systems.zlink.framework.streams.ZLinkStreamMessageKind;

import java.util.HashMap;
import java.util.List;
import java.util.Map;
import java.util.Optional;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.atomic.AtomicBoolean;

final class ZLinkStreamSessionClient implements ZLinkSessionClient {
    private final ZLinkStreamSessionContextState context;
    private final ZLinkMessageSerializer serializer;
    private final ZLinkStreamCodec defaultCodec;
    private final ZLinkStreamCompressionCodec compressionCodec;
    private final ZLinkMessageFlowTracer flow;

    ZLinkStreamSessionClient(
            ZLinkStreamSessionContextState context,
            ZLinkMessageSerializer serializer,
            ZLinkStreamCodec defaultCodec,
            ZLinkStreamCompressionCodec compressionCodec,
            ZLinkMessageFlowTracer flow) {
        this.context = context;
        this.serializer = serializer;
        this.defaultCodec = defaultCodec;
        this.compressionCodec = compressionCodec;
        this.flow = flow;
    }

    @Override
    public ZLinkSessionSendCall send(Object message) {
        ZLinkStreamCodec codec =
                ZLinkPayloadEncoding.streamCodec(serializer, message, defaultCodec);
        ZLinkPayloadEncoding.EncodedPayload encoded =
                ZLinkPayloadEncoding.encode(serializer, message);
        return new ZLinkStreamSessionSendCall(
                context,
                encoded.payload(),
                encoded.packetName(),
                Map.of(),
                false,
                codec,
                compressionCodec,
                flow,
                new AtomicBoolean());
    }

    @Override
    public ZLinkSessionReplyCall reply(Object message) {
        ZLinkStreamCodec codec =
                ZLinkPayloadEncoding.streamCodec(serializer, message, defaultCodec);
        ZLinkPayloadEncoding.EncodedPayload encoded =
                ZLinkPayloadEncoding.encode(serializer, message);
        return new ZLinkStreamSessionReplyCall(
                encoded.payload(),
                context,
                encoded.packetName(),
                false,
                codec,
                compressionCodec,
                flow);
    }
}

record ZLinkStreamSessionSendCall(
        ZLinkStreamSessionContextState context,
        ZLinkFrameworkMultipartView payload,
        String packetName,
        Map<String, String> metadata,
        boolean compressed,
        ZLinkStreamCodec codec,
        ZLinkStreamCompressionCodec compressionCodec,
        ZLinkMessageFlowTracer flow,
        AtomicBoolean submitGate)
        implements ZLinkSessionSendCall {
    ZLinkStreamSessionSendCall(
            ZLinkStreamSessionContextState context,
            ZLinkFrameworkMultipartView payload,
            String packetName,
            Map<String, String> metadata,
            boolean compressed,
            ZLinkStreamCodec codec,
            ZLinkStreamCompressionCodec compressionCodec) {
        this(
                context,
                payload,
                packetName,
                metadata,
                compressed,
                codec,
                compressionCodec,
                null,
                new AtomicBoolean());
    }

    @Override
    public ZLinkSessionSendCall metadata(String key, String value) {
        Map<String, String> next = new HashMap<>(metadata);
        next.put(key, value);
        return new ZLinkStreamSessionSendCall(
                context,
                payload,
                packetName,
                Map.copyOf(next),
                compressed,
                codec,
                compressionCodec,
                flow,
                submitGate);
    }

    public ZLinkSessionSendCall packetName(String messageName) {
        if (messageName == null || messageName.isBlank()) {
            throw new IllegalArgumentException("messageName is required");
        }
        return new ZLinkStreamSessionSendCall(
                context,
                payload,
                messageName,
                metadata,
                compressed,
                codec,
                compressionCodec,
                flow,
                submitGate);
    }

    @Override
    public ZLinkSessionSendCall compress() {
        return new ZLinkStreamSessionSendCall(
                context,
                payload,
                packetName,
                metadata,
                true,
                codec,
                compressionCodec,
                flow,
                submitGate);
    }

    @Override
    public CompletionStage<Void> submit() {
        CompletionStage<Void> duplicate = ZLinkOneWayCalls.beginOneWay(submitGate);
        if (duplicate != null) {
            return duplicate;
        }
        try (var flowScope =
                ZLinkFlowContext.enterCurrentOrCreate(
                        ZLinkFlowOrigin.APPLICATION, flow != null && flow.captureEnabled())) {
            ZLinkStreamHeader header;
            List<Message> parts;
            try {
                ZLinkStreamPayloadCodec.Encoded encoded =
                        ZLinkStreamPayloadCodec.encode(
                                payload.getFirst(), compressed, compressionCodec);
                header =
                        new ZLinkStreamHeader(
                                ZLinkStreamMessageKind.SEND,
                                codec,
                                encoded.flags(),
                                Optional.empty(),
                                packetName,
                                metadata,
                                Optional.of(ZLinkStreamCorrelation.next()));
                parts = List.of(Message.from(encoded.payload()));
            } catch (RuntimeException | Error failure) {
                payload.close();
                throw failure;
            }
            CompletionStage<Void> result;
            try {
                result = context.sendAsync(header, parts);
                result.whenComplete(
                        (ignored, failure) ->
                                ZLinkStreamSessionCallResources.close(parts, payload));
            } catch (RuntimeException | Error failure) {
                ZLinkStreamSessionCallResources.close(parts, payload);
                throw failure;
            }
            return ZLinkOneWayCalls.adaptOneWay(result);
        }
    }
}

record ZLinkStreamSessionReplyCall(
        ZLinkFrameworkMultipartView payload,
        ZLinkStreamSessionContextState context,
        String packetName,
        boolean compressed,
        ZLinkStreamCodec codec,
        ZLinkStreamCompressionCodec compressionCodec,
        ZLinkMessageFlowTracer flow,
        AtomicBoolean submitGate)
        implements ZLinkSessionReplyCall {
    ZLinkStreamSessionReplyCall(
            ZLinkFrameworkMultipartView payload,
            ZLinkStreamSessionContextState context,
            String packetName,
            boolean compressed,
            ZLinkStreamCodec codec,
            ZLinkStreamCompressionCodec compressionCodec) {
        this(
                payload,
                context,
                packetName,
                compressed,
                codec,
                compressionCodec,
                null,
                new AtomicBoolean());
    }

    ZLinkStreamSessionReplyCall(
            ZLinkFrameworkMultipartView payload,
            ZLinkStreamSessionContextState context,
            String packetName,
            boolean compressed,
            ZLinkStreamCodec codec,
            ZLinkStreamCompressionCodec compressionCodec,
            ZLinkMessageFlowTracer flow) {
        this(
                payload,
                context,
                packetName,
                compressed,
                codec,
                compressionCodec,
                flow,
                new AtomicBoolean());
    }

    @Override
    public ZLinkSessionReplyCall compress() {
        return new ZLinkStreamSessionReplyCall(
                payload, context, packetName, true, codec, compressionCodec, flow, submitGate);
    }

    @Override
    public CompletionStage<Void> submit() {
        Optional<ZLinkStreamHeader> currentHeader = context.currentDispatchHeader();
        if (currentHeader.isEmpty() || currentHeader.get().requestSequence().isEmpty()) {
            CompletionStage<Void> duplicate = ZLinkOneWayCalls.beginOneWay(submitGate);
            if (duplicate != null) {
                return duplicate;
            }
            payload.close();
            return CompletableFuture.failedFuture(
                    new IllegalStateException(
                            "Reply is only available while handling a request packet."));
        }
        CompletionStage<Void> duplicate = ZLinkOneWayCalls.beginOneWay(submitGate);
        if (duplicate != null) {
            return duplicate;
        }
        try (var flowScope =
                ZLinkFlowContext.enterCurrentOrCreate(
                        ZLinkFlowOrigin.APPLICATION, flow != null && flow.captureEnabled())) {
            if (!context.claimReplyHeader(currentHeader.get())) {
                payload.close();
                return CompletableFuture.failedFuture(
                        new IllegalStateException(
                                "The request reply token has already been used."));
            }
            ZLinkStreamHeader current = currentHeader.get();
            ZLinkStreamHeader replyHeader;
            List<Message> parts;
            try {
                ZLinkStreamPayloadCodec.Encoded encoded =
                        ZLinkStreamPayloadCodec.encode(
                                payload.getFirst(), compressed, compressionCodec);
                replyHeader =
                        ZLinkStreamHeader.createResponse(
                                current, codec, encoded.flags(), packetName, Map.of());
                parts = List.of(Message.from(encoded.payload()));
            } catch (RuntimeException | Error failure) {
                payload.close();
                throw failure;
            }
            CompletionStage<Void> result;
            try {
                result = context.replyAsync(replyHeader, parts);
                result.whenComplete(
                        (ignored, failure) -> {
                            try {
                                if (failure == null) {
                                    context.traceStreamReplied(current);
                                }
                            } finally {
                                ZLinkStreamSessionCallResources.close(parts, payload);
                            }
                        });
            } catch (RuntimeException | Error failure) {
                ZLinkStreamSessionCallResources.close(parts, payload);
                throw failure;
            }
            return ZLinkOneWayCalls.adaptOneWay(result);
        }
    }
}

final class ZLinkStreamSessionCallResources {
    private ZLinkStreamSessionCallResources() {}

    static void close(List<Message> parts, ZLinkFrameworkMultipartView payload) {
        try {
            ZLinkFrameworkMultipartView.closeAll(parts);
        } finally {
            payload.close();
        }
    }
}
