/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.runtime.nativeapi;

import systems.zlink.internal.DurationConversions;

import systems.zlink.contracts.messaging.Message;
import systems.zlink.contracts.messaging.Received;
import systems.zlink.contracts.errors.ZlinkRecvException;
import systems.zlink.contracts.sockets.RecvResult;
import systems.zlink.contracts.errors.ZlinkRequestException;
import systems.zlink.contracts.errors.ZlinkSubmitException;
import systems.zlink.contracts.sockets.RequestResult;
import systems.zlink.contracts.sockets.SubmitResult;
import systems.zlink.contracts.errors.ZlinkException;
import java.lang.foreign.MemorySegment;
import java.time.Duration;
import java.util.List;
import java.util.Objects;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionException;
import java.util.concurrent.ConcurrentMap;
import java.util.concurrent.Executor;
import java.util.concurrent.Executors;
import java.util.concurrent.ScheduledExecutorService;
import java.util.concurrent.ScheduledFuture;
import java.util.concurrent.ThreadFactory;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.function.Supplier;

public final class RequestReplySupport {
    public static final long DEFAULT_TIMEOUT_MS = 5_000L;
    private static final ScheduledExecutorService REQUEST_TIMEOUTS =
      Executors.newSingleThreadScheduledExecutor(
        new NamedDaemonThreadFactory("zlink-request-timeout"));
    private static final java.util.concurrent.ExecutorService REQUEST_COMPLETIONS =
      Executors.newSingleThreadExecutor(
        new NamedDaemonThreadFactory("zlink-request-complete"));

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

    public static <T> void armTimeout(ConcurrentMap<Long, CompletableFuture<T>> pending,
                                      long requestId,
                                      CompletableFuture<T> future,
                                      long timeoutMs) {
        ScheduledFuture<?> timeout = REQUEST_TIMEOUTS.schedule(() -> {
            if (pending.remove(requestId, future)) {
                future.completeExceptionally(new ZlinkRequestException(
                    RequestResult.TIMED_OUT));
            }
        }, timeoutMs, TimeUnit.MILLISECONDS);
        future.whenComplete((ignored, error) -> timeout.cancel(false));
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

    public static Executor callbackCompletions() {
        return REQUEST_COMPLETIONS;
    }

    public static <T> void completeAsync(CompletableFuture<T> future,
                                         Supplier<T> supplier) {
        callbackCompletions().execute(() -> {
            if (future == null || future.isDone()) {
                return;
            }
            try {
                T value = supplier.get();
                if (!future.complete(value) && value instanceof AutoCloseable closeable) {
                    try {
                        closeable.close();
                    } catch (Exception ignored) {
                    }
                }
            } catch (Throwable error) {
                future.completeExceptionally(error);
            }
        });
    }

    public static void completeExceptionallyAsync(CompletableFuture<?> future,
                                                  Throwable error) {
        callbackCompletions().execute(() -> {
            if (future != null) {
                future.completeExceptionally(error);
            }
        });
    }

    public static void startSocketRequestProgress(CompletableFuture<?> future,
                                                  MemorySegment socketHandle,
                                                  String threadName) {
        RequestProgressPump.trackSocketRequest(future, socketHandle, threadName);
    }

    public static ScheduledFuture<?> scheduleRequestTimeout(Runnable action,
                                                            long timeoutMs) {
        return REQUEST_TIMEOUTS.schedule(action, timeoutMs, TimeUnit.MILLISECONDS);
    }

    private static final class NamedDaemonThreadFactory implements ThreadFactory {
        private final String name;
        private final AtomicInteger sequence = new AtomicInteger();

        private NamedDaemonThreadFactory(String name) {
            this.name = name;
        }

        @Override
        public Thread newThread(Runnable runnable) {
            Thread thread = new Thread(runnable,
                name + "-" + sequence.getAndIncrement());
            thread.setDaemon(true);
            return thread;
        }
    }

}
