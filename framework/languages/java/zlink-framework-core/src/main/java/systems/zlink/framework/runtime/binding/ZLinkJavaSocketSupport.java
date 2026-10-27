package systems.zlink.framework.runtime.binding;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.errors.ConnectResult;
import systems.zlink.contracts.errors.ZlinkConnectException;
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
import systems.zlink.contracts.sockets.RouterSocket;
import systems.zlink.contracts.sockets.SendFlags;
import systems.zlink.contracts.sockets.SubmitResult;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendReceived;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendRecvMode;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendRequestResult;
import systems.zlink.framework.runtime.internal.calls.ZLinkOneWayCalls;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceOperationRegistry;

import java.time.Duration;
import java.util.List;
import java.util.Optional;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.function.BiConsumer;
import java.util.function.BooleanSupplier;
import java.util.function.Supplier;

final class ZLinkJavaSocketSupport {
    private ZLinkJavaSocketSupport() {}

    static Throwable requestTimeout() {
        return ZLinkJavaRawMeshNode.requestFailure(
                new systems.zlink.contracts.errors.ZlinkRequestException(
                        systems.zlink.contracts.sockets.RequestResult.TIMED_OUT));
    }

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

    /** An already absent physical RID satisfies liveness teardown. */
    static void disconnectRidIfPresent(RouterSocket socket, RoutingId peer) {
        try {
            socket.disconnectRid(peer);
        } catch (ZlinkConnectException failure) {
            if (failure.getResult() != ConnectResult.NOT_FOUND) {
                throw failure;
            }
        }
    }

    static RecvFlags map(ZLinkBackendRecvMode mode) {
        return mode == ZLinkBackendRecvMode.DONT_WAIT ? RecvFlags.DONT_WAIT : RecvFlags.NONE;
    }

    static CompletionStage<Void> submit(
            SendOperation operation,
            List<Message> parts,
            ZLinkServiceOperationRegistry operations) {
        var submit = operation.message(parts.get(0));
        for (int i = 1; i < parts.size(); i++) {
            submit.message(parts.get(i));
        }
        return submit(submit, operations);
    }

    static CompletionStage<Void> submit(
            SendSubmitOperation operation, ZLinkServiceOperationRegistry operations) {
        if (operations.isClosed()) {
            return CompletableFuture.failedStage(
                    ZLinkOneWayCalls.failureForStatus(ZLinkOneWayCalls.SHUTDOWN));
        }
        var submission = operation.submit();
        var result = submission.result();
        if (result == SubmitResult.BACKPRESSURED) {
            return operations.submitSend(submission::admitted);
        }
        // The admission supplier is consumed only for BACKPRESSURED, handled above.
        return admission(result, null);
    }

    /** Reports the binding terminal directly to the request's registered entry. */
    static CompletionStage<List<Message>> observeReply(
            RequestSubmission submission,
            Runnable admission,
            CompletableFuture<Void> requestAdmission,
            BiConsumer<List<Message>, Throwable> terminal) {
        SubmitResult submitResult = submission.result();
        CompletionStage<List<Message>> bindingReply = submission.reply();
        if (submitResult == SubmitResult.OK) {
            if (requestAdmission != null) requestAdmission.complete(null);
        } else {
            admission(submitResult, submission::admitted)
                    .whenComplete(
                            (ignored, failure) -> {
                                if (failure == null && requestAdmission != null)
                                    requestAdmission.complete(null);
                            });
        }
        bindingReply.whenComplete(
                (parts, failure) -> {
                    if (failure == null && admission != null) admission.run();
                    terminal.accept(parts, failure);
                });
        return bindingReply;
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
            RequestOperation operation,
            List<Message> parts,
            Duration timeout,
            Runnable admission,
            CompletableFuture<Void> requestAdmission,
            ZLinkServiceOperationRegistry operations) {
        var id = systems.zlink.framework.runtime.internal.service.ZLinkServiceOperationIds.next();
        return operations.submitTerminal(
                id,
                timeout,
                terminal ->
                        submitRequestTo(
                                operation,
                                parts,
                                operations.remainingTimeout(id),
                                admission,
                                requestAdmission,
                                terminal),
                ZLinkBackendReceived::close);
    }

    static CompletionStage<?> submitRequestTo(
            RequestOperation operation,
            List<Message> parts,
            Duration timeout,
            Runnable admission,
            CompletableFuture<Void> requestAdmission,
            BiConsumer<ZLinkBackendReceived, Throwable> terminal) {
        try {
            var submit = operation.message(parts.get(0)).timeout(timeout);
            for (int i = 1; i < parts.size(); i++) submit.message(parts.get(i));
            return observeReply(
                    submit.submit(),
                    admission,
                    requestAdmission,
                    (replyParts, failure) -> {
                        if (failure != null) {
                            terminal.accept(null, ZLinkJavaRawMeshNode.requestFailure(failure));
                            return;
                        }
                        try {
                            terminal.accept(
                                    new ZLinkBackendReceived(
                                            ZLinkBackendRequestResult.OK,
                                            Optional.empty(),
                                            Optional.empty(),
                                            Optional.empty(),
                                            replyParts.stream().map(Message::from).toList()),
                                    null);
                        } catch (RuntimeException | Error error) {
                            terminal.accept(null, error);
                        } finally {
                            Message.closeAll(replyParts);
                        }
                    });
        } catch (RuntimeException failure) {
            terminal.accept(null, ZLinkJavaRawMeshNode.requestFailure(failure));
            return null;
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
