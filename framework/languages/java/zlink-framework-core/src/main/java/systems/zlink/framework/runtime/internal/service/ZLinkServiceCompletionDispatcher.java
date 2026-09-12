package systems.zlink.framework.runtime.internal.service;

import java.util.concurrent.locks.LockSupport;

/**
 * Owns the process-wide execution lane for service operation terminals. A
 * registered work item is also its intrusive queue node, so a terminal winner
 * never needs a second queue allocation.
 */
final class ZLinkServiceCompletionDispatcher {
    static final ZLinkServiceCompletionDispatcher INSTANCE =
        new ZLinkServiceCompletionDispatcher();

    private final Object gate = new Object();
    private final Thread worker;
    private WorkItem head;
    private WorkItem tail;

    private ZLinkServiceCompletionDispatcher() {
        worker = Thread.ofPlatform()
            .daemon(true)
            .name("zlink-jvm-service-completion")
            .unstarted(this::run);
        worker.start();
    }

    void register(WorkItem item) {
        synchronized (gate) {
            if (item.registered) {
                throw new IllegalStateException(
                    "completion work item is already registered");
            }
            item.registered = true;
        }
    }

    void releaseWithoutDispatch(WorkItem item) {
        synchronized (gate) {
            releaseRegistration(item);
        }
    }

    void post(WorkItem item) {
        item.dispatchNext = null;
        synchronized (gate) {
            requireRegistered(item);
            if (tail == null) {
                head = item;
            } else {
                tail.dispatchNext = item;
            }
            tail = item;
        }
        LockSupport.unpark(worker);
    }

    void postChain(WorkItem first, WorkItem last) {
        if (first == null) {
            return;
        }
        synchronized (gate) {
            WorkItem current = first;
            while (true) {
                requireRegistered(current);
                if (current == last) {
                    break;
                }
                current = current.dispatchNext;
                if (current == null) {
                    throw new IllegalStateException(
                        "completion work chain does not reach its tail");
                }
            }
            if (tail == null) {
                head = first;
            } else {
                tail.dispatchNext = first;
            }
            tail = last;
        }
        LockSupport.unpark(worker);
    }

    private void run() {
        while (true) {
            WorkItem item = take();
            if (item == null) {
                LockSupport.park(this);
                continue;
            }
            try {
                item.dispatch();
            } catch (Throwable ignored) {
                // One application completion must not stop later completions.
            } finally {
                synchronized (gate) {
                    releaseRegistration(item);
                }
            }
        }
    }

    private WorkItem take() {
        synchronized (gate) {
            WorkItem item = head;
            if (item == null) {
                return null;
            }
            head = item.dispatchNext;
            item.dispatchNext = null;
            if (head == null) {
                tail = null;
            }
            return item;
        }
    }

    private void releaseRegistration(WorkItem item) {
        requireRegistered(item);
        item.registered = false;
    }

    private static void requireRegistered(WorkItem item) {
        if (!item.registered) {
            throw new IllegalStateException(
                "completion work item has no dispatcher registration");
        }
    }

    abstract static class WorkItem {
        private WorkItem dispatchNext;
        private boolean registered;

        final void setDispatchNext(WorkItem value) {
            dispatchNext = value;
        }

        final WorkItem getDispatchNext() {
            return dispatchNext;
        }

        abstract void dispatch();
    }
}
