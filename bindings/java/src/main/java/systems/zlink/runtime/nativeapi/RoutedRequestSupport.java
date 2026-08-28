/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.runtime.nativeapi;

import java.lang.foreign.Arena;
import java.lang.foreign.FunctionDescriptor;
import java.lang.foreign.Linker;
import java.lang.foreign.MemorySegment;
import java.lang.foreign.ValueLayout;
import java.lang.invoke.MethodHandle;
import java.lang.invoke.MethodHandles;
import java.lang.invoke.MethodType;
import java.util.Arrays;
import java.util.List;
import java.util.Objects;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.ConcurrentHashMap;
import java.util.concurrent.ConcurrentMap;
import java.util.concurrent.atomic.AtomicLong;
import systems.zlink.contracts.errors.ZlinkRequestException;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.contracts.sockets.RequestResult;

/** Core reply callback registry; it owns no executor, queue, or timer. */
public final class RoutedRequestSupport implements AutoCloseable {
    private static final Linker LINKER = Linker.nativeLinker();
    private static final FunctionDescriptor FD_REPLY_CALLBACK =
        FunctionDescriptor.ofVoid(ValueLayout.JAVA_INT, ValueLayout.ADDRESS,
            ValueLayout.JAVA_LONG, ValueLayout.ADDRESS);
    private final Arena callbackArena = Arena.ofShared();
    private final MemorySegment replyCallback;
    private final AtomicLong nextRequestId = new AtomicLong(1L);
    private final ConcurrentMap<Long, PendingReply> pending =
        new ConcurrentHashMap<>();
    private final CallbackLifecycle callbackLifecycle;

    public RoutedRequestSupport(CallbackLifecycle callbackLifecycle) {
        this.callbackLifecycle = Objects.requireNonNull(callbackLifecycle,
            "callbackLifecycle");
        try {
            this.replyCallback = LINKER.upcallStub(callbackHandle(),
                FD_REPLY_CALLBACK, callbackArena);
        } catch (RuntimeException failure) {
            RuntimeResources.closeArena(callbackArena);
            throw failure;
        }
    }

    public MemorySegment replyCallback() {
        return replyCallback;
    }

    public long nextRequestId() {
        long value;
        do {
            value = nextRequestId.getAndIncrement();
        } while (value == 0L);
        return value;
    }

    public static MemorySegment userData(long requestId) {
        return MemorySegment.ofAddress(requestId);
    }

    public void registerRoutedPending(
            long requestId, CompletableFuture<List<Message>> future) {
        pending.put(requestId, new PendingReply(future));
    }

    public void removeRoutedPending(long requestId) {
        pending.remove(requestId);
    }

    /** Removes and fails one pending request, used by socket close. */
    public boolean completePendingExceptionally(long requestId,
                                                Throwable failure) {
        PendingReply reply = pending.remove(requestId);
        if (reply == null) {
            return false;
        }
        callbackLifecycle.enter();
        try {
            reply.future().completeExceptionally(failure);
        } finally {
            callbackLifecycle.exit();
        }
        return true;
    }

    private MethodHandle callbackHandle() {
        try {
            return MethodHandles.lookup().findVirtual(
                RoutedRequestSupport.class, "handleReplyCallback",
                MethodType.methodType(void.class, int.class,
                    MemorySegment.class, long.class, MemorySegment.class))
                .bindTo(this);
        } catch (ReflectiveOperationException failure) {
            throw new ExceptionInInitializerError(failure);
        }
    }

    private void handleReplyCallback(int result,
                                     MemorySegment parts,
                                     long partCount,
                                     MemorySegment userdata) {
        long requestId = userdata == null ? 0L : userdata.address();
        PendingReply replyState = pending.remove(requestId);
        if (replyState == null) {
            closeParts(parts, partCount);
            return;
        }
        Runnable completion;
        Runnable rejectedCompletion;
        callbackLifecycle.enter();
        try {
            if (result != RequestResult.OK.value()) {
                ZlinkRequestException failure = new ZlinkRequestException(
                    requestResult(result), result);
                completion = () -> replyState.future().completeExceptionally(
                    failure);
                rejectedCompletion = completion;
            } else {
                Message[] frames =
                    InternalAccess.messageFromOwnedMessageVectorShared(
                        parts, partCount);
                List<Message> reply = frames.length == 0
                    ? List.of() : Arrays.asList(frames);
                completion = () -> {
                    if (!replyState.future().complete(reply)) {
                        Message.closeAll(frames);
                    }
                };
                rejectedCompletion = () -> {
                    try {
                        Message.closeAll(frames);
                    } finally {
                        replyState.future().completeExceptionally(
                            new ZlinkRequestException(RequestResult.TERMINATED,
                                NativeErrno.ECANCELED));
                    }
                };
            }
        } catch (Throwable failure) {
            completion = () -> replyState.future().completeExceptionally(
                failure);
            rejectedCompletion = completion;
        } finally {
            closeParts(parts, partCount);
            callbackLifecycle.exit();
        }
        try {
            callbackLifecycle.dispatch(completion);
        } catch (RuntimeException rejected) {
            // Socket close cannot normally reject while the Core callback is
            // active: native close reports BUSY until the callback returns.
            // Keep the defensive path off the callback thread as well.
            CompletableFuture.runAsync(rejectedCompletion);
        }
    }

    private static void closeParts(MemorySegment parts, long partCount) {
        if (parts != null && parts.address() != 0L && partCount > 0L) {
            try {
                NativeMessage.multipartClose(parts, partCount);
            } catch (RuntimeException ignored) {
            }
        }
    }

    private static RequestResult requestResult(int value) {
        try {
            return RequestResult.fromValue(value);
        } catch (IllegalArgumentException ignored) {
            return RequestResult.INTERNAL_ERROR;
        }
    }

    public void close(Throwable failure) {
        Objects.requireNonNull(failure, "failure");
        pending.forEach((requestId, reply) ->
            completePendingExceptionally(requestId, failure));
        RuntimeResources.closeArena(callbackArena);
    }

    @Override
    public void close() {
        close(new ZlinkRequestException(RequestResult.TERMINATED,
            NativeErrno.ECANCELED));
    }

    /** Marks the native callback's completion delivery context. */
    public interface CallbackLifecycle {
        void enter();

        void exit();

        void dispatch(Runnable completion);
    }

    private record PendingReply(CompletableFuture<List<Message>> future) {
    }
}
