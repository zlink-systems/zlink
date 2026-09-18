package systems.zlink.stream.connector;
import java.util.Objects;

import java.util.ArrayDeque;
import java.util.HashMap;
import java.util.ArrayList;
import java.util.Iterator;
import java.util.List;
import java.util.Map;
import java.util.Queue;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.function.BooleanSupplier;
import java.util.function.Consumer;
import java.util.function.Predicate;
import java.util.function.Supplier;

final class ZLinkStreamDispatchQueue {
    private final Queue<QueuedDispatch> queue = new ArrayDeque<>();
    private final Map<String, Integer> receivedCounts = new HashMap<>();
    private final List<Waiter> waiters = new ArrayList<>();
    private final Consumer<ZLinkStreamError> publishError;
    //  Guarded by queue. A long has no atomic non-volatile read (JLS 17.7),
    //  so reading it outside the monitor could observe a torn value - the
    //  very thing this counter exists to rule out.
    private long version;

    ZLinkStreamDispatchQueue() {
        this(ignored -> { });
    }

    ZLinkStreamDispatchQueue(Consumer<ZLinkStreamError> publishError) {
        this.publishError = publishError;
    }

    int size() {
        synchronized (queue) {
            return queue.size();
        }
    }

    void add(Runnable item) {
        add(null, item);
    }

    void add(String packetName, Runnable item) {
        addAsync(packetName, () -> {
            item.run();
            return CompletableFuture.completedFuture(null);
        });
    }

    void addAsync(Supplier<CompletionStage<Void>> item) {
        addAsync(null, item);
    }

    void addAsync(String packetName, Supplier<CompletionStage<Void>> item) {
        synchronized (queue) {
            queue.add(new QueuedDispatch(packetName, null, item, () -> true));
            version++;
        }
    }

    /**
     * Records one received packet and routes it.
     *
     * <p>Spec 32 10: {@code receivedCount(name)} counts what arrived. It is
     * counted here, once, at arrival - before a waiter, an immediate
     * dispatch or a queued dispatch can take the message - so the value does
     * not depend on who consumes it or on the dispatch mode.
     */
    void addMessage(
        ZLinkStreamMessage<ZLinkStreamEncodedPayload> message,
        Supplier<CompletionStage<Void>> dispatch,
        BooleanSupplier dispatchable,
        boolean runImmediately) {
        if (message.packetName() != null) {
            synchronized (queue) {
                receivedCounts.merge(message.packetName(), 1, Integer::sum);
            }
        }
        while (true) {
            long observedVersion;
            List<Waiter> candidates;
            synchronized (queue) {
                observedVersion = version;
                candidates = waiters.stream()
                    .filter(waiter -> waiter.name().equals(message.packetName()))
                    .toList();
            }
            Waiter matched = null;
            Throwable predicateFailure = null;
            for (Waiter waiter : candidates) {
                try {
                    if (!waiter.predicate().test(message)) {
                        continue;
                    }
                } catch (Throwable error) {
                    predicateFailure = error;
                }
                if (claimWaiter(waiter)) {
                    matched = waiter;
                    break;
                }
                predicateFailure = null;
            }
            if (matched != null) {
                if (predicateFailure != null) {
                    closeMessage(message);
                    matched.result().completeExceptionally(predicateFailure);
                } else if (!matched.result().complete(message)) {
                    closeMessage(message);
                }
                return;
            }

            boolean immediate = false;
            if (runImmediately) {
                try {
                    immediate = dispatchable.getAsBoolean();
                } catch (Throwable error) {
                    closeMessage(message);
                    publishError.accept(new ZLinkStreamError(
                        ZLinkStreamErrorCode.USER_CALLBACK_FAILED,
                        "Stream dispatchability check failed.", error));
                    return;
                }
            }
            synchronized (queue) {
                if (version != observedVersion) {
                    continue;
                }
                if (!immediate) {
                    queue.add(new QueuedDispatch(
                        message.packetName(), message, dispatch, dispatchable));
                    version++;
                }
            }
            if (immediate) {
                try {
                    dispatch.get();
                } catch (Throwable error) {
                    // The dispatch supplier did not return its completion
                    // stage, so its normal terminal close callback was never
                    // installed. Release the outer message here.
                    closeMessage(message);
                    publishError.accept(new ZLinkStreamError(
                        ZLinkStreamErrorCode.USER_CALLBACK_FAILED,
                        "Stream message handler failed.", error));
                }
            }
            return;
        }
    }

