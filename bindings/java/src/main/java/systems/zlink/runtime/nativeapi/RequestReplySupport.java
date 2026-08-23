/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.runtime.nativeapi;

import systems.zlink.internal.DurationConversions;
import systems.zlink.contracts.errors.ZlinkException;
import systems.zlink.contracts.errors.ZlinkRecvException;
import systems.zlink.contracts.errors.ZlinkRequestException;
import systems.zlink.contracts.errors.ZlinkSubmitException;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.contracts.messaging.Received;
import systems.zlink.contracts.sockets.RecvResult;
import systems.zlink.contracts.sockets.RequestResult;
import systems.zlink.contracts.sockets.SubmitResult;
import java.time.Duration;
import java.util.List;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionException;

/** Small conversion helpers for Core-owned request completion. */
public final class RequestReplySupport {
    public static final long DEFAULT_TIMEOUT_MS = 5_000L;

    private RequestReplySupport() {
    }

    public static long timeoutMillis(Duration timeout) {
        return DurationConversions.timeoutMillisOrDefault(timeout,
            DEFAULT_TIMEOUT_MS);
    }

    public static int toTimeoutInt(long timeoutMs) {
        if (timeoutMs <= 1L) {
            return 1;
        }
        return timeoutMs >= Integer.MAX_VALUE ? Integer.MAX_VALUE
            : (int) timeoutMs;
    }

    public static Message cloneMessage(Message source) {
        return InternalAccess.messageSharedCopyOf(source);
    }

    public static List<Message> takeReceivedParts(Received received) {
        return InternalAccess.receivedTakeParts(received);
    }

    public static Throwable unwrap(Throwable error) {
        if (error instanceof CompletionException && error.getCause() != null) {
            return error.getCause();
        }
        return error;
    }

    public static RequestResult requestResult(Throwable error) {
        Throwable cause = unwrap(error);
        if (cause instanceof ZlinkRequestException requestException) {
            return requestException.getResult();
        }
        if (cause instanceof java.util.concurrent.TimeoutException) {
            return RequestResult.TIMED_OUT;
        }
        return RequestResult.PROTOCOL_ERROR;
    }

    public static SubmitResult submitResult(Throwable error) {
        Throwable cause = unwrap(error);
        if (cause instanceof ZlinkSubmitException submitException) {
            return submitException.getResult();
        }
        return SubmitResult.INTERNAL_ERROR;
    }

    public static Throwable normalizeRequestFailure(Throwable error) {
        Throwable cause = unwrap(error);
        if (cause instanceof ZlinkRequestException
            || cause instanceof ZlinkSubmitException
            || cause instanceof RuntimeException) {
            return cause;
        }
        return new ZlinkRequestException(RequestResult.PROTOCOL_ERROR);
    }

    public static ZlinkRequestException requestFailure(Throwable error) {
        Throwable cause = unwrap(error);
        if (cause instanceof ZlinkRequestException requestException) {
            return requestException;
        }
        if (cause instanceof ZlinkRecvException recvException) {
            if (recvException.getResult() == RecvResult.TERMINATED) {
                return new ZlinkRequestException(RequestResult.TERMINATED,
                    recvException.getNativeErrno());
            }
            return new ZlinkRequestException(RequestResult.PROTOCOL_ERROR,
                recvException.getNativeErrno());
        }
        if (cause instanceof ZlinkException zlinkException) {
            return new ZlinkRequestException(RequestResult.PROTOCOL_ERROR,
                zlinkException.getNativeErrno());
        }
        return new ZlinkRequestException(RequestResult.PROTOCOL_ERROR);
    }

    public static boolean isClosedSignal(IllegalStateException ex) {
        String message = ex.getMessage();
        return message != null && message.contains("closed");
    }
}
