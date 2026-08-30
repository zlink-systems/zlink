/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.runtime.sockets;

import java.lang.foreign.Arena;
import java.lang.foreign.FunctionDescriptor;
import java.lang.foreign.Linker;
import java.lang.foreign.MemorySegment;
import java.lang.foreign.ValueLayout;
import java.lang.invoke.MethodHandle;
import java.lang.invoke.MethodHandles;
import java.lang.invoke.MethodType;
import java.time.Duration;
import java.util.ArrayList;
import java.util.List;
import java.util.Objects;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.ArrayBlockingQueue;
import java.util.concurrent.ConcurrentHashMap;
import java.util.concurrent.ConcurrentMap;
import java.util.concurrent.atomic.AtomicLong;
import java.util.concurrent.atomic.AtomicReference;
import systems.zlink.contracts.errors.ErrorCategory;
import systems.zlink.contracts.errors.ZlinkException;
import systems.zlink.contracts.errors.ZlinkSubmitException;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.contracts.sockets.SubmitResult;
import systems.zlink.internal.DurationConversions;
import systems.zlink.runtime.nativeapi.InternalAccess;
import systems.zlink.runtime.nativeapi.CompletionDispatcher;
import systems.zlink.runtime.nativeapi.Native;
import systems.zlink.runtime.nativeapi.NativeErrno;
import systems.zlink.runtime.nativeapi.NativeLayouts;
import systems.zlink.runtime.nativeapi.RuntimeResources;

/**
 * One socket-local Core send-completion handler and its strong pending table.
 *
 * <p>The table is keyed by a binding-owned token, rather than the Core op id:
 * Core may invoke the completion inline before {@code zlink_send_async}
 * returns its op id. The foreign callback only snapshots the event and queues
 * stage completion; it never retries or calls user continuations on a Core
 * thread.
 */
final class SendCompletionRegistry implements AutoCloseable {
    private static final int CACHED_PART_CAPACITY = 4;
    private static final long FAST_TOKEN = 1L;
    private static final MemorySegment FAST_USERDATA =
        MemorySegment.ofAddress(FAST_TOKEN);
    private static final CompletionStage<Void> COMPLETED_STAGE =
        CompletableFuture.completedStage(null);
    private static final Linker LINKER = Linker.nativeLinker();
    private static final FunctionDescriptor CALLBACK_DESCRIPTOR =
        FunctionDescriptor.ofVoid(ValueLayout.ADDRESS, ValueLayout.ADDRESS,
            ValueLayout.ADDRESS);
    private static final ThreadLocal<SubmitScratch> SUBMIT_SCRATCH =
        ThreadLocal.withInitial(SubmitScratch::new);
    private final NativeSocketRuntime socket;
    private final AtomicLong nextToken = new AtomicLong(FAST_TOKEN + 1L);
    private final AtomicReference<Pending> fastPending =
        new AtomicReference<>();
    private final ConcurrentMap<Long, Pending> overflowPending =
        new ConcurrentHashMap<>();
    private final ArrayBlockingQueue<Pending> pendingPool =
        new ArrayBlockingQueue<>(32);
    private final CompletionDispatcher.CompletionLane completionLane;
    private final Arena callbackArena = Arena.ofShared();
    private final MemorySegment callback;

    SendCompletionRegistry(NativeSocketRuntime socket,
                           CompletionDispatcher.CompletionLane completionLane) {
        this.socket = Objects.requireNonNull(socket, "socket");
        this.completionLane = Objects.requireNonNull(completionLane,
            "completionLane");
        try {
            callback = LINKER.upcallStub(callbackHandle(), CALLBACK_DESCRIPTOR,
                callbackArena);
            int rc = Native.sendCompleteHandler(socket.handle(), callback,
                MemorySegment.NULL);
            if (rc != 0) {
                throw ZlinkException.fromLastError(ErrorCategory.HANDLER);
            }
        } catch (Throwable failure) {
            RuntimeResources.closeArena(callbackArena);
            if (failure instanceof RuntimeException runtimeFailure) {
                throw runtimeFailure;
            }
            throw new IllegalStateException(
                "failed to install Core send completion handler", failure);
        }
    }