    void awaitMessage(
        String name,
        Predicate<ZLinkStreamMessage<ZLinkStreamEncodedPayload>> predicate,
        CompletableFuture<ZLinkStreamMessage<ZLinkStreamEncodedPayload>> result) {
        while (true) {
            long observedVersion;
            List<QueuedDispatch> candidates;
            synchronized (queue) {
                observedVersion = version;
                candidates = queue.stream()
                    .filter(item -> item.message() != null
                        && name.equals(item.packetName()))
                    .toList();
            }
            QueuedDispatch matched = null;
            Throwable predicateFailure = null;
            for (QueuedDispatch item : candidates) {
                try {
                    if (!predicate.test(item.message())) {
                        continue;
                    }
                } catch (Throwable error) {
                    predicateFailure = error;
                }
                if (claimQueued(item)) {
                    matched = item;
                    break;
                }
                predicateFailure = null;
            }
            if (matched != null) {
                ZLinkStreamMessage<ZLinkStreamEncodedPayload> found =
                    matched.message();
                if (predicateFailure != null) {
                    closeMessage(found);
                    result.completeExceptionally(predicateFailure);
                } else if (!result.complete(found)) {
                    closeMessage(found);
                }
                return;
            }
            synchronized (queue) {
                if (version != observedVersion) {
                    continue;
                }
                Waiter waiter = new Waiter(name, predicate, result);
                waiters.add(waiter);
                version++;
                result.whenComplete((ignored, error) -> removeWaiter(waiter));
                return;
            }
        }
    }

    void clear() {
        List<QueuedDispatch> queued;
        List<Waiter> pending;
        synchronized (queue) {
            queued = List.copyOf(queue);
            pending = List.copyOf(waiters);
            queue.clear();
            waiters.clear();
            version++;
        }
        queued.stream()
            .map(QueuedDispatch::message)
            .filter(Objects::nonNull)
            .forEach(ZLinkStreamDispatchQueue::closeMessage);
        pending.forEach(waiter -> waiter.result().completeExceptionally(
            ZLinkStreamException.disconnected("stream dispatch queue was closed")));
    }

    /**
     * Restarts the received counters and drops what the previous connection
     * left unconsumed.
     *
     * <p>Spec 32 10 puts the reference point at the moment a connection is
     * established, so a reconnect counts from 0 again, and it clears the
     * messages of the previous connection with the count. Keeping them would
     * leave the two describing different connections and let {@code waitFor}
     * hand back a packet from before the drop.
     *
     * <p>Only received messages are dropped. Queued callbacks - a state
     * change, an error, a disconnect - belong to the connector surface, not
     * to the receive message queue, and are still owed to the application.
     *
     * <p>{@code replacesAnEarlierConnection} says whether a connection ended
     * before this one. A wait that cannot continue because its connection
     * ended is {@code Disconnected} (spec 32 10.1); a wait registered before
     * the first connection has no ended connection behind it.
     */
    void resetForNewConnection(boolean replacesAnEarlierConnection) {
        List<QueuedDispatch> abandoned;
        List<Waiter> pending = List.of();
        synchronized (queue) {
            receivedCounts.clear();
            abandoned = queue.stream()
                .filter(item -> item.message() != null)
                .toList();
            if (!abandoned.isEmpty()) {
                queue.removeIf(item -> item.message() != null);
                version++;
            }
            if (replacesAnEarlierConnection && !waiters.isEmpty()) {
                pending = List.copyOf(waiters);
                waiters.clear();
                version++;
            }
        }
        abandoned.stream()
            .map(QueuedDispatch::message)
            .forEach(ZLinkStreamDispatchQueue::closeMessage);
        pending.forEach(waiter -> waiter.result().completeExceptionally(
            ZLinkStreamException.disconnected(
                "the connection that the wait observed has ended")));
    }

    int receivedCount(String packetName) {
        synchronized (queue) {
            return receivedCounts.getOrDefault(packetName, 0);
        }
    }

    CompletionStage<Void> drainAsync() {
        CompletableFuture<Void> drained = new CompletableFuture<>();
        drainInto(drained);
        return drained;
    }

