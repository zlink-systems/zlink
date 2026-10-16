package systems.zlink.framework.runtime.internal.service;

import systems.zlink.framework.runtime.internal.ZLinkCompletionBridge;

import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.ForkJoinPool;
import java.util.function.BiConsumer;
import java.util.logging.Level;
import java.util.logging.Logger;

/** Owns independent process-pool turns for Framework completion delivery. */
public final class ZLinkServiceCompletionDispatcher {
    private static final Logger LOGGER =
            Logger.getLogger(ZLinkServiceCompletionDispatcher.class.getName());
    static final ZLinkServiceCompletionDispatcher INSTANCE = new ZLinkServiceCompletionDispatcher();
    private static final ThreadLocal<Boolean> CURRENT_COMPLETION = new ThreadLocal<>();

    private ZLinkServiceCompletionDispatcher() {}

    public static boolean isCurrentCompletion() {
        return Boolean.TRUE.equals(CURRENT_COMPLETION.get());
    }

    /** Reuses an existing completion turn; other sources start an independent pool turn. */
    public static <T> CompletionStage<T> deliver(CompletionStage<T> source) {
        if (source.toCompletableFuture().isDone()) return source;
        var caller = new CompletableFuture<T>();
        deliver(source, caller);
        return caller;
    }

    /** Connects a caller view only after its owner has published that shared view. */
    public static <T> void deliver(CompletionStage<T> source, CompletableFuture<T> caller) {
        Delivery<T> delivery = new Delivery<>(source, caller);
        INSTANCE.register(delivery);
        source.whenComplete(delivery);
    }

    void register(WorkItem item) {
        if (item.registered) {
            throw new IllegalStateException("completion work item is already registered");
        }
        item.registered = true;
    }

    void releaseWithoutDispatch(WorkItem item) {
        releaseRegistration(item);
    }

    void post(WorkItem item) {
        requireRegistered(item);
        ForkJoinPool.commonPool().execute(item);
    }

    void postChain(WorkItem first, WorkItem last) {
        if (first == null) return;
        WorkItem current = first;
        while (true) {
            requireRegistered(current);
            if (current == last) break;
            current = current.dispatchNext;
            if (current == null) {
                throw new IllegalStateException("completion work chain does not reach its tail");
            }
        }
        current = first;
        while (true) {
            // Posting can run, release and reuse this entry before post returns.
            WorkItem next = current.dispatchNext;
            current.dispatchNext = null;
            boolean end = current == last;
            post(current);
            if (end) break;
            current = next;
        }
    }

    private static void releaseRegistration(WorkItem item) {
        requireRegistered(item);
        item.registered = false;
        item.released();
    }

    private static void requireRegistered(WorkItem item) {
        if (!item.registered) {
            throw new IllegalStateException("completion work item has no dispatcher registration");
        }
    }

    abstract static class WorkItem implements Runnable {
        private WorkItem dispatchNext;
        private volatile boolean registered;

        final void setDispatchNext(WorkItem value) {
            dispatchNext = value;
        }

        @Override
        public final void run() {
            Boolean previous = CURRENT_COMPLETION.get();
            CURRENT_COMPLETION.set(true);
            try {
                try {
                    dispatch();
                } catch (RuntimeException | Error failure) {
                    LOGGER.log(Level.SEVERE, "service completion dispatch failed", failure);
                } finally {
                    releaseRegistration(this);
                }
            } finally {
                if (previous == null) CURRENT_COMPLETION.remove();
                else CURRENT_COMPLETION.set(previous);
            }
        }

        abstract void dispatch();

        void released() {}
    }

    private static final class Delivery<T> extends WorkItem implements BiConsumer<T, Throwable> {
        private final CompletableFuture<T> caller;
        private T value;
        private Throwable failure;

        private Delivery(CompletionStage<T> source, CompletableFuture<T> caller) {
            this.caller = caller;
            ZLinkCompletionBridge.forwardCancellation(caller, source);
        }

        @Override
        public void accept(T value, Throwable failure) {
            this.value = value;
            this.failure = failure;
            if (isCurrentCompletion()) run();
            else INSTANCE.post(this);
        }

        @Override
        void dispatch() {
            if (failure == null) caller.complete(value);
            else caller.completeExceptionally(failure);
        }
    }
}
