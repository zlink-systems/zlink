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
import java.util.concurrent.ArrayBlockingQueue;
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
    private final ConcurrentMap<Object, CompletableFuture<List<Message>>> pending =
        new ConcurrentHashMap<>();
    private final ThreadLocal<RequestIdLookup> requestIdLookup =
        ThreadLocal.withInitial(RequestIdLookup::new);
    private final CallbackLifecycle callbackLifecycle;
    private final ArrayBlockingQueue<MemorySegment> replySnapshots =
        new ArrayBlockingQueue<>(8);

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
        pending.put(requestId, future);
    }

    public void removeRoutedPending(long requestId) {
        removePending(requestId);
    }

    /** Removes and fails one pending request, used by socket close. */
    public boolean completePendingExceptionally(long requestId,
                                                Throwable failure) {
        CompletableFuture<List<Message>> future = removePending(requestId);
        if (future == null) {
            return false;
        }
        callbackLifecycle.enter();
        try {
            future.completeExceptionally(failure);
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
        CompletableFuture<List<Message>> future = removePending(requestId);
        if (future == null) {
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
                completion = () -> future.completeExceptionally(failure);
                rejectedCompletion = completion;
            } else {
                if (partCount == 2L
                    && NativeRequestBridge.snapshotPairAvailable()) {
                    MemorySegment snapshot = acquireReplySnapshot();
                    if (NativeRequestBridge.snapshotPair(parts, snapshot)
                        != 0) {
                        recycleReplySnapshot(snapshot);
                        throw new ZlinkRequestException(
                            RequestResult.INTERNAL_ERROR, Native.errno());
                    }
                    completion = () -> completeSnapshotPair(future, snapshot);
                    rejectedCompletion = () -> {
                        try {
                            NativeMessage.multipartClose(snapshot, 2L);
                            recycleReplySnapshot(snapshot);
                        } finally {
                            future.completeExceptionally(
                                new ZlinkRequestException(
                                    RequestResult.TERMINATED,
                                    NativeErrno.ECANCELED));
                        }
                    };
                } else {
                    List<Message> reply;
                    if (partCount == 2L) {
                        Message first = InternalAccess
                            .messageFromOwnedMessageVectorPartShared(parts,
                                true);
                        try {
                            MemorySegment secondAddress =
                                MemorySegment.ofAddress(parts.address()
                                    + NativeLayouts.MESSAGE_LAYOUT.byteSize());
                            Message second = InternalAccess
                                .messageFromOwnedMessageVectorPartShared(
                                    secondAddress, false);
                            reply = List.of(first, second);
                        } catch (Throwable failure) {
                            first.close();
                            throw failure;
                        }
                    } else {
                        Message[] frames = InternalAccess
                            .messageFromOwnedMessageVectorShared(parts,
                                partCount);
                        reply = frames.length == 0
                            ? List.of() : Arrays.asList(frames);
                    }
                    completion = () -> {
                        if (!future.complete(reply)) {
                            Message.closeAll(reply);
                        }
                    };
                    rejectedCompletion = () -> {
                        try {
                            Message.closeAll(reply);
                        } finally {
                            future.completeExceptionally(
                                new ZlinkRequestException(
                                    RequestResult.TERMINATED,
                                    NativeErrno.ECANCELED));
                        }
                    };
                }
            }
        } catch (Throwable failure) {
            completion = () -> future.completeExceptionally(failure);
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

    private void completeSnapshotPair(
            CompletableFuture<List<Message>> future,
            MemorySegment snapshot) {
        Message first = null;
        Message second = null;
        try {
            first = InternalAccess.messageFromOwnedMessageVectorPartShared(
                snapshot, true);
            second = InternalAccess.messageFromOwnedMessageVectorPartShared(
                snapshot.asSlice(NativeLayouts.MESSAGE_LAYOUT.byteSize()),
                false);
            List<Message> reply = List.of(first, second);
            if (!future.complete(reply)) {
                Message.closeAll(reply);
            }
        } catch (Throwable failure) {
            if (first != null) {
                first.close();
            }
            if (second != null) {
                second.close();
            }
            future.completeExceptionally(failure);
        } finally {
            NativeMessage.multipartClose(snapshot, 2L);
            recycleReplySnapshot(snapshot);
        }
    }

    private MemorySegment acquireReplySnapshot() {
        MemorySegment snapshot = replySnapshots.poll();
        return snapshot != null ? snapshot : callbackArena.allocate(
            NativeLayouts.MESSAGE_LAYOUT, 2L);
    }

    private void recycleReplySnapshot(MemorySegment snapshot) {
        replySnapshots.offer(snapshot);
    }

    private static RequestResult requestResult(int value) {
        try {
            return RequestResult.fromValue(value);
        } catch (IllegalArgumentException ignored) {
            return RequestResult.INTERNAL_ERROR;
        }
    }

    private CompletableFuture<List<Message>> removePending(long requestId) {
        RequestIdLookup lookup = requestIdLookup.get();
        lookup.value = requestId;
        return pending.remove(lookup);
    }

    public void close(Throwable failure) {
        Objects.requireNonNull(failure, "failure");
        pending.forEach((requestId, future) ->
            completePendingExceptionally(((Long) requestId).longValue(),
                failure));
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

    /** Allocation-free lookup key for the Long keys retained by pending. */
    private static final class RequestIdLookup {
        private long value;

        @Override
        public int hashCode() {
            return Long.hashCode(value);
        }

        @Override
        public boolean equals(Object other) {
            return other instanceof Long key && key.longValue() == value;
        }
    }

}
