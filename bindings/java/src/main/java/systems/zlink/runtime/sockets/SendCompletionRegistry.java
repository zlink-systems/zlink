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
import java.util.concurrent.ConcurrentHashMap;
import java.util.concurrent.ConcurrentMap;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.RejectedExecutionException;
import java.util.concurrent.atomic.AtomicLong;
import systems.zlink.contracts.errors.ErrorCategory;
import systems.zlink.contracts.errors.ZlinkException;
import systems.zlink.contracts.errors.ZlinkSubmitException;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.contracts.sockets.SubmitResult;
import systems.zlink.internal.DurationConversions;
import systems.zlink.runtime.nativeapi.InternalAccess;
import systems.zlink.runtime.nativeapi.MessagePartsBuffer;
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
    private static final Linker LINKER = Linker.nativeLinker();
    private static final FunctionDescriptor CALLBACK_DESCRIPTOR =
        FunctionDescriptor.ofVoid(ValueLayout.ADDRESS, ValueLayout.ADDRESS,
            ValueLayout.ADDRESS);
    private final NativeSocketRuntime socket;
    private final AtomicLong nextToken = new AtomicLong(1L);
    private final ConcurrentMap<Long, Pending> pending =
        new ConcurrentHashMap<>();
    private final ExecutorService completionExecutor =
        RuntimeResources.daemonSingleThreadExecutor("zlink-send-completion");
    private final Arena callbackArena = Arena.ofShared();
    private final MemorySegment callback;

    SendCompletionRegistry(NativeSocketRuntime socket) {
        this.socket = Objects.requireNonNull(socket, "socket");
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
            RuntimeResources.shutdownExecutor(completionExecutor);
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
        if (sourceParts.isEmpty()) {
            throw new IllegalArgumentException("parts must not be empty");
        }
        for (int i = 0; i < sourceParts.size(); i++) {
            Objects.requireNonNull(sourceParts.get(i), "parts[" + i + "]");
        }
        if (timeout != null && timeout.isNegative()) {
            throw new IllegalArgumentException("timeout must not be negative");
        }

        MessagePartsBuffer materializer = new MessagePartsBuffer();
        for (Message part : sourceParts) {
            materializer.add(part);
        }

        long token = nextToken();
        Pending operation = new Pending(token);
        PendingFuture future = new PendingFuture(operation);
        operation.future = future;

        pending.put(token, operation);
        try (Arena arena = Arena.ofConfined()) {
            MemorySegment nativeParts = materializer.transferToNativeArray(arena);
            MemorySegment options = arena.allocate(
                NativeLayouts.SEND_ASYNC_OPTIONS_LAYOUT);
            options.set(ValueLayout.JAVA_INT,
                NativeLayouts.SEND_ASYNC_OPTIONS_STRUCT_SIZE_OFFSET,
                (int) NativeLayouts.SEND_ASYNC_OPTIONS_LAYOUT.byteSize());
            options.set(ValueLayout.JAVA_INT,
                NativeLayouts.SEND_ASYNC_OPTIONS_TIMEOUT_MS_OFFSET,
                DurationConversions.timeoutMillisOrZero(timeout));
            options.set(ValueLayout.ADDRESS,
                NativeLayouts.SEND_ASYNC_OPTIONS_USERDATA_OFFSET,
                MemorySegment.ofAddress(token));
            options.set(ValueLayout.ADDRESS,
                NativeLayouts.SEND_ASYNC_OPTIONS_TARGET_OFFSET,
                target == null ? MemorySegment.NULL : target);
            MemorySegment opIdOut = arena.allocate(ValueLayout.JAVA_LONG);
            opIdOut.set(ValueLayout.JAVA_LONG, 0L, 0L);

            int result;
            try {
                result = Native.sendAsync(socket.handle(), nativeParts,
                    sourceParts.size(), options, opIdOut);
            } catch (Throwable failure) {
                pending.remove(token, operation);
                materializer.restoreFromNativeArray(nativeParts,
                    sourceParts.size());
                throw failure;
            }
            if (result != SubmitResult.OK.value()) {
                pending.remove(token, operation);
                materializer.restoreFromNativeArray(nativeParts,
                    sourceParts.size());
                throw submitFailure(result, Native.errno());
            }

            operation.opId = opIdOut.get(ValueLayout.JAVA_LONG, 0L);
            if (operation.opId == 0L) {
                // Core admitted the record synchronously and deliberately
                // does not emit a completion callback for this operation.
                if (pending.remove(token, operation)) {
                    future.complete(null);
                }
            } else if (future.isCancelled()
                       && pending.get(token) == operation) {
                cancel(operation);
            }
        }
        return future;
    }

    private void cancel(Pending operation) {
        long opId = operation.opId;
        if (opId == 0L || pending.get(operation.token) != operation) {
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
            Pending operation = pending.remove(token);
            if (operation == null) {
                return;
            }
            int result = event.get(ValueLayout.JAVA_INT,
                NativeLayouts.SEND_COMPLETE_RESULT_OFFSET);
            int errno = event.get(ValueLayout.JAVA_INT,
                NativeLayouts.SEND_COMPLETE_ERRNO_OFFSET);
            try {
                completionExecutor.execute(() -> complete(operation, result,
                    errno));
            } catch (RejectedExecutionException failure) {
                operation.future.completeExceptionally(
                    new ZlinkSubmitException(SubmitResult.TERMINATED,
                        NativeErrno.ECANCELED));
            }
        } catch (Throwable ignored) {
            // A foreign callback must never unwind through the Core boundary.
        }
    }

    private static void complete(Pending operation, int result, int errno) {
        if (result == 0) {
            operation.future.complete(null);
        } else {
            operation.future.completeExceptionally(
                new ZlinkSubmitException(SubmitResult.NOT_ADMITTED,
                    result == 201 && errno == 0
                        ? NativeErrno.ETIMEDOUT : errno));
        }
    }

    void dispatchCompletion(Runnable completion) {
        Objects.requireNonNull(completion, "completion");
        completionExecutor.execute(completion);
    }

    @Override
    public void close() {
        // Native socket close succeeds before this cleanup runs, so Core
        // rejects any submit that has not entered yet. Such submitters remove
        // their own pending entry when that rejection is returned.
        List<Pending> abandoned = new ArrayList<>();
        pending.forEach((token, operation) -> {
            if (pending.remove(token, operation)) {
                abandoned.add(operation);
            }
        });
        for (Pending operation : abandoned) {
            operation.future.completeExceptionally(
                new ZlinkSubmitException(SubmitResult.TERMINATED,
                    NativeErrno.ECANCELED));
        }
        RuntimeResources.closeArena(callbackArena);
        RuntimeResources.shutdownExecutor(completionExecutor);
    }

    private long nextToken() {
        long token;
        do {
            token = nextToken.getAndIncrement();
        } while (token == 0L);
        return token;
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

    private static final class Pending {
        private final long token;
        private volatile long opId;
        private CompletableFuture<Void> future;

        private Pending(long token) {
            this.token = token;
        }
    }
}
