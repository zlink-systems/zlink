package systems.zlink.stream.connector;

import com.fasterxml.jackson.databind.JsonNode;
import com.fasterxml.jackson.databind.ObjectMapper;

import systems.zlink.contracts.messaging.Message;

import java.io.IOException;
import java.util.List;
import java.util.Map;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.function.Consumer;
import java.util.function.Function;
import java.util.function.Supplier;

final class ZLinkStreamReceiveDispatcher {
    private static final ObjectMapper ERROR_MAPPER = new ObjectMapper();
    private static final String HEARTBEAT_PING_NAME = "$zlink.heartbeat.ping";
    private static final String HEARTBEAT_PONG_NAME = "$zlink.heartbeat.pong";

    private final ZLinkStreamConnectorConfiguration configuration;
    private final Map<String, List<ZLinkStreamMessageHandler<ZLinkStreamEncodedPayload>>> handlers;
    private final ZLinkStreamDispatchQueue dispatchQueue;
    private final ZLinkStreamPendingRequests pendingRequests;
    private final ZLinkStreamConnectorPayloadCodec payloadCodec;
    private final Consumer<ZLinkStreamError> errorPublisher;
    private final Function<String, CompletionStage<Void>> controlSender;
    private final Consumer<ZLinkStreamCloseReason> sessionClosing;
    private final ZLinkStreamActorRegistry actors;

    ZLinkStreamReceiveDispatcher(
            ZLinkStreamConnectorConfiguration configuration,
            Map<String, List<ZLinkStreamMessageHandler<ZLinkStreamEncodedPayload>>> handlers,
            ZLinkStreamDispatchQueue dispatchQueue,
            ZLinkStreamPendingRequests pendingRequests,
            ZLinkStreamConnectorPayloadCodec payloadCodec,
            Consumer<ZLinkStreamError> errorPublisher,
            Function<String, CompletionStage<Void>> controlSender,
            Consumer<ZLinkStreamCloseReason> sessionClosing,
            ZLinkStreamActorRegistry actors) {
        this.configuration = configuration;
        this.handlers = handlers;
        this.dispatchQueue = dispatchQueue;
        this.pendingRequests = pendingRequests;
        this.payloadCodec = payloadCodec;
        this.errorPublisher = errorPublisher;
        this.controlSender = controlSender;
        this.sessionClosing = sessionClosing;
        this.actors = actors;
    }

    void dispatch(byte[] encodedHeader, byte[] payload) {
        ZLinkStreamWireProtocol.Header header = ZLinkStreamWireProtocol.decodeHeader(encodedHeader);
        byte[] decodedPayload;
        try {
            if (header.kind() == ZLinkStreamWireProtocol.KIND_RESPONSE) {
                completeResponse(header, payload);
                return;
            }
            if (header.actorSlot() != null) {
                actors.actorId(header.actorSlot());
            }
            decodedPayload = payloadCodec.decode(header, payload);
        } catch (ZLinkStreamException failure) {
            if (failure.errorCode() != ZLinkStreamErrorCode.DECOMPRESSION_FAILED) {
                throw failure;
            }
            boolean pendingFailed =
                    header.requestSeq() != null
                            && pendingRequests.fail(header.requestSeq(), failure);
            if (!pendingFailed && header.kind() != ZLinkStreamWireProtocol.KIND_RESPONSE) {
                errorPublisher.accept(
                        new ZLinkStreamError(
                                ZLinkStreamErrorCode.DECOMPRESSION_FAILED,
                                failure.getMessage(),
                                failure));
            }
            return;
        }
        DefaultZLinkStreamConnector.trace(
                "connector read-frame endpoint="
                        + configuration.endpoint()
                        + " kind="
                        + header.kind()
                        + " name="
                        + header.name()
                        + " requestSeq="
                        + header.requestSeq()
                        + " bytes="
                        + decodedPayload.length
                        + " correlation="
                        + header.correlationId());
        if (header.kind() == ZLinkStreamWireProtocol.KIND_CONTROL) {
            dispatchControl(header, decodedPayload);
            return;
        }
        if (header.kind() == ZLinkStreamWireProtocol.KIND_ERROR) {
            dispatchError(header, decodedPayload);
            return;
        }
        if (header.kind() == ZLinkStreamWireProtocol.KIND_SEND
                || header.kind() == ZLinkStreamWireProtocol.KIND_REQUEST) {
            dispatchToHandlers(header, decodedPayload);
        }
    }

