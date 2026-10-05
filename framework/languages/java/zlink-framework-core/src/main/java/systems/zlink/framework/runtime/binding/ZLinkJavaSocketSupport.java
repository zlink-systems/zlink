package systems.zlink.framework.runtime.binding;

import systems.zlink.contracts.errors.ZlinkRecvException;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.contracts.messaging.PublishOperation;
import systems.zlink.contracts.messaging.Received;
import systems.zlink.contracts.messaging.ReplyOperation;
import systems.zlink.contracts.messaging.RequestOperation;
import systems.zlink.contracts.messaging.RequestSubmission;
import systems.zlink.contracts.messaging.SendOperation;
import systems.zlink.contracts.messaging.SendSubmitOperation;
import systems.zlink.contracts.sockets.RecvFlags;
import systems.zlink.contracts.sockets.RecvResult;
import systems.zlink.contracts.sockets.RequestResult;
import systems.zlink.contracts.sockets.SendFlags;
import systems.zlink.contracts.sockets.SubmitResult;
import systems.zlink.framework.errors.ZLinkFrameworkException;
import systems.zlink.framework.runtime.internal.ZLinkCompletionBridge;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendReceived;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendRecvMode;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendRequestResult;
import systems.zlink.framework.runtime.internal.calls.ZLinkOneWayCalls;

import java.time.Duration;
import java.util.List;
import java.util.Optional;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionException;
import java.util.concurrent.CompletionStage;
import java.util.function.BooleanSupplier;
import java.util.function.Supplier;

final class ZLinkJavaSocketSupport {
    private ZLinkJavaSocketSupport() {}

    static void validateChannelName(String channelName) {
        if (channelName == null || channelName.isBlank()) {
            throw new IllegalArgumentException("channelName must not be blank");
        }
    }

    static boolean recvOrNoData(BooleanSupplier receive) {
        try {
            return receive.getAsBoolean();
        } catch (ZlinkRecvException ex) {
            if (ex.getResult() == RecvResult.NO_DATA) {
                return false;
            }
            throw ex;
        }
    }

    static RecvFlags map(ZLinkBackendRecvMode mode) {
        return mode == ZLinkBackendRecvMode.DONT_WAIT ? RecvFlags.DONT_WAIT : RecvFlags.NONE;
    }

    static CompletionStage<Void> submit(SendOperation operation, List<Message> parts) {
        var submit = operation.message(parts.get(0));
        for (int i = 1; i < parts.size(); i++) {
            submit.message(parts.get(i));
        }
        return submit(submit);
    }

    static CompletionStage<Void> submit(SendSubmitOperation operation) {
        var submission = operation.submit();
        return admission(submission.result(), submission::admitted);
    }

    static CompletionStage<List<Message>> reply(RequestSubmission submission) {
        CompletionStage<Void> admitted = admission(submission.result(), submission::admitted);
        CompletionStage<List<Message>> bindingReply = submission.reply();
        CompletableFuture<List<Message>> result = new CompletableFuture<>();
        admitted.whenComplete(
                (ignored, failure) -> {
                    if (failure != null) result.completeExceptionally(failure);
                });
        bindingReply.whenComplete(
                (parts, failure) -> {
                    if (failure != null) result.completeExceptionally(failure);
                    else ZLinkCompletionBridge.completeOrDiscard(result, parts, Message::closeAll);
                });
        ZLinkCompletionBridge.forwardCancellation(result, admitted, bindingReply);
        return result;
    }

    private static CompletionStage<Void> admission(
            SubmitResult result, Supplier<CompletionStage<Void>> admitted) {
        if (result == SubmitResult.OK) return ZLinkOneWayCalls.immediateAdmission();
        if (result == SubmitResult.BACKPRESSURED) return admitted.get();
        return ZLinkOneWayCalls.adaptOneWay(
                CompletableFuture.failedFuture(
                        new systems.zlink.contracts.errors.ZlinkSubmitException(result)),
                true);
    }

    static boolean submitSync(SendOperation operation, List<Message> parts) {
        var submit = operation.message(parts.get(0));
        for (int i = 1; i < parts.size(); i++) {
            submit.message(parts.get(i));
        }
        submit.submit_sync();
        return true;
    }

    static void submit(PublishOperation operation, List<Message> parts, SendFlags flags) {
        var submit = operation.message(parts.get(0));
        for (int i = 1; i < parts.size(); i++) {
            submit.message(parts.get(i));
        }
        submit.flags(flags).submit();
    }

    static void submitReply(ReplyOperation operation, List<Message> parts) {
        var submit = operation.message(parts.get(0));
        for (int i = 1; i < parts.size(); i++) {
            submit.message(parts.get(i));
        }
        submit.submit();
    }

    static CompletionStage<ZLinkBackendReceived> submitRequest(
            RequestOperation operation, List<Message> parts, Duration timeout) {
        var submit = operation.message(parts.get(0)).timeout(timeout);
        for (int i = 1; i < parts.size(); i++) {
            submit.message(parts.get(i));
        }
        try {
            CompletionStage<List<Message>> bindingReply = reply(submit.submit());
            CompletableFuture<ZLinkBackendReceived> result = new CompletableFuture<>();
            bindingReply.whenComplete(
                    (replyParts, failure) -> {
                        if (failure != null) {
                            RequestResult terminal =
                                    ZLinkJavaRawMeshNode.requestResult(failure, false);
                            result.completeExceptionally(
                                    new CompletionException(
                                            terminal == null
                                                    ? failure
                                                    : new ZLinkFrameworkException(
                                                            ZLinkJavaRawMeshNode.backendResult(
                                                                            terminal)
                                                                    .toFrameworkErrorKind(),
                                                            failure.getMessage(),
                                                            failure)));
                            return;
                        }
                        try {
                            ZLinkBackendReceived received =
                                    new ZLinkBackendReceived(
                                            ZLinkBackendRequestResult.OK,
                                            Optional.empty(),
                                            Optional.empty(),
                                            Optional.empty(),
                                            replyParts.stream().map(Message::from).toList());
                            ZLinkCompletionBridge.completeOrDiscard(
                                    result, received, ZLinkBackendReceived::close);
                        } catch (RuntimeException | Error error) {
                            result.completeExceptionally(error);
                        } finally {
                            replyParts.forEach(Message::close);
                        }
                    });
            ZLinkCompletionBridge.forwardCancellation(result, bindingReply);
            return result;
        } catch (RuntimeException failure) {
            RequestResult terminal = ZLinkJavaRawMeshNode.requestResult(failure, true);
            return CompletableFuture.failedFuture(
                    terminal == null
                            ? failure
                            : new ZLinkFrameworkException(
                                    ZLinkJavaRawMeshNode.backendResult(terminal)
                                            .toFrameworkErrorKind(),
                                    failure.getMessage(),
                                    failure));
        }
    }

    static ZLinkBackendReceived fromReceived(Received received) {
        boolean hasReplyToken = received.replyToken().isPresent();
        return new ZLinkBackendReceived(
                received.getRoutingId(),
                Optional.empty(),
                Optional.empty(),
                received.parts().stream().map(Message::from).toList(),
                hasReplyToken
                        ? replyParts -> {
                            try {
                                submitReply(received.reply(), replyParts);
                            } finally {
                                received.close();
                            }
                        }
                        : null,
                received::close);
    }
}
