package systems.zlink.stream.connector;

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
    private final Consumer<ZLinkStreamCloseReason> closeReasonReceived;
    private final ZLinkStreamActorRegistry actors;

    ZLinkStreamReceiveDispatcher(
            ZLinkStreamConnectorConfiguration configuration,
            Map<String, List<ZLinkStreamMessageHandler<ZLinkStreamEncodedPayload>>> handlers,
            ZLinkStreamDispatchQueue dispatchQueue,
            ZLinkStreamPendingRequests pendingRequests,
            ZLinkStreamConnectorPayloadCodec payloadCodec,
            Consumer<ZLinkStreamError> errorPublisher,
            Function<String, CompletionStage<Void>> controlSender,
            Consumer<ZLinkStreamCloseReason> closeReasonReceived,
            ZLinkStreamActorRegistry actors) {
        this.configuration = configuration;
        this.handlers = handlers;
        this.dispatchQueue = dispatchQueue;
        this.pendingRequests = pendingRequests;
        this.payloadCodec = payloadCodec;
        this.errorPublisher = errorPublisher;
        this.controlSender = controlSender;
        this.closeReasonReceived = closeReasonReceived;
        this.actors = actors;
    }

    void dispatch(byte[] encodedHeader, byte[] payload) {
        ZLinkStreamWireProtocol.Header header = ZLinkStreamWireProtocol.decodeHeader(encodedHeader);
        if (header.actorSlot() != null) {
            actors.actorId(header.actorSlot());
        }
        byte[] decodedPayload = payloadCodec.decode(header, payload);
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
        if (header.kind() == ZLinkStreamWireProtocol.KIND_RESPONSE) {
            completeResponse(header, decodedPayload);
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
            try {
                ZLinkStreamCloseReason reason = ZLinkSessionClosingControl.decode(payload);
                DefaultZLinkStreamConnector.trace(
                        "connector session-closing version="
                                + ZLinkSessionClosingControl.VERSION
                                + " reason="
                                + reason.name().toLowerCase());
                closeReasonReceived.accept(reason);
            } catch (IllegalArgumentException invalidControl) {
                closeReasonReceived.accept(ZLinkStreamCloseReason.PROTOCOL_ERROR);
                throw invalidControl;
            }
            return;
        }
        if (payload.length != 0) {
            closeReasonReceived.accept(ZLinkStreamCloseReason.PROTOCOL_ERROR);
            throw new IllegalArgumentException("heartbeat control packet payload must be empty");
        }
        if (HEARTBEAT_PING_NAME.equals(header.name())) {
            controlSender.apply(HEARTBEAT_PONG_NAME);
            return;
        }
        if (HEARTBEAT_PONG_NAME.equals(header.name())) {
            return;
        }
        closeReasonReceived.accept(ZLinkStreamCloseReason.PROTOCOL_ERROR);
        throw new IllegalArgumentException("unknown control packet");
    }

    private void completeResponse(ZLinkStreamWireProtocol.Header header, byte[] payload) {
        pendingRequests.complete(
                header.requestSeq(),
                new ZLinkStreamEncodedPayload(
                        header.name(),
                        Message.from(payload),
                        header.metadata(),
                        ZLinkStreamConnectorPayloadCodec.fromWireCodec(header.codec())));
    }

    private void dispatchError(ZLinkStreamWireProtocol.Header header, byte[] payload) {
        ZLinkStreamError error;
        RuntimeException requestFailure;
        try {
            RemoteErrorPayload remote = ERROR_MAPPER.readValue(payload, RemoteErrorPayload.class);
            if (remote.code() == null
                    || remote.code().isBlank()
                    || remote.message() == null
                    || remote.message().isBlank()) {
                throw new IOException("error payload requires string code and message fields");
            }
            requestFailure = new IllegalStateException(remote.code() + ": " + remote.message());
            error =
                    new ZLinkStreamError(
                            ZLinkStreamErrorCode.REMOTE_ERROR, remote.message(), requestFailure);
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

    private record RemoteErrorPayload(String code, String message) {}

    private void dispatchToHandlers(ZLinkStreamWireProtocol.Header header, byte[] payload) {
        String actorId = header.actorSlot() == null ? null : actors.actorId(header.actorSlot());
        List<ZLinkStreamMessageHandler<ZLinkStreamEncodedPayload>> registered =
                List.copyOf(handlers.getOrDefault(header.name(), List.of()));
        List<ZLinkStreamActorRegistry.DefaultActor.HandlerRegistration> actorRegistered =
                header.actorSlot() == null
                        ? List.of()
                        : actors.handlers(header.actorSlot(), header.name());
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
        Supplier<CompletionStage<Void>> dispatch =
                () -> {
                    List<ZLinkStreamMessageHandler<ZLinkStreamEncodedPayload>> activeRegistered =
                            registered.stream()
                                    .filter(
                                            handler ->
                                                    handlers.getOrDefault(header.name(), List.of())
                                                            .contains(handler))
                                    .toList();
                    List<ZLinkStreamMessageHandler<ZLinkStreamEncodedPayload>>
                            activeActorRegistered =
                                    actorRegistered.stream()
                                            .filter(
                                                    ZLinkStreamActorRegistry.DefaultActor
                                                                    .HandlerRegistration
                                                            ::active)
                                            .map(
                                                    ZLinkStreamActorRegistry.DefaultActor
                                                                    .HandlerRegistration
                                                            ::handler)
                                            .toList();
                    if (activeRegistered.isEmpty() && activeActorRegistered.isEmpty()) {
                        message.payload().payload().close();
                        return CompletableFuture.completedFuture(null);
                    }
                    CompletionStage<Void> completion = CompletableFuture.completedFuture(null);
                    completion =
                            invokeHandlers(completion, activeRegistered, header, payload, actorId);
                    completion =
                            invokeHandlers(
                                    completion, activeActorRegistered, header, payload, actorId);
                    return completion.whenComplete(
                            (ignored, error) -> message.payload().payload().close());
                };
        dispatchQueue.addMessage(
                message,
                dispatch,
                () ->
                        registered.stream()
                                        .anyMatch(
                                                handler ->
                                                        handlers.getOrDefault(
                                                                        header.name(), List.of())
                                                                .contains(handler))
                                || actorRegistered.stream()
                                        .anyMatch(
                                                ZLinkStreamActorRegistry.DefaultActor
                                                                .HandlerRegistration
                                                        ::active),
                configuration.dispatchMode() == ZLinkStreamDispatchMode.IMMEDIATE);
    }

    private CompletionStage<Void> invokeHandlers(
            CompletionStage<Void> completion,
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
            completion =
                    completion.thenCompose(
                            ignored ->
                                    invokeUserCallback(() -> handler.handleAsync(handlerMessage)));
        }
        return completion;
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