    private void dispatchControl(ZLinkStreamWireProtocol.Header header, byte[] payload) {
        if (ZLinkStreamActorRegistry.BOUND.equals(header.name())) {
            actors.bound(payload);
            return;
        }
        if (ZLinkStreamActorRegistry.UNBOUND.equals(header.name())) {
            actors.unbound(payload);
            return;
        }
        if (ZLinkSessionClosingControl.NAME.equals(header.name())) {
            //  An invalid payload throws, and the receive path ends the connection as a
            //  protocol violation (spec 32 9).
            ZLinkStreamCloseReason reason = ZLinkSessionClosingControl.decode(payload);
            DefaultZLinkStreamConnector.trace(
                    "connector session-closing version="
                            + ZLinkSessionClosingControl.VERSION
                            + " reason="
                            + reason.name().toLowerCase());
            sessionClosing.accept(reason);
            return;
        }
        if (payload.length != 0) {
            throw new IllegalArgumentException("heartbeat control packet payload must be empty");
        }
        if (HEARTBEAT_PING_NAME.equals(header.name())) {
            controlSender.apply(HEARTBEAT_PONG_NAME);
            return;
        }
        if (HEARTBEAT_PONG_NAME.equals(header.name())) {
            return;
        }
        throw new IllegalArgumentException("unknown control packet");
    }

    private void completeResponse(ZLinkStreamWireProtocol.Header header, byte[] payload) {
        pendingRequests.complete(
                header.requestSeq(),
                () ->
                        new ZLinkStreamEncodedPayload(
                                header.name(),
                                Message.from(payloadCodec.decode(header, payload)),
                                header.metadata(),
                                ZLinkStreamConnectorPayloadCodec.fromWireCodec(header.codec())));
    }

    private void dispatchError(ZLinkStreamWireProtocol.Header header, byte[] payload) {
        ZLinkStreamError error;
        RuntimeException requestFailure;
        try {
            JsonNode remote = ERROR_MAPPER.readTree(payload);
            JsonNode code = remote == null ? null : remote.get("code");
            JsonNode message = remote == null ? null : remote.get("message");
            if (remote == null
                    || !remote.isObject()
                    || code == null
                    || !code.isTextual()
                    || code.textValue().isBlank()
                    || message == null
                    || !message.isTextual()
                    || message.textValue().isBlank()) {
                throw new IOException("error payload requires string code and message fields");
            }
            requestFailure =
                    new IllegalStateException(code.textValue() + ": " + message.textValue());
            error =
                    new ZLinkStreamError(
                            ZLinkStreamErrorCode.REMOTE_ERROR, message.textValue(), requestFailure);
        } catch (IOException ex) {
            requestFailure = new IllegalArgumentException("remote error payload is invalid", ex);
            error =
                    new ZLinkStreamError(
                            ZLinkStreamErrorCode.FRAME_DECODE_FAILED,
                            "Remote error payload is invalid.",
                            requestFailure);
        }
        if (header.requestSeq() == null
                || !pendingRequests.fail(header.requestSeq(), new ZLinkStreamException(error))) {
            errorPublisher.accept(error);
        }
    }