    CompletionStage<Void> submit(List<Message> sourceParts,
                                 Duration timeout,
                                 MemorySegment target) {
        Objects.requireNonNull(sourceParts, "sourceParts");
        int partCount = sourceParts.size();
        if (partCount == 0) {
            throw new IllegalArgumentException("parts must not be empty");
        }
        for (int i = 0; i < partCount; i++) {
            Objects.requireNonNull(sourceParts.get(i),
                "parts[" + i + "]");
        }
        if (timeout != null && timeout.isNegative()) {
            throw new IllegalArgumentException("timeout must not be negative");
        }

        Pending operation = acquirePending();

        SubmitScratch scratch = partCount <= CACHED_PART_CAPACITY
            ? SUBMIT_SCRATCH.get() : null;
        if (scratch != null && scratch.tryAcquire()) {
            try {
                return submitWithStorage(sourceParts, partCount, timeout, target,
                    operation, scratch.options,
                    scratch.opIdOut, scratch.nativeParts, scratch);
            } finally {
                scratch.release();
            }
        } else {
            try (Arena arena = Arena.ofConfined()) {
                return submitWithStorage(sourceParts, partCount, timeout, target,
                    operation,
                    arena.allocate(NativeLayouts.SEND_ASYNC_OPTIONS_LAYOUT),
                    arena.allocate(ValueLayout.JAVA_LONG),
                    arena.allocate(NativeLayouts.MESSAGE_LAYOUT, partCount),
                    null);
            }
        }
    }

    private CompletionStage<Void> submitWithStorage(
            List<Message> sourceParts,
            int partCount,
            Duration timeout,
            MemorySegment target,
            Pending operation,
            MemorySegment options,
            MemorySegment opIdOut,
            MemorySegment nativeParts,
            SubmitScratch scratch) {
        options.set(ValueLayout.JAVA_INT,
            NativeLayouts.SEND_ASYNC_OPTIONS_STRUCT_SIZE_OFFSET,
            (int) NativeLayouts.SEND_ASYNC_OPTIONS_LAYOUT.byteSize());
        options.set(ValueLayout.JAVA_INT,
            NativeLayouts.SEND_ASYNC_OPTIONS_TIMEOUT_MS_OFFSET,
            DurationConversions.timeoutMillisOrZero(timeout));
        options.set(ValueLayout.ADDRESS,
            NativeLayouts.SEND_ASYNC_OPTIONS_TARGET_OFFSET,
            target == null ? MemorySegment.NULL : target);
        opIdOut.set(ValueLayout.JAVA_LONG, 0L, 0L);
        transferToNativeArray(sourceParts, nativeParts, partCount, scratch);

        // Register immediately before the Core call. This preserves the
        // inline-completion guarantee while keeping preparation failures out
        // of the pending table.
        registerPending(operation);
        options.set(ValueLayout.ADDRESS,
            NativeLayouts.SEND_ASYNC_OPTIONS_USERDATA_OFFSET,
            operation.fastSlot
                ? FAST_USERDATA : MemorySegment.ofAddress(operation.token));

        int result;
        try {
            result = Native.sendAsync(socket.handle(), nativeParts,
                partCount, options, opIdOut);
        } catch (Throwable failure) {
            removePending(operation);
            restoreFromNativeArray(sourceParts, nativeParts, partCount);
            recyclePending(operation);
            throw failure;
        }
        if (result != SubmitResult.OK.value()) {
            int errno = Native.errno();
            removePending(operation);
            restoreFromNativeArray(sourceParts, nativeParts, partCount);
            recyclePending(operation);
            throw submitFailure(result, errno);
        }

        operation.opId = opIdOut.get(ValueLayout.JAVA_LONG, 0L);
        if (operation.opId == 0L) {
            // Core admitted the record synchronously and deliberately does
            // not emit a completion callback for this operation.
            if (removePending(operation)) {
                recyclePending(operation);
                return COMPLETED_STAGE;
            }
        }
        return operation.attachFuture();
    }

    private static void transferToNativeArray(
            List<Message> parts,
            MemorySegment nativeParts,
            int partCount,
            SubmitScratch scratch) {
        long stride = NativeLayouts.MESSAGE_LAYOUT.byteSize();
        int moved = 0;
        try {
            for (int i = 0; i < partCount; i++) {
                InternalAccess.messageTransferTo(parts.get(i),
                    scratch == null
                        ? nativeParts.asSlice(i * stride, stride)
                        : scratch.part(i));
                moved++;
            }
        } catch (RuntimeException | Error failure) {
            restoreFromNativeArray(parts, nativeParts, moved);
            throw failure;
        }
    }

