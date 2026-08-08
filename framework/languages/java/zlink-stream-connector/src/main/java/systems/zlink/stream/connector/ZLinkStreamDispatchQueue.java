package systems.zlink.stream.connector;

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
    private final int maxReceivedMessages;
    private final Consumer<ZLinkStreamError> publishError;
    private long version;

    ZLinkStreamDispatchQueue(int maxReceivedMessages) {
        this(maxReceivedMessages, ignored -> { });
    }

    ZLinkStreamDispatchQueue(
        int maxReceivedMessages,
        Consumer<ZLinkStreamError> publishError) {
        this.maxReceivedMessages = maxReceivedMessages;
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

    void addMessage(
        ZLinkStreamMessage<ZLinkStreamEncodedPayload> message,
        Supplier<CompletionStage<Void>> dispatch,
        BooleanSupplier dispatchable,
        boolean runImmediately) {
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
            boolean dropped = false;
            synchronized (queue) {
                if (version != observedVersion) {
                    continue;
                }
                if (!immediate) {
                    if (receivedMessageCount() >= maxReceivedMessages) {
                        dropped = true;
                    } else {
                        queue.add(new QueuedDispatch(
                            message.packetName(), message, dispatch, dispatchable));
                        receivedCounts.merge(message.packetName(), 1, Integer::sum);
                        version++;
                    }
                }
            }
            if (dropped) {
                closeMessage(message);
                publishError.accept(new ZLinkStreamError(
                    ZLinkStreamErrorCode.RECEIVED_MESSAGE_DROPPED,
                    "Received message queue is full; dropped packet '"
                        + message.packetName() + "'."));
                return;
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
            receivedCounts.clear();
            waiters.clear();
            version++;
        }
        queued.stream()
            .map(QueuedDispatch::message)
            .filter(java.util.Objects::nonNull)
            .forEach(ZLinkStreamDispatchQueue::closeMessage);
        pending.forEach(waiter -> waiter.result().completeExceptionally(
            new IllegalStateException("stream dispatch queue was closed")));
    }

    int receivedCount(String packetName) {
        synchronized (queue) {
            return receivedCounts.getOrDefault(packetName, 0);
        }
    }

    CompletionStage<Void> drainAsync() {
        QueuedDispatch next = null;
        while (true) {
            long observedVersion;
            List<QueuedDispatch> candidates;
            synchronized (queue) {
                observedVersion = version;
                candidates = List.copyOf(queue);
            }
            for (QueuedDispatch candidate : candidates) {
                boolean dispatchable;
                try {
                    dispatchable = candidate.dispatchable().getAsBoolean();
                } catch (Throwable error) {
                    return CompletableFuture.failedFuture(error);
                }
                if (!dispatchable) {
                    continue;
                }
                synchronized (queue) {
                    if (version != observedVersion) {
                        break;
                    }
                    if (removeQueuedLocked(candidate)) {
                        next = candidate;
                    }
                }
                if (next != null || observedVersion != version) {
                    break;
                }
            }
            if (next != null) {
                break;
            }
            synchronized (queue) {
                if (version == observedVersion) {
                    return CompletableFuture.completedFuture(null);
                }
            }
        }
        if (next == null) {
            return CompletableFuture.completedFuture(null);
        }
        try {
            return next.action().get().thenCompose(ignored -> drainAsync());
        } catch (Throwable error) {
            closeMessage(next.message());
            return CompletableFuture.failedFuture(error);
        }
    }

    private int receivedMessageCount() {
        int total = 0;
        for (int value : receivedCounts.values()) {
            total += value;
        }
        return total;
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
                decrementReceivedCount(candidate.packetName());
                version++;
                return true;
            }
        }
        return false;
    }

    private void decrementReceivedCount(String packetName) {
        if (packetName == null) {
            return;
        }
        int next = receivedCounts.getOrDefault(packetName, 0) - 1;
        if (next <= 0) {
            receivedCounts.remove(packetName);
        } else {
            receivedCounts.put(packetName, next);
        }
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