    private void dispatchToHandlers(ZLinkStreamWireProtocol.Header header, byte[] payload) {
        //  The slot names the Actor when the packet arrives; the handlers of that
        //  Actor handle are read when the packet is dispatched.
        ZLinkStreamActorRegistry.DefaultActor actor =
                header.actorSlot() == null ? null : actors.actor(header.actorSlot());
        String actorId = actor == null ? null : actor.actorId();
        ZLinkStreamMessage<ZLinkStreamEncodedPayload> message =
                new ZLinkStreamMessage<>(
                        header.name(),
                        new ZLinkStreamEncodedPayload(
                                header.name(),
                                Message.from(payload),
                                header.metadata(),
                                ZLinkStreamConnectorPayloadCodec.fromWireCodec(header.codec())),
                        header.metadata(),
                        actorId);
        //  Spec 32 5.6, 7, 10: the connector and Actor handle handlers are the
        //  ones registered when the packet is dispatched, so one registered after
        //  it arrived takes it.
        Supplier<Supplier<CompletionStage<Void>>> selectDispatch =
                () -> {
                    List<ZLinkStreamMessageHandler<ZLinkStreamEncodedPayload>> activeRegistered =
                            List.copyOf(handlers.getOrDefault(header.name(), List.of()));
                    List<ZLinkStreamMessageHandler<ZLinkStreamEncodedPayload>>
                            activeActorRegistered = actorHandlers(actor, header.name());
                    if (activeRegistered.isEmpty() && activeActorRegistered.isEmpty()) {
                        return null;
                    }
                    return () -> {
                        invokeHandlers(activeRegistered, header, payload, actorId);
                        invokeHandlers(activeActorRegistered, header, payload, actorId);
                        message.payload().payload().close();
                        return CompletableFuture.completedFuture(null);
                    };
                };
        dispatchQueue.addMessage(
                message,
                selectDispatch,
                () ->
                        handlers.getOrDefault(header.name(), List.of()).size()
                                + actorHandlers(actor, header.name()).size(),
                configuration.dispatchMode() == ZLinkStreamDispatchMode.IMMEDIATE);
    }

    private static List<ZLinkStreamMessageHandler<ZLinkStreamEncodedPayload>> actorHandlers(
            ZLinkStreamActorRegistry.DefaultActor actor, String name) {
        if (actor == null) {
            return List.of();
        }
        return actor.handlers(name).stream()
                .filter(ZLinkStreamActorRegistry.DefaultActor.HandlerRegistration::active)
                .map(ZLinkStreamActorRegistry.DefaultActor.HandlerRegistration::handler)
                .toList();
    }

    private void invokeHandlers(
            List<ZLinkStreamMessageHandler<ZLinkStreamEncodedPayload>> registered,
            ZLinkStreamWireProtocol.Header header,
            byte[] payload,
            String actorId) {
        for (ZLinkStreamMessageHandler<ZLinkStreamEncodedPayload> handler : registered) {
            ZLinkStreamMessage<ZLinkStreamEncodedPayload> handlerMessage =
                    new ZLinkStreamMessage<>(
                            header.name(),
                            new ZLinkStreamEncodedPayload(
                                    header.name(),
                                    Message.from(payload),
                                    header.metadata(),
                                    ZLinkStreamConnectorPayloadCodec.fromWireCodec(header.codec())),
                            header.metadata(),
                            actorId);
            invokeUserCallback(() -> handler.handleAsync(handlerMessage));
        }
    }

    private CompletionStage<Void> invokeUserCallback(UserCallback callback) {
        try {
            return callback.invoke()
                    .exceptionally(
                            ex -> {
                                errorPublisher.accept(
                                        DefaultZLinkStreamConnector.userCallbackFailed(ex));
                                return null;
                            });
        } catch (Throwable ex) {
            errorPublisher.accept(DefaultZLinkStreamConnector.userCallbackFailed(ex));
            return CompletableFuture.completedFuture(null);
        }
    }

    @FunctionalInterface
    private interface UserCallback {
        CompletionStage<Void> invoke();
    }
}
