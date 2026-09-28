package systems.zlink.stream.connector;

import java.util.ArrayDeque;
import java.util.ArrayList;
import java.util.Collections;
import java.util.HashMap;
import java.util.IdentityHashMap;
import java.util.Iterator;
import java.util.List;
import java.util.Map;
import java.util.Queue;
import java.util.Set;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.function.Consumer;
import java.util.function.IntSupplier;
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
        this(ignored -> {});
    }

    ZLinkStreamDispatchQueue(Consumer<ZLinkStreamError> publishError) {
        this.publishError = publishError;
    }

    int pendingCallbacks() {
        List<QueuedDispatch> queued;
        synchronized (queue) {
            queued = List.copyOf(queue);
        }
        int count = 0;
        for (QueuedDispatch item : queued) {
            if (item.pendingCallbacks().getAsInt() > 0) {
                count++;
            }
        }
        return count;
    }

    void add(Runnable item) {
        add(null, item);
    }

    void add(String packetName, Runnable item) {
        addAsync(
                packetName,
                () -> {
                    item.run();
                    return CompletableFuture.completedFuture(null);
                });
    }

    void addAsync(Supplier<CompletionStage<Void>> item) {
        addAsync(null, item);
    }

    void addAsync(String packetName, Supplier<CompletionStage<Void>> item) {
        synchronized (queue) {
            queue.add(new QueuedDispatch(packetName, null, () -> item, () -> 1));
            version++;
        }
    }

    /**
     * Records one received packet and routes it.
     *
     * <p>Spec 32 10: {@code receivedCount(name)} counts what arrived. It is counted once, in the
     * same step under the queue lock that hands the message to its consumer - a waiter, an
     * immediate dispatch or the queue - so the value does not depend on who consumes it or on the
     * dispatch mode, and a counted message is already observable to {@code dispatch} and {@code
     * waitFor}.
     */
    void addMessage(
            ZLinkStreamMessage<ZLinkStreamEncodedPayload> message,
            Supplier<Supplier<CompletionStage<Void>>> selectDispatch,
            IntSupplier pendingCallbacks,
            boolean runImmediately) {
        while (true) {
            long observedVersion;
            List<Waiter> candidates;
            synchronized (queue) {
                observedVersion = version;
                candidates =
                        waiters.stream()
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
                if (claimWaiter(waiter, message)) {
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

            Supplier<CompletionStage<Void>> immediate = null;
            if (runImmediately) {
                try {
                    immediate = selectDispatch.get();
                } catch (Throwable error) {
                    synchronized (queue) {
                        countArrivalLocked(message);
                    }
                    closeMessage(message);
                    publishError.accept(
                            new ZLinkStreamError(
                                    ZLinkStreamErrorCode.USER_CALLBACK_FAILED,
                                    "Stream dispatchability check failed.",
                                    error));
                    return;
                }
            }
            synchronized (queue) {
                if (version != observedVersion) {
                    continue;
                }
                countArrivalLocked(message);
                if (immediate == null) {
                    queue.add(
                            new QueuedDispatch(
                                    message.packetName(),
                                    message,
                                    selectDispatch,
                                    pendingCallbacks));
                    version++;
                }
            }
            if (immediate != null) {
                try {
                    immediate.get();
                } catch (Throwable error) {
                    // The dispatch supplier did not return its completion
                    // stage, so its normal terminal close callback was never
                    // installed. Release the outer message here.
                    closeMessage(message);
                    publishError.accept(
                            new ZLinkStreamError(
                                    ZLinkStreamErrorCode.USER_CALLBACK_FAILED,
                                    "Stream message handler failed.",
                                    error));
                }
            }
            return;
        }
    }

    void awaitMessage(
            String name,
            Predicate<ZLinkStreamMessage<ZLinkStreamEncodedPayload>> predicate,
            CompletableFuture<ZLinkStreamMessage<ZLinkStreamEncodedPayload>> result) {
        // A version change may add packets while the predicate runs. Keep the
        // scan cursor across that change so an older rejected packet is not
        // tested again on every arrival.
        Set<QueuedDispatch> examined = Collections.newSetFromMap(new IdentityHashMap<>());
        while (true) {
            long observedVersion;
            List<QueuedDispatch> candidates;
            synchronized (queue) {
                observedVersion = version;
                candidates =
                        queue.stream()
                                .filter(
                                        item ->
                                                item.message() != null
                                                        && name.equals(item.packetName())
                                                        && !examined.contains(item))
                                .toList();
            }
            QueuedDispatch matched = null;
            Throwable predicateFailure = null;
            for (QueuedDispatch item : candidates) {
                examined.add(item);
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
                ZLinkStreamMessage<ZLinkStreamEncodedPayload> found = matched.message();
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

    /**
     * Restarts the received counters and drops what the previous connection left unconsumed.
     *
     * <p>Spec 32 10 puts the reference point at the moment a connection is established, so a
     * reconnect counts from 0 again, and it clears the messages of the previous connection with the
     * count. Keeping them would leave the two describing different connections and let {@code
     * waitFor} hand back a packet from before the drop.
     *
     * <p>Only received messages are dropped. Queued callbacks - a state change, an error, a
     * disconnect - belong to the connector surface, not to the receive message queue, and are still
     * owed to the application. Waiters are not touched either: the ones of the previous connection
     * ended with it in {@link #connectionEnded()}, and one registered since then is waiting for
     * this connection.
     */
    void resetForNewConnection() {
        List<QueuedDispatch> abandoned;
        synchronized (queue) {
            receivedCounts.clear();
            abandoned = queue.stream().filter(item -> item.message() != null).toList();
            if (!abandoned.isEmpty()) {
                queue.removeIf(item -> item.message() != null);
                version++;
            }
        }
        abandoned.stream()
                .map(QueuedDispatch::message)
                .forEach(ZLinkStreamDispatchQueue::closeMessage);
    }

    /**
     * Fails every registered waiter as {@code Disconnected} because the connection it was observing
     * has ended - a transport loss, a server close, or {@code close()}.
     *
     * <p>Spec 32 10.1.1: the release belongs to the ending of the connection, not to the
     * establishment of the next one. Released here, a wait does not hang until its own timeout when
     * no next connection comes (reconnect off, attempts spent) and does not rebind to the next one
     * when it does. The queue and the counts stay: they are rebaselined by the next {@link
     * #resetForNewConnection()}, not by the ending (spec 32 10).
     */
    void connectionEnded() {
        List<Waiter> pending;
        synchronized (queue) {
            if (waiters.isEmpty()) {
                return;
            }
            pending = List.copyOf(waiters);
            waiters.clear();
            version++;
        }
        pending.forEach(
                waiter ->
                        waiter.result()
                                .completeExceptionally(
                                        ZLinkStreamException.disconnected(
                                                "the connection that the wait observed has"
                                                        + " ended")));
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

    /** Starts queued callbacks in order on the pump thread, without awaiting their stages. */
    private void drainInto(CompletableFuture<Void> drained) {
        while (true) {
            SelectedDispatch next;
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
            try {
                next.action()
                        .get()
                        .whenComplete(
                                (ignored, error) -> {
                                    if (error != null) {
                                        publishError.accept(
                                                DefaultZLinkStreamConnector.userCallbackFailed(
                                                        error));
                                    }
                                });
            } catch (Throwable error) {
                closeMessage(next.message());
                publishError.accept(DefaultZLinkStreamConnector.userCallbackFailed(error));
            }
        }
    }

    /**
     * Takes the first queued item that is dispatchable right now, or {@code null} when the queue
     * holds none. A queue that changed while it was being examined is examined again, so an item
     * added or claimed in between is neither missed nor dispatched twice.
     */
    private SelectedDispatch takeDispatchable() {
        while (true) {
            long observedVersion;
            List<QueuedDispatch> candidates;
            synchronized (queue) {
                observedVersion = version;
                candidates = List.copyOf(queue);
            }
            SelectedDispatch next = null;
            for (QueuedDispatch candidate : candidates) {
                //  Spec 32 7, 10: the handlers are decided here, when the item is
                //  dispatched. A packet no handler takes now stays queued for a
                //  handler registered later or a wait.
                Supplier<CompletionStage<Void>> action = candidate.selectDispatch().get();
                if (action == null) {
                    continue;
                }
                boolean versionChanged;
                synchronized (queue) {
                    versionChanged = version != observedVersion;
                    if (!versionChanged && removeQueuedLocked(candidate)) {
                        next = new SelectedDispatch(candidate.message(), action);
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

    private boolean claimWaiter(
            Waiter waiter, ZLinkStreamMessage<ZLinkStreamEncodedPayload> message) {
        synchronized (queue) {
            for (Iterator<Waiter> iterator = waiters.iterator(); iterator.hasNext(); ) {
                if (iterator.next() == waiter) {
                    iterator.remove();
                    countArrivalLocked(message);
                    version++;
                    return true;
                }
            }
            return false;
        }
    }

    /** Guarded by queue; called only where the arrival is handed to its consumer. */
    private void countArrivalLocked(ZLinkStreamMessage<ZLinkStreamEncodedPayload> message) {
        if (message.packetName() != null) {
            receivedCounts.merge(message.packetName(), 1, Integer::sum);
        }
    }

    private boolean claimQueued(QueuedDispatch candidate) {
        synchronized (queue) {
            return removeQueuedLocked(candidate);
        }
    }

    private boolean removeQueuedLocked(QueuedDispatch candidate) {
        for (Iterator<QueuedDispatch> iterator = queue.iterator(); iterator.hasNext(); ) {
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

    private static void closeMessage(ZLinkStreamMessage<ZLinkStreamEncodedPayload> message) {
        try {
            message.payload().payload().close();
        } catch (RuntimeException ignored) {
            // The receive path has no caller that can recover a dropped payload.
        }
    }

    private record QueuedDispatch(
            String packetName,
            ZLinkStreamMessage<ZLinkStreamEncodedPayload> message,
            Supplier<Supplier<CompletionStage<Void>>> selectDispatch,
            IntSupplier pendingCallbacks) {}

    private record SelectedDispatch(
            ZLinkStreamMessage<ZLinkStreamEncodedPayload> message,
            Supplier<CompletionStage<Void>> action) {}

    private record Waiter(
            String name,
            Predicate<ZLinkStreamMessage<ZLinkStreamEncodedPayload>> predicate,
            CompletableFuture<ZLinkStreamMessage<ZLinkStreamEncodedPayload>> result) {}
}