    private static void restoreFromNativeArray(
            List<Message> parts,
            MemorySegment nativeParts,
            int count) {
        if (nativeParts == MemorySegment.NULL || count <= 0) {
            return;
        }
        int bounded = Math.min(count, parts.size());
        long stride = NativeLayouts.MESSAGE_LAYOUT.byteSize();
        for (int i = 0; i < bounded; i++) {
            InternalAccess.messageRestoreFromNative(parts.get(i),
                nativeParts.asSlice(i * stride, stride), i + 1 < bounded,
                null);
        }
    }

    private void cancel(Pending operation) {
        long opId = operation.opId;
        if (opId == 0L || !isPending(operation)) {
            return;
        }
        try {
            Native.sendAsyncCancel(socket.handle(), opId);
        } catch (RuntimeException ignored) {
            // Core still owns the operation and will deliver its one completion.
        }
    }

    private void handleCompletion(MemorySegment subject,
                                  MemorySegment eventAddress,
                                  MemorySegment callbackUserdata) {
        if (eventAddress == null || eventAddress.address() == 0L) {
            return;
        }
        try {
            MemorySegment event = eventAddress.reinterpret(
                NativeLayouts.SEND_COMPLETE_EVENT_LAYOUT.byteSize());
            MemorySegment userdata = event.get(ValueLayout.ADDRESS,
                NativeLayouts.SEND_COMPLETE_USERDATA_OFFSET);
            long token = userdata == null ? 0L : userdata.address();
            Pending operation = removePending(token);
            if (operation == null) {
                return;
            }
            int result = event.get(ValueLayout.JAVA_INT,
                NativeLayouts.SEND_COMPLETE_RESULT_OFFSET);
            int errno = event.get(ValueLayout.JAVA_INT,
                NativeLayouts.SEND_COMPLETE_ERRNO_OFFSET);
            completionLane.dispatch(() -> complete(operation, result,
                errno));
        } catch (Throwable ignored) {
            // A foreign callback must never unwind through the Core boundary.
        }
    }

    private void complete(Pending operation, int result, int errno) {
        operation.publish(result, errno);
    }

    void dispatchCompletion(Runnable completion) {
        completionLane.dispatch(completion);
    }

    @Override
    public void close() {
        // Native socket close succeeds before this cleanup runs, so Core
        // rejects any submit that has not entered yet. Such submitters remove
        // their own pending entry when that rejection is returned.
        List<Pending> abandoned = new ArrayList<>();
        Pending fast = fastPending.getAndSet(null);
        if (fast != null) {
            abandoned.add(fast);
        }
        overflowPending.forEach((token, operation) -> {
            if (overflowPending.remove(token, operation)) {
                abandoned.add(operation);
            }
        });
        if (!abandoned.isEmpty()) {
            // Preserve the socket's completion order during teardown. Core
            // callbacks already queued on this lane must complete before the
            // operations abandoned by native socket close.
            completionLane.dispatch(() -> completeAbandoned(abandoned));
        }
        RuntimeResources.closeArena(callbackArena);
    }

    private static void completeAbandoned(List<Pending> abandoned) {
        for (Pending operation : abandoned) {
            operation.publish(SubmitResult.TERMINATED.value(),
                NativeErrno.ECANCELED);
        }
    }

    private long nextToken() {
        long token;
        do {
            token = nextToken.getAndIncrement();
        } while (token == 0L || token == FAST_TOKEN);
        return token;
    }

    private void registerPending(Pending operation) {
        operation.token = FAST_TOKEN;
        operation.fastSlot = true;
        if (fastPending.compareAndSet(null, operation)) {
            return;
        }
        operation.token = nextToken();
        operation.fastSlot = false;
        overflowPending.put(operation.token, operation);
    }

    private boolean isPending(Pending operation) {
        return operation.fastSlot
            ? fastPending.get() == operation
            : overflowPending.get(operation.token) == operation;
    }

    private boolean removePending(Pending operation) {
        return operation.fastSlot
            ? fastPending.compareAndSet(operation, null)
            : overflowPending.remove(operation.token, operation);
    }

    private Pending removePending(long token) {
        Pending fast = fastPending.get();
        if (fast != null && fast.token == token) {
            return fastPending.compareAndSet(fast, null) ? fast : null;
        }
        return overflowPending.remove(token);
    }