    /**
     * Runs the dispatchable items one after another until the queue has none
     * left, one fails, or one hands back a stage that has not finished.
     *
     * <p>Spec 32 7 makes {@code Manual} the default so the application pumps
     * on the thread that may touch its objects, and it runs the registered
     * handlers in the order they were queued. Both are properties of this
     * loop: an item runs on the thread that reached it, and the next one is
     * only taken once the previous one has finished.
     *
     * <p>An item that finished on the spot - the usual case, a handler that
     * returns without waiting for anything - is followed by the next turn of
     * this loop. Chaining it through a continuation instead left the
     * continuation running on the same stack, so the depth grew with the
     * queue and a {@code Manual} queue, which grows until the pump, ran the
     * stack out. Only an item that has not finished hands the rest of the
     * queue to its own completion, and that resumes on the thread that
     * completed it - the same thread the recursive chain used.
     */
    private void drainInto(CompletableFuture<Void> drained) {
        while (true) {
            QueuedDispatch next;
            try {
                next = takeDispatchable();
            } catch (Throwable error) {
                drained.completeExceptionally(error);
                return;
            }
            if (next == null) {
                drained.complete(null);
                return;
            }
            CompletableFuture<Void> item;
            try {
                item = next.action().get().toCompletableFuture();
            } catch (Throwable error) {
                closeMessage(next.message());
                drained.completeExceptionally(error);
                return;
            }
            if (item.isDone() && !item.isCompletedExceptionally()) {
                continue;
            }
            item.whenComplete((ignored, error) -> {
                if (error != null) {
                    drained.completeExceptionally(error);
                } else {
                    drainInto(drained);
                }
            });
            return;
        }
    }

    /**
     * Takes the first queued item that is dispatchable right now, or
     * {@code null} when the queue holds none. A queue that changed while it
     * was being examined is examined again, so an item added or claimed in
     * between is neither missed nor dispatched twice.
     */
    private QueuedDispatch takeDispatchable() {
        while (true) {
            long observedVersion;
            List<QueuedDispatch> candidates;
            synchronized (queue) {
                observedVersion = version;
                candidates = List.copyOf(queue);
            }
            QueuedDispatch next = null;
            for (QueuedDispatch candidate : candidates) {
                if (!candidate.dispatchable().getAsBoolean()) {
                    continue;
                }
                boolean versionChanged;
                synchronized (queue) {
                    versionChanged = version != observedVersion;
                    if (!versionChanged && removeQueuedLocked(candidate)) {
                        next = candidate;
                    }
                }
                if (versionChanged || next != null) {
                    break;
                }
            }
            if (next != null) {
                return next;
            }
            synchronized (queue) {
                if (version == observedVersion) {
                    return null;
                }
            }
        }
    }

    private void removeWaiter(Waiter waiter) {
        synchronized (queue) {
            if (waiters.remove(waiter)) {
                version++;
            }
        }
    }

    private boolean claimWaiter(Waiter waiter) {
        synchronized (queue) {
            for (Iterator<Waiter> iterator = waiters.iterator(); iterator.hasNext();) {
                if (iterator.next() == waiter) {
                    iterator.remove();
                    version++;
                    return true;
                }
            }
            return false;
        }
    }

    private boolean claimQueued(QueuedDispatch candidate) {
        synchronized (queue) {
            return removeQueuedLocked(candidate);
        }
    }

    private boolean removeQueuedLocked(QueuedDispatch candidate) {
        for (Iterator<QueuedDispatch> iterator = queue.iterator(); iterator.hasNext();) {
            if (iterator.next() == candidate) {
                iterator.remove();
                //  Spec 32 10: consuming does not lower the count. A
                //  scenario that registers an `on` handler and dispatches
                //  must still be able to assert how many arrived.
                version++;
                return true;
            }
        }
        return false;
    }

    private static void closeMessage(
        ZLinkStreamMessage<ZLinkStreamEncodedPayload> message) {
        try {
            message.payload().payload().close();
        } catch (RuntimeException ignored) {
            // The receive path has no caller that can recover a dropped payload.
        }
    }

    private record QueuedDispatch(
        String packetName,
        ZLinkStreamMessage<ZLinkStreamEncodedPayload> message,
        Supplier<CompletionStage<Void>> action,
        BooleanSupplier dispatchable) {
    }

    private record Waiter(
        String name,
        Predicate<ZLinkStreamMessage<ZLinkStreamEncodedPayload>> predicate,
        CompletableFuture<ZLinkStreamMessage<ZLinkStreamEncodedPayload>> result) {
    }
}
