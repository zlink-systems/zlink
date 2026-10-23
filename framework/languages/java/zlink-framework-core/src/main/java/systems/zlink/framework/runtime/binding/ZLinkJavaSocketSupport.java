package systems.zlink.framework.runtime.binding;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.errors.ConnectResult;
import systems.zlink.contracts.errors.ZlinkConnectException;
import systems.zlink.contracts.errors.ZlinkRecvException;
import systems.zlink.contracts.errors.ZlinkRequestException;
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
import systems.zlink.contracts.sockets.RouterSocket;
import systems.zlink.contracts.sockets.SendFlags;
import systems.zlink.contracts.sockets.SubmitResult;
import systems.zlink.framework.runtime.internal.ZLinkCompletionBridge;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendReceived;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendRecvMode;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendRequestResult;
import systems.zlink.framework.runtime.internal.calls.ZLinkOneWayCalls;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceOperationRegistry;

import java.time.Duration;
import java.util.List;
import java.util.Optional;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionException;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.TimeUnit;
import java.util.function.BiConsumer;
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

    static CompletionStage<List<Message>> reply(
            RequestSubmission submission,
            long deadlineNanos,
            Runnable admission,
            CompletableFuture<Void> requestAdmission) {
        CompletableFuture<List<Message>> result = new CompletableFuture<>();
        observeReply(
                submission,
                deadlineNanos,
                admission,
                requestAdmission,
                (parts, failure) -> completeReply(result, parts, failure),
                result);
        return result;
    }

    /** Reports the binding terminal directly to an already registered request owner. */
    static CompletionStage<List<Message>> observeReply(
            RequestSubmission submission,
            long deadlineNanos,
            Runnable admission,
            CompletableFuture<Void> requestAdmission,
            BiConsumer<List<Message>, Throwable> terminal) {
        return observeReply(submission, deadlineNanos, admission, requestAdmission, terminal, null);
    }

    private static CompletionStage<List<Message>> observeReply(
            RequestSubmission submission,
            long deadlineNanos,
            Runnable admission,
            CompletableFuture<Void> requestAdmission,
            BiConsumer<List<Message>, Throwable> terminal,
            CompletableFuture<List<Message>> standalone) {
        SubmitResult submitResult = submission.result();
        CompletionStage<List<Message>> bindingReply = submission.reply();
        CompletionStage<Void> admitted = null;
        if (submitResult == SubmitResult.OK) {
            // Admitted at submit: nothing waits for admission and only the reply can be cancelled.
            if (requestAdmission != null) requestAdmission.complete(null);
        } else {
            admitted = admission(submitResult, submission::admitted);
            if (requestAdmission != null) {
                admitted.whenComplete(
                        (ignored, failure) -> {
                            if (failure == null) requestAdmission.complete(null);
                        });
            }
        }
        // Binding completes reply with the same cause when admission fails. Reply is the
        // single terminal source, including the payload cleanup performed by its observer.
        bindingReply.whenComplete(
                (parts, failure) -> {
                    if (failure == null && admission != null) admission.run();
                    terminal.accept(parts, failure);
                });
        if (standalone != null) {
            if (admitted == null)
                ZLinkCompletionBridge.forwardCancellation(standalone, bindingReply);
            else ZLinkCompletionBridge.forwardCancellation(standalone, admitted, bindingReply);
        }
        if (standalone != null
                && submitResult == SubmitResult.BACKPRESSURED
                && !standalone.isDone()) {
            var deadline =
                    ZLinkProcessExecutionLanes.deadlines()
                            .schedule(
                                    () -> {
                                        standalone.completeExceptionally(
                                                new ZlinkRequestException(RequestResult.TIMED_OUT));
                                    },
                                    Math.max(0L, deadlineNanos - System.nanoTime()),
                                    TimeUnit.NANOSECONDS);
            standalone.whenComplete((ignored, failure) -> deadline.cancel(false));
        }
        return bindingReply;
    }

    private static void completeReply(
            CompletableFuture<List<Message>> result, List<Message> parts, Throwable failure) {
        if (failure != null) result.completeExceptionally(failure);
        else {
            ZLinkCompletionBridge.completeOrDiscard(result, parts, Message::closeAll);
        }
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
            CompletableFuture<Void> requestAdmission) {
        long deadlineNanos = System.nanoTime() + timeout.toNanos();
        var submit = operation.message(parts.get(0)).timeout(timeout);
        for (int i = 1; i < parts.size(); i++) {
            submit.message(parts.get(i));
        }
        try {
            CompletionStage<List<Message>> bindingReply =
                    reply(submit.submit(), deadlineNanos, admission, requestAdmission);
            CompletableFuture<ZLinkBackendReceived> result = new CompletableFuture<>();
            bindingReply.whenComplete(
                    (replyParts, failure) -> {
                        if (failure != null) {
                            result.completeExceptionally(
                                    new CompletionException(
                                            ZLinkJavaRawMeshNode.requestFailure(failure)));
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
            return CompletableFuture.failedFuture(ZLinkJavaRawMeshNode.requestFailure(failure));
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