    private static ZlinkSubmitException submitFailure(int result, int errno) {
        try {
            SubmitResult mapped = SubmitResult.fromValue(result);
            return new ZlinkSubmitException(mapped, errno);
        } catch (IllegalArgumentException ignored) {
            return (ZlinkSubmitException) ZlinkException.fromErrno(
                ErrorCategory.SUBMIT, errno);
        }
    }

    private Pending acquirePending() {
        Pending operation = pendingPool.poll();
        if (operation == null) {
            operation = new Pending();
        } else {
            operation.reset();
        }
        return operation;
    }

    private void recyclePending(Pending operation) {
        pendingPool.offer(operation);
    }

    private MethodHandle callbackHandle() {
        try {
            return MethodHandles.lookup().findVirtual(
                SendCompletionRegistry.class, "handleCompletion",
                MethodType.methodType(void.class, MemorySegment.class,
                    MemorySegment.class, MemorySegment.class))
                .bindTo(this);
        } catch (ReflectiveOperationException failure) {
            throw new ExceptionInInitializerError(failure);
        }
    }

    private final class PendingFuture extends CompletableFuture<Void> {
        private final Pending operation;

        private PendingFuture(Pending operation) {
            this.operation = operation;
        }

        @Override
        public boolean cancel(boolean mayInterruptIfRunning) {
            boolean cancelled = super.cancel(mayInterruptIfRunning);
            if (cancelled) {
                SendCompletionRegistry.this.cancel(operation);
            }
            return cancelled;
        }
    }

    private final class Pending {
        private long token;
        private boolean fastSlot;
        private volatile long opId;
        private PendingFuture future;
        private int terminalResult = Integer.MIN_VALUE;
        private int terminalErrno;

        private Pending() {
        }

        private PendingFuture attachFuture() {
            PendingFuture attached;
            int result;
            int errno;
            synchronized (this) {
                if (future != null) {
                    return future;
                }
                attached = new PendingFuture(this);
                future = attached;
                result = terminalResult;
                errno = terminalErrno;
            }
            if (result != Integer.MIN_VALUE) {
                completeFuture(attached, result, errno);
                recyclePending(this);
            }
            return attached;
        }

        private void publish(int result, int errno) {
            PendingFuture attached;
            synchronized (this) {
                if (terminalResult != Integer.MIN_VALUE) {
                    return;
                }
                terminalResult = result;
                terminalErrno = errno;
                attached = future;
            }
            if (attached != null) {
                completeFuture(attached, result, errno);
                recyclePending(this);
            }
        }

        private void reset() {
            token = 0L;
            fastSlot = false;
            opId = 0L;
            future = null;
            terminalResult = Integer.MIN_VALUE;
            terminalErrno = 0;
        }
    }

    private static void completeFuture(PendingFuture future, int result,
                                       int errno) {
        if (result == 0) {
            future.complete(null);
        } else {
            future.completeExceptionally(
                new ZlinkSubmitException(
                    result == SubmitResult.TERMINATED.value()
                        ? SubmitResult.TERMINATED
                        : SubmitResult.NOT_ADMITTED,
                    result == 201 && errno == 0
                        ? NativeErrno.ETIMEDOUT : errno));
        }
    }

    /** Per-caller reusable native submit structures for common multipart sends. */
    private static final class SubmitScratch {
        private final Arena arena = Arena.ofAuto();
        private final MemorySegment options = arena.allocate(
            NativeLayouts.SEND_ASYNC_OPTIONS_LAYOUT);
        private final MemorySegment opIdOut = arena.allocate(
            ValueLayout.JAVA_LONG);
        private final MemorySegment nativeParts = arena.allocate(
            NativeLayouts.MESSAGE_LAYOUT, CACHED_PART_CAPACITY);
        private final MemorySegment[] partSlices = createPartSlices();
        private boolean inUse;

        boolean tryAcquire() {
            if (inUse) {
                return false;
            }
            inUse = true;
            return true;
        }

        MemorySegment part(int index) {
            return partSlices[index];
        }

        void release() {
            inUse = false;
        }

        private MemorySegment[] createPartSlices() {
            MemorySegment[] slices =
                new MemorySegment[CACHED_PART_CAPACITY];
            long stride = NativeLayouts.MESSAGE_LAYOUT.byteSize();
            for (int i = 0; i < slices.length; i++) {
                slices[i] = nativeParts.asSlice(i * stride, stride);
            }
            return slices;
        }
    }
}
