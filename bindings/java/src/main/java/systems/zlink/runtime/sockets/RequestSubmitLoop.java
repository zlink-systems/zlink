/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.runtime.sockets;

import java.lang.foreign.MemorySegment;
import java.util.List;
import java.util.concurrent.CompletableFuture;
import java.util.function.BiConsumer;
import systems.zlink.contracts.errors.ZlinkSubmitException;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.contracts.messaging.Received;
import systems.zlink.contracts.sockets.RequestResult;
import systems.zlink.contracts.sockets.SendFlags;
import systems.zlink.contracts.sockets.SubmitResult;
import systems.zlink.runtime.nativeapi.Native;
import systems.zlink.runtime.nativeapi.NativeErrno;
import systems.zlink.runtime.nativeapi.RequestReplySupport;
import systems.zlink.runtime.nativeapi.RequestProgressPump;
import systems.zlink.runtime.nativeapi.RoutedRequestSupport;

final class RequestSubmitLoop {
    private RequestSubmitLoop() {
    }

    static void submitResultParts(List<Message> parts,
                                  PartSubmitter submitter) {
        submitParts(parts, submitter, rc -> {
            if (rc != SubmitResult.OK.value()) {
                throw new ZlinkSubmitException(SubmitResult.fromValue(rc));
            }
        });
    }

    static void submitErrnoParts(List<Message> parts,
                                 PartSubmitter submitter,
                                 SubmitFailure submitFailure) {
        submitParts(parts, submitter, rc -> {
            if (rc != 0) {
                throw submitFailure.fromLastError();
            }
        });
    }

    static CompletableFuture<Received> submitFuture(long timeoutMs,
                                                    MemorySegment socketHandle,
                                                    String progressLabel,
                                                    RequestSubmission submission) {
        long requestId = RoutedRequestSupport.nextRequestId();
        CompletableFuture<Received> future =
            RoutedRequestSupport.registerPending(requestId, timeoutMs);
        try {
            submission.submit(RoutedRequestSupport.replyCallback(),
                RoutedRequestSupport.userData(requestId));
            RequestReplySupport.startSocketRequestProgress(future,
                socketHandle, progressLabel);
            return future;
        } catch (RuntimeException ex) {
            RoutedRequestSupport.removePending(requestId);
            future.cancel(false);
            throw ex;
        }
    }

    static boolean submitCallback(long timeoutMs,
                                  MemorySegment socketHandle,
                                  String progressLabel,
                                  SendFlags flags,
                                  BiConsumer<RequestResult, List<Message>> callback,
                                  RequestSubmission submission) {
        long requestId = RoutedRequestSupport.nextRequestId();
        boolean callerOwnsProgress =
            RequestProgressPump.hasExternalProgress(socketHandle);
        CompletableFuture<Void> progress = callerOwnsProgress ? null
            : RoutedRequestSupport.registerDirectPending(requestId, callback);
        if (callerOwnsProgress) {
            RoutedRequestSupport.registerDirectPendingWithoutProgress(requestId,
                callback);
        }
        try {
            submission.submit(RoutedRequestSupport.replyCallback(),
                RoutedRequestSupport.userData(requestId));
            if (progress != null) {
                RequestReplySupport.startSocketRequestProgress(progress,
                    socketHandle, progressLabel);
            }
            return true;
        } catch (ZlinkSubmitException ex) {
            RoutedRequestSupport.removeDirectPending(requestId);
            if (flags == SendFlags.DONT_WAIT
                && ex.getResult() == SubmitResult.BACKPRESSURED) {
                return false;
            }
            throw ex;
        } catch (RuntimeException ex) {
            RoutedRequestSupport.removeDirectPending(requestId);
            throw ex;
        }
    }

    private static void submitParts(List<Message> parts,
                                    PartSubmitter submitter,
                                    ResultHandler resultHandler) {
        for (int i = 0; i < parts.size(); i++) {
            int partFlag = i + 1 < parts.size()
                ? Native.PART_MORE : Native.PART_FINAL;
            Message part = parts.get(i);
            int rc = NativeErrno.retryWhileInterrupted(
                () -> submitter.submit(part, partFlag),
                result -> result != 0);
            resultHandler.handle(rc);
        }
    }

    @FunctionalInterface
    interface PartSubmitter {
        int submit(Message part, int partFlag);
    }

    @FunctionalInterface
    interface SubmitFailure {
        RuntimeException fromLastError();
    }

    @FunctionalInterface
    interface RequestSubmission {
        void submit(MemorySegment handler, MemorySegment userData);
    }

    @FunctionalInterface
    private interface ResultHandler {
        void handle(int rc);
    }
}
