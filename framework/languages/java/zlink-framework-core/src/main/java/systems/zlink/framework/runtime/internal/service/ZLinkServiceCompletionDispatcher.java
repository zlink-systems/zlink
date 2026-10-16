package systems.zlink.framework.runtime.internal.service;

import systems.zlink.framework.runtime.internal.calls.ZLinkBlockingCalls;

import java.util.concurrent.atomic.AtomicReference;
import java.util.concurrent.locks.LockSupport;
import java.util.logging.Level;
import java.util.logging.Logger;

/**
 * Owns the process-wide execution lane for service operation terminals. A registered work item is
 * also its intrusive queue node, so a terminal winner never needs a second queue allocation.
 */
final class ZLinkServiceCompletionDispatcher {
    private static final Logger LOGGER =
            Logger.getLogger(ZLinkServiceCompletionDispatcher.class.getName());
    static final ZLinkServiceCompletionDispatcher INSTANCE = new ZLinkServiceCompletionDispatcher();

    private final Thread worker;
    // Producers publish pre-registered nodes; the worker detaches a complete rotation.
    // Reversing that rotation restores publication order without allocating queue nodes.
    private final AtomicReference<WorkItem> pending = new AtomicReference<>();

    private ZLinkServiceCompletionDispatcher() {
        worker =
                Thread.ofPlatform()
                        .daemon(true)
                        .name("zlink-jvm-service-completion")
                        .unstarted(this::run);
        worker.start();
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
        publish(item, item);
    }

    void postChain(WorkItem first, WorkItem last) {
        if (first == null) {
            return;
        }
        WorkItem current = first;
        while (true) {
            requireRegistered(current);
            if (current == last) break;
            current = current.dispatchNext;
            if (current == null) {
                throw new IllegalStateException("completion work chain does not reach its tail");
            }
        }
        last.dispatchNext = null;
        publish(reverse(first), first);
    }

    private void publish(WorkItem first, WorkItem last) {
        WorkItem observed;
        do {
            observed = pending.get();
            last.dispatchNext = observed;
        } while (!pending.compareAndSet(observed, first));
        // One wake per empty-to-nonempty transition. A producer racing with park
        // leaves its permit with the worker, so detaching a rotation needs no flag.
        if (observed == null) LockSupport.unpark(worker);
    }

    private void run() {
        ZLinkBlockingCalls.markInfrastructureThread();
        while (true) {
            WorkItem item = reverse(pending.getAndSet(null));
            if (item == null) {
                LockSupport.park(this);
                continue;
            }
            while (item != null) {
                WorkItem next = item.dispatchNext;
                item.dispatchNext = null;
                try {
                    item.dispatch();
                } catch (RuntimeException | Error failure) {
                    LOGGER.log(Level.SEVERE, "service completion dispatch failed", failure);
                } finally {
                    releaseRegistration(item);
                }
                item = next;
            }
        }
    }

    private static WorkItem reverse(WorkItem first) {
        WorkItem reversed = null;
        while (first != null) {
            WorkItem next = first.dispatchNext;
            first.dispatchNext = reversed;
            reversed = first;
            first = next;
        }
        return reversed;
    }

    private void releaseRegistration(WorkItem item) {
        requireRegistered(item);
        item.registered = false;
        item.released();
    }

    private static void requireRegistered(WorkItem item) {
        if (!item.registered) {
            throw new IllegalStateException("completion work item has no dispatcher registration");
        }
    }

    abstract static class WorkItem {
        private WorkItem dispatchNext;
        private volatile boolean registered;

        final void setDispatchNext(WorkItem value) {
            dispatchNext = value;
        }

        abstract void dispatch();

        void released() {}
    }
}
