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
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.ConcurrentHashMap;
import java.util.concurrent.ConcurrentMap;
import java.util.concurrent.atomic.AtomicLong;
import systems.zlink.contracts.errors.ZlinkRequestException;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.contracts.sockets.RequestResult;

/** Core reply callback registry; it owns no executor, queue, or timer. */
public final class RoutedRequestSupport {
    private static final Linker LINKER = Linker.nativeLinker();
    private static final FunctionDescriptor FD_REPLY_CALLBACK =
        FunctionDescriptor.ofVoid(ValueLayout.JAVA_INT, ValueLayout.ADDRESS,
            ValueLayout.JAVA_LONG, ValueLayout.ADDRESS);
    private static final Arena CALLBACK_ARENA = Arena.ofShared();
    private static final MemorySegment REPLY_CALLBACK = LINKER.upcallStub(
        callbackHandle(), FD_REPLY_CALLBACK, CALLBACK_ARENA);
    private static final AtomicLong NEXT_REQUEST_ID = new AtomicLong(1L);
    private static final ConcurrentMap<Long, PendingReply> PENDING =
        new ConcurrentHashMap<>();

    private RoutedRequestSupport() {
    }

    public static MemorySegment replyCallback() {
        return REPLY_CALLBACK;
    }

    public static long nextRequestId() {
        long value;
        do {
            value = NEXT_REQUEST_ID.getAndIncrement();
        } while (value == 0L);
        return value;
    }

    public static MemorySegment userData(long requestId) {
        return MemorySegment.ofAddress(requestId);
    }

    public static void registerRoutedPending(
            long requestId, CompletableFuture<List<Message>> future,
            CallbackLifecycle callbackLifecycle) {
        PENDING.put(requestId, new PendingReply(future, callbackLifecycle));
    }

    public static void removeRoutedPending(long requestId) {
        PENDING.remove(requestId);
    }

    /** Removes and fails one pending request, used by socket close. */
    public static boolean completePendingExceptionally(long requestId,
                                                        Throwable failure) {
        PendingReply pending = PENDING.remove(requestId);
        if (pending == null) {
            return false;
        }
        pending.callbackLifecycle().enter();
        try {
            pending.future().completeExceptionally(failure);
        } finally {
            pending.callbackLifecycle().exit();
        }
        return true;
    }

    private static MethodHandle callbackHandle() {
        try {
            return MethodHandles.lookup().findStatic(
                RoutedRequestSupport.class, "handleReplyCallback",
                MethodType.methodType(void.class, int.class,
                    MemorySegment.class, long.class, MemorySegment.class));
        } catch (ReflectiveOperationException failure) {
            throw new ExceptionInInitializerError(failure);
        }
    }

    private static void handleReplyCallback(int result,
                                            MemorySegment parts,
                                            long partCount,
                                            MemorySegment userdata) {
        long requestId = userdata == null ? 0L : userdata.address();
        PendingReply pending = PENDING.remove(requestId);
        if (pending == null) {
            closeParts(parts, partCount);
            return;
        }
        pending.callbackLifecycle().enter();
        try {
            if (result != RequestResult.OK.value()) {
                pending.future().completeExceptionally(
                    new ZlinkRequestException(requestResult(result), result));
                return;
            }
            Message[] frames = InternalAccess.messageFromOwnedMessageVectorShared(
                parts, partCount);
            List<Message> reply = frames.length == 0
                ? List.of() : Arrays.asList(frames);
            if (!pending.future().complete(reply)) {
                Message.closeAll(frames);
            }
        } catch (Throwable failure) {
            pending.future().completeExceptionally(failure);
        } finally {
            closeParts(parts, partCount);
            pending.callbackLifecycle().exit();
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

    /** Marks the native callback's completion delivery context. */
    public interface CallbackLifecycle {
        void enter();

        void exit();
    }

    private record PendingReply(CompletableFuture<List<Message>> future,
                                CallbackLifecycle callbackLifecycle) {
    }
}
