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
        }
    }

    void addMessage(
        ZLinkStreamMessage<ZLinkStreamEncodedPayload> message,
        Supplier<CompletionStage<Void>> dispatch,
        BooleanSupplier dispatchable,
        boolean runImmediately) {
        Waiter matched = null;
        Throwable predicateFailure = null;
        boolean dropped = false;
        synchronized (queue) {
            for (Iterator<Waiter> iterator = waiters.iterator(); iterator.hasNext();) {
                Waiter waiter = iterator.next();
                if (!waiter.name().equals(message.packetName())) {
                    continue;
                }
                try {
                    if (!waiter.predicate().test(message)) {
                        continue;
                    }
                } catch (Throwable error) {
                    predicateFailure = error;
                }
                iterator.remove();
                matched = waiter;
                break;
            }
            if (matched == null && !(runImmediately && dispatchable.getAsBoolean())) {
                if (receivedMessageCount() >= maxReceivedMessages) {
                    dropped = true;
                } else {
                    queue.add(new QueuedDispatch(
                        message.packetName(), message, dispatch, dispatchable));
                    receivedCounts.merge(message.packetName(), 1, Integer::sum);
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
        if (matched != null) {
            if (predicateFailure != null) {
                closeMessage(message);
                matched.result().completeExceptionally(predicateFailure);
            } else {
                if (!matched.result().complete(message)) {
                    closeMessage(message);
                }
            }
            return;
        }
        if (runImmediately && dispatchable.getAsBoolean()) {
            try {
                dispatch.get();
            } catch (Throwable error) {
                publishError.accept(new ZLinkStreamError(
                    ZLinkStreamErrorCode.USER_CALLBACK_FAILED,
                    "Stream message handler failed.", error));
            }
        }
    }

    void awaitMessage(
        String name,
        Predicate<ZLinkStreamMessage<ZLinkStreamEncodedPayload>> predicate,
        CompletableFuture<ZLinkStreamMessage<ZLinkStreamEncodedPayload>> result) {
        ZLinkStreamMessage<ZLinkStreamEncodedPayload> found = null;
        Throwable predicateFailure = null;
        synchronized (queue) {
            for (Iterator<QueuedDispatch> iterator = queue.iterator(); iterator.hasNext();) {
                QueuedDispatch item = iterator.next();
                if (item.message() == null || !name.equals(item.packetName())) {
                    continue;
                }
                try {
                    if (!predicate.test(item.message())) {
                        continue;
                    }
                } catch (Throwable error) {
                    predicateFailure = error;
                    iterator.remove();
                    decrementReceivedCount(item.packetName());
                    found = item.message();
                    break;
                }
                iterator.remove();
                decrementReceivedCount(item.packetName());
                found = item.message();
                break;
            }
            if (found == null && predicateFailure == null) {
                Waiter waiter = new Waiter(name, predicate, result);
                waiters.add(waiter);
                result.whenComplete((ignored, error) -> removeWaiter(waiter));
            }
        }
        if (found != null) {
            if (predicateFailure != null) {
                closeMessage(found);
                result.completeExceptionally(predicateFailure);
            } else {
                if (!result.complete(found)) {
                    closeMessage(found);
                }
            }
        }
    }

    void clear() {
        synchronized (queue) {
            List<QueuedDispatch> queued = List.copyOf(queue);
            List<Waiter> pending = List.copyOf(waiters);
            queue.clear();
            receivedCounts.clear();
            waiters.clear();
            queued.stream()
                .map(QueuedDispatch::message)
                .filter(java.util.Objects::nonNull)
                .forEach(ZLinkStreamDispatchQueue::closeMessage);
            pending.forEach(waiter -> waiter.result().completeExceptionally(
                new IllegalStateException("stream dispatch queue was closed")));
        }
    }

    int receivedCount(String packetName) {
        synchronized (queue) {
            return receivedCounts.getOrDefault(packetName, 0);
        }
    }

    CompletionStage<Void> drainAsync() {
        QueuedDispatch next;
        synchronized (queue) {
            next = null;
            for (Iterator<QueuedDispatch> iterator = queue.iterator(); iterator.hasNext();) {
                QueuedDispatch candidate = iterator.next();
                if (!candidate.dispatchable().getAsBoolean()) {
                    continue;
                }
                iterator.remove();
                next = candidate;
                decrementReceivedCount(next.packetName());
                break;
            }
        }
        if (next == null) {
            return CompletableFuture.completedFuture(null);
        }
        try {
            return next.action().get().thenCompose(ignored -> drainAsync());
        } catch (Throwable error) {
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
            waiters.remove(waiter);
        }
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
