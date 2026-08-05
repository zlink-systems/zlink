/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.runtime.nativeapi;

import systems.zlink.contracts.messaging.Message;
import systems.zlink.contracts.messaging.Received;
import systems.zlink.contracts.errors.ZlinkRequestException;
import systems.zlink.contracts.sockets.RequestResult;
import systems.zlink.contracts.core.RoutingId;
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
import java.util.concurrent.ScheduledFuture;
import java.util.concurrent.atomic.AtomicLong;
import java.util.function.BiConsumer;

public final class RoutedRequestSupport {
    private static final Linker LINKER = Linker.nativeLinker();
    private static final FunctionDescriptor FD_REPLY_CALLBACK =
      FunctionDescriptor.ofVoid(ValueLayout.JAVA_INT, ValueLayout.ADDRESS,
        ValueLayout.JAVA_LONG, ValueLayout.ADDRESS);
    private static final Arena CALLBACK_ARENA = Arena.ofShared();
    private static final MemorySegment REPLY_CALLBACK = LINKER.upcallStub(
      callbackHandle(), FD_REPLY_CALLBACK, CALLBACK_ARENA);
    private static final AtomicLong NEXT_REQUEST_ID = new AtomicLong(1L);
    private static final ConcurrentMap<Long, CompletableFuture<Received>> PENDING =
      new ConcurrentHashMap<>();
    private static final ConcurrentMap<Long, DirectReplyState> DIRECT_PENDING =
      new ConcurrentHashMap<>();

    private RoutedRequestSupport() {
    }

    public static MemorySegment replyCallback() {
        return REPLY_CALLBACK;
    }

    public static long nextRequestId() {
        return NEXT_REQUEST_ID.getAndIncrement();
    }

    public static MemorySegment userData(long requestId) {
        return MemorySegment.ofAddress(requestId);
    }

    public static CompletableFuture<Received> registerPending(long requestId,
                                                              long timeoutMs) {
        CompletableFuture<Received> future = new CompletableFuture<>();
        PENDING.put(requestId, future);
        RequestReplySupport.armTimeout(PENDING, requestId, future, timeoutMs);
        return future;
    }

    public static CompletableFuture<Received> removePending(long requestId) {
        return PENDING.remove(requestId);
    }

    public static CompletableFuture<Void> registerDirectPending(
                                            long requestId,
                                            long timeoutMs,
                                            BiConsumer<RequestResult, List<Message>> callback) {
        DirectReplyState state = new DirectReplyState(callback);
        DIRECT_PENDING.put(requestId, state);
        state.timeout = RequestReplySupport.scheduleRequestTimeout(() -> {
            DirectReplyState removed = DIRECT_PENDING.remove(requestId);
            if (removed != null) {
                removed.complete(RequestResult.TIMED_OUT, List.of());
            }
        }, timeoutMs);
        return state.progress;
    }

    public static void removeDirectPending(long requestId) {
        DirectReplyState state = DIRECT_PENDING.remove(requestId);
        if (state != null) {
            state.cancelTimeout();
            state.progress.cancel(false);
        }
    }

    public static void completePendingExceptionally(long requestId,
                                                    Throwable error) {
        CompletableFuture<Received> future = PENDING.remove(requestId);
        if (future != null) {
            future.completeExceptionally(error);
        }
    }

    public static void completeDirectPending(long requestId,
                                             RequestResult result) {
        DirectReplyState state = DIRECT_PENDING.remove(requestId);
        if (state != null) {
            state.cancelTimeout();
            state.complete(result, List.of());
        }
    }

    public static MemorySegment movePayloadToNative(Arena arena,
                                                    List<Message> payload) {
        long messageSize = NativeLayouts.MESSAGE_LAYOUT.byteSize();
        MemorySegment nativeParts = arena.allocate(messageSize * payload.size(),
          NativeLayouts.MESSAGE_LAYOUT.byteAlignment());
        int built = 0;
        try {
            for (int i = 0; i < payload.size(); i++) {
                InternalAccess.messageTransferTo(payload.get(i),
                  nativeParts.asSlice((long) i * messageSize, messageSize));
                built++;
            }
            return nativeParts;
        } catch (RuntimeException ex) {
            for (int i = built; i < payload.size(); i++) {
                try {
                    payload.get(i).close();
                } catch (RuntimeException ignored) {
                }
            }
            throw ex;
        }
    }

