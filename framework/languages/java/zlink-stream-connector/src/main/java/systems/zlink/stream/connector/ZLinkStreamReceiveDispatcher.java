package systems.zlink.stream.connector;

import com.fasterxml.jackson.databind.ObjectMapper;
import java.io.IOException;
import java.util.List;
import java.util.Map;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.function.Consumer;
import java.util.function.Function;
import systems.zlink.contracts.messaging.Message;

final class ZLinkStreamReceiveDispatcher {
    private static final ObjectMapper ERROR_MAPPER = new ObjectMapper();
    private static final String HEARTBEAT_PING_NAME = "$zlink.heartbeat.ping";
    private static final String HEARTBEAT_PONG_NAME = "$zlink.heartbeat.pong";

    private final ZLinkStreamConnectorConfiguration configuration;
    private final Map<String, List<ZLinkStreamMessageHandler<ZLinkStreamEncodedPayload>>> handlers;
    private final ZLinkStreamDispatchQueue dispatchQueue;
    private final ZLinkStreamPendingRequests pendingRequests;
    private final ZLinkStreamInboundObserverDispatcher inboundObservers;
    private final ZLinkStreamConnectorPayloadCodec payloadCodec;
    private final Consumer<ZLinkStreamError> errorPublisher;
    private final Function<String, CompletionStage<Void>> controlSender;
    private final Consumer<ZLinkStreamCloseReason> closeReasonReceived;

    ZLinkStreamReceiveDispatcher(
        ZLinkStreamConnectorConfiguration configuration,
        Map<String, List<ZLinkStreamMessageHandler<ZLinkStreamEncodedPayload>>> handlers,
        ZLinkStreamDispatchQueue dispatchQueue,
        ZLinkStreamPendingRequests pendingRequests,
        ZLinkStreamInboundObserverDispatcher inboundObservers,
        ZLinkStreamConnectorPayloadCodec payloadCodec,
        Consumer<ZLinkStreamError> errorPublisher,
        Function<String, CompletionStage<Void>> controlSender,
        Consumer<ZLinkStreamCloseReason> closeReasonReceived) {
        this.configuration = configuration;
        this.handlers = handlers;
        this.dispatchQueue = dispatchQueue;
        this.pendingRequests = pendingRequests;
        this.inboundObservers = inboundObservers;
        this.payloadCodec = payloadCodec;
        this.errorPublisher = errorPublisher;
        this.controlSender = controlSender;
        this.closeReasonReceived = closeReasonReceived;
    }

    void dispatch(byte[] encodedHeader, byte[] payload) {
        ZLinkStreamWireProtocol.Header header = ZLinkStreamWireProtocol.decodeHeader(encodedHeader);
        byte[] decodedPayload = payloadCodec.decode(header, payload);
        DefaultZLinkStreamConnector.trace("connector read-frame endpoint=" + configuration.endpoint()
            + " kind=" + header.kind()
            + " name=" + header.name()
            + " requestSeq=" + header.requestSeq()
            + " bytes=" + decodedPayload.length
            + " correlation=" + header.correlationId()
            + " flow=" + header.flowId()
            + " origin=" + flowOriginName(header.flowOrigin()));
        inboundObservers.enqueue(header, payload);
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

    private static String flowOriginName(int origin) {
        return switch (origin) {
            case 1 -> "inbound";
            case 2 -> "timer";
            case 3 -> "application";
            case 4 -> "lifecycle";
            default -> null;
        };
    }

    private void dispatchControl(ZLinkStreamWireProtocol.Header header, byte[] payload) {
        if (ZLinkSessionClosingControl.NAME.equals(header.name())) {
            try {
                ZLinkStreamCloseReason reason = ZLinkSessionClosingControl.decode(payload);
                DefaultZLinkStreamConnector.trace(
                    "connector session-closing version=" + ZLinkSessionClosingControl.VERSION
                        + " reason=" + reason.name().toLowerCase());
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
            if (remote.code() == null || remote.code().isBlank()
                || remote.message() == null || remote.message().isBlank()) {
                throw new IOException("error payload requires string code and message fields");
            }
            requestFailure = new IllegalStateException(remote.code() + ": " + remote.message());
            error = new ZLinkStreamError(
                ZLinkStreamErrorCode.REMOTE_ERROR,
                remote.message(),
                requestFailure);
        } catch (IOException ex) {
            requestFailure = new IllegalArgumentException("remote error payload is invalid", ex);
            error = new ZLinkStreamError(
                ZLinkStreamErrorCode.FRAME_DECODE_FAILED,
                "Remote error payload is invalid.",
                requestFailure);
        }
        if (header.requestSeq() == null
            || !pendingRequests.fail(header.requestSeq(), requestFailure)) {
            errorPublisher.accept(error);
        }
    }

    private record RemoteErrorPayload(String code, String message) { }

    private void dispatchToHandlers(ZLinkStreamWireProtocol.Header header, byte[] payload) {
        ZLinkConnectorFlowContext.State flow = ZLinkConnectorFlowContext.inbound(
            header.flowId(), header.flowOrigin());
        ZLinkFlowOrigin flowOrigin = ZLinkFlowOrigin.fromWireValue(flow.flowOrigin());
        ZLinkStreamMessage<ZLinkStreamEncodedPayload> message = new ZLinkStreamMessage<>(
            header.name(),
            new ZLinkStreamEncodedPayload(
                header.name(),
                Message.from(payload),
                header.metadata(),
                ZLinkStreamConnectorPayloadCodec.fromWireCodec(header.codec())),
            header.metadata(),
            flow.flowId(),
            flowOrigin);
        java.util.function.Supplier<CompletionStage<Void>> dispatch = () -> {
            List<ZLinkStreamMessageHandler<ZLinkStreamEncodedPayload>> registered =
                List.copyOf(handlers.getOrDefault(header.name(), List.of()));
            if (registered.isEmpty()) {
                message.payload().payload().close();
                return CompletableFuture.completedFuture(null);
            }
            CompletionStage<Void> completion = CompletableFuture.completedFuture(null);
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
                        flow.flowId(),
                        flowOrigin);
                completion = completion.thenCompose(ignored -> invokeUserCallback(flow,
                    () -> handler.handleAsync(handlerMessage)));
            }
            return completion.whenComplete((ignored, error) -> message.payload().payload().close());
        };
        dispatchQueue.addMessage(
            message,
            dispatch,
            () -> !handlers.getOrDefault(header.name(), List.of()).isEmpty(),
            configuration.dispatchMode() == ZLinkStreamDispatchMode.IMMEDIATE);
    }

    private CompletionStage<Void> invokeUserCallback(
        ZLinkConnectorFlowContext.State flow,
        UserCallback callback) {
        try (ZLinkConnectorFlowContext.Scope ignored = ZLinkConnectorFlowContext.enter(flow)) {
            return callback.invoke().exceptionally(ex -> {
                errorPublisher.accept(DefaultZLinkStreamConnector.userCallbackFailed(ex));
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
