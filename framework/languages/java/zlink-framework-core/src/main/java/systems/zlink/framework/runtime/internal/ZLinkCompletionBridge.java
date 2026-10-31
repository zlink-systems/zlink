package systems.zlink.framework.runtime.internal;

import java.util.Objects;
import java.util.concurrent.CancellationException;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionException;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.Executor;
import java.util.concurrent.ForkJoinPool;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicReferenceFieldUpdater;
import java.util.function.BiConsumer;
import java.util.function.Consumer;
import java.util.function.Supplier;

/** Bridges caller cancellation to waiter stages and disposes results the caller cannot accept. */
public final class ZLinkCompletionBridge {
    private ZLinkCompletionBridge() {}

    /** Preserves direct failures at existing synchronous state-registration boundaries. */
    public static <T> T await(CompletionStage<T> completion) {
        try {
            return completion.toCompletableFuture().join();
        } catch (CompletionException failure) {
            if (failure.getCause() instanceof RuntimeException runtime) throw runtime;
            if (failure.getCause() instanceof Error error) throw error;
            throw failure;
        }
    }

    public static void forwardCancellation(CompletableFuture<?> caller, CompletionStage<?> waiter) {
        if (caller instanceof CancellationFuture<?> cancellable
                && waiter instanceof CompletableFuture<?> future
                && CancellationFuture.canCancelDirectly(future)
                && cancellable.forwardTo(future)) {
            return;
        }
        forwardCancellation(caller, waiter, null);
    }

    public static void forwardCancellation(
            CompletableFuture<?> caller, CompletionStage<?> first, CompletionStage<?> second) {
        forwardCancellation(
                caller,
                first,
                second,
                (firstWaiter, secondWaiter) -> {
                    firstWaiter.toCompletableFuture().cancel(false);
                    if (secondWaiter != null) secondWaiter.toCompletableFuture().cancel(false);
                });
    }

    /** Runs the forwarding action when cancellation must be scheduled on the waiter's owner. */
    public static void forwardCancellation(CompletableFuture<?> caller, Runnable forward) {
        forwardCancellation(caller, forward, null, (action, ignored) -> action.run());
    }

    private static <T, U> void forwardCancellation(
            CompletableFuture<?> caller, T first, U second, BiConsumer<T, U> action) {
        caller.whenComplete(
                (ignored, failure) -> {
                    if (failure instanceof CancellationException) action.accept(first, second);
                });
    }

    public static <T> void completeOrDiscard(
            CompletableFuture<T> caller, T value, Consumer<? super T> discard) {
        if (!caller.complete(value)) discard.accept(value);
    }

    /** Creates a terminal future that can forward cancellation without a dependent stage. */
    public static <T> CompletableFuture<T> newCancellationFuture() {
        return new CancellationFuture<>(null);
    }

    /** Creates a terminal future with a side effect that does not need its own dependent stage. */
    public static <T> CompletableFuture<T> newCancellationFuture(
            BiConsumer<CompletableFuture<T>, Throwable> terminalObserver) {
        return new CancellationFuture<>(
                Objects.requireNonNull(terminalObserver, "terminalObserver"));
    }

    private static final class CancellationFuture<T> extends CompletableFuture<T> {
        private static final Object FINISHED = new Object();
        private static final CancellationException CANCELLED = new CancellationException();
        private static final AtomicReferenceFieldUpdater<CancellationFuture, Object> FORWARDER =
                AtomicReferenceFieldUpdater.newUpdater(
                        CancellationFuture.class, Object.class, "forwarder");

        private volatile Object forwarder;
        private volatile BiConsumer<CompletableFuture<T>, Throwable> terminalObserver;

        private CancellationFuture(BiConsumer<CompletableFuture<T>, Throwable> terminalObserver) {
            this.terminalObserver = terminalObserver;
        }

        static boolean canCancelDirectly(CompletableFuture<?> waiter) {
            return waiter.getClass() == CompletableFuture.class
                    || waiter.getClass() == CancellationFuture.class;
        }

        boolean forwardTo(CompletableFuture<?> waiter) {
            return FORWARDER.compareAndSet(this, null, waiter);
        }

        @Override
        public boolean complete(T value) {
            boolean completed = super.complete(value);
            if (completed) finish(false, null);
            return completed;
        }

        @Override
        public boolean completeExceptionally(Throwable failure) {
            boolean completed = super.completeExceptionally(failure);
            if (completed) finish(failure instanceof CancellationException, failure);
            return completed;
        }

        @Override
        public boolean cancel(boolean mayInterruptIfRunning) {
            boolean cancelled = super.cancel(mayInterruptIfRunning);
            if (cancelled) finish(true, CANCELLED);
            return cancelled;
        }

        @Override
        public void obtrudeValue(T value) {
            boolean incomplete = !isDone();
            super.obtrudeValue(value);
            if (incomplete) finish(false, null);
        }

        @Override
        public void obtrudeException(Throwable failure) {
            boolean incomplete = !isDone();
            super.obtrudeException(failure);
            if (incomplete) finish(failure instanceof CancellationException, failure);
        }

        @Override
        public CompletableFuture<T> completeAsync(Supplier<? extends T> supplier) {
            return completeAsync(supplier, ForkJoinPool.commonPool());
        }

        @Override
        public CompletableFuture<T> completeAsync(
                Supplier<? extends T> supplier, Executor executor) {
            Objects.requireNonNull(supplier, "supplier");
            Objects.requireNonNull(executor, "executor");
            executor.execute(
                    () -> {
                        try {
                            complete(supplier.get());
                        } catch (Throwable failure) {
                            completeExceptionally(failure);
                        }
                    });
            return this;
        }

        @Override
        public CompletableFuture<T> orTimeout(long timeout, TimeUnit unit) {
            return super.orTimeout(timeout, unit);
        }

        @Override
        public CompletableFuture<T> completeOnTimeout(T value, long timeout, TimeUnit unit) {
            return super.completeOnTimeout(value, timeout, unit);
        }

        private void finish(boolean cancelWaiter, Throwable failure) {
            Object target = FORWARDER.getAndSet(this, FINISHED);
            if (target == FINISHED) return;
            if (cancelWaiter && target instanceof CompletableFuture<?> waiter) waiter.cancel(false);
            BiConsumer<CompletableFuture<T>, Throwable> observer = terminalObserver;
            terminalObserver = null;
            if (observer != null) observer.accept(this, failure);
        }
    }
}