    private static MethodHandle callbackHandle() {
        try {
            return MethodHandles.lookup().findStatic(RoutedRequestSupport.class,
              "handleReplyCallback", MethodType.methodType(void.class, int.class,
                MemorySegment.class, long.class, MemorySegment.class));
        } catch (ReflectiveOperationException ex) {
            throw new ExceptionInInitializerError(ex);
        }
    }

    private static void handleReplyCallback(int result,
                                            MemorySegment parts,
                                            long partCount,
                                            MemorySegment userData) {
        long requestId = userData.address();
        DirectReplyState directState = DIRECT_PENDING.remove(requestId);
        if (directState != null) {
            completeDirect(directState, result, parts, partCount);
            return;
        }

        completeFuture(PENDING.remove(requestId), result, parts, partCount);
    }

    private static void completeDirect(DirectReplyState state,
                                       int result,
                                       MemorySegment parts,
                                       long partCount) {
        state.cancelTimeout();
        try {
            if (result != RequestResult.OK.value()) {
                state.complete(RequestResult.fromValue(result), List.of());
                return;
            }

            List<Message> replyParts;
            try {
                Message[] frames = InternalAccess.messageFromOwnedMessageVectorShared(
                    parts, partCount);
                replyParts = frames.length == 0
                    ? List.of()
                    : Arrays.asList(frames);
                state.complete(RequestResult.OK, replyParts, frames);
                return;
            } catch (Throwable error) {
                state.complete(RequestResult.PROTOCOL_ERROR, List.of());
                return;
            }
        } finally {
            NativeMessage.multipartClose(parts, partCount);
        }
    }

    private static void completeFuture(CompletableFuture<Received> future,
                                       int result,
                                       MemorySegment parts,
                                       long partCount) {
        try {
            if (result != RequestResult.OK.value()) {
                if (future != null) {
                    RequestReplySupport.completeExceptionallyAsync(future,
                        new ZlinkRequestException(RequestResult.fromValue(result),
                            result));
                }
                return;
            }
            if (future != null) {
                Message[] frames = InternalAccess.messageFromOwnedMessageVectorShared(
                    parts, partCount);
                RequestReplySupport.completeAsync(future,
                    () -> InternalAccess.received((RoutingId) null, frames,
                        0L, false, null));
            }
        } catch (Throwable error) {
            if (future != null) {
                RequestReplySupport.completeExceptionallyAsync(future, error);
            }
        } finally {
            NativeMessage.multipartClose(parts, partCount);
        }
    }

    private static final class DirectReplyState {
        private final BiConsumer<RequestResult, List<Message>> callback;
        private final CompletableFuture<Void> progress = new CompletableFuture<>();
        private volatile ScheduledFuture<?> timeout;

        private DirectReplyState(BiConsumer<RequestResult, List<Message>> callback) {
            this.callback = callback;
        }

        private void cancelTimeout() {
            ScheduledFuture<?> scheduled = timeout;
            if (scheduled != null) {
                scheduled.cancel(false);
            }
        }

        private void complete(RequestResult result, List<Message> parts) {
            complete(result, parts, null);
        }

        private void complete(RequestResult result, List<Message> parts,
                              Message[] closeOnCallbackFailure) {
            // HOT PATH: callback-style request/reply must deliver the user
            // callback directly instead of materializing a Received object and
            // chaining a completion stage. The small progress future is only a
            // pump lifetime token; it is not part of user reply delivery.
            try {
                callback.accept(result, parts);
                progress.complete(null);
            } catch (Throwable error) {
                if (closeOnCallbackFailure != null) {
                    Message.closeAll(closeOnCallbackFailure);
                }
                progress.completeExceptionally(error);
            }
        }
    }
}
