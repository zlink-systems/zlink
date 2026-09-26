package systems.zlink.stream.connector;

import java.util.ArrayDeque;
import java.util.ArrayList;
import java.util.List;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.function.Consumer;
import java.util.function.Supplier;

/** Owns the ordered frame writes of the admitted connection. */
final class ZLinkStreamSendChain {
    private final Object lock = new Object();
    //  The write in progress is not in the queue.
    private final ArrayDeque<Write> queue = new ArrayDeque<>();
    private Write active;
    private boolean pumping;

    CompletableFuture<Void> enqueue(Supplier<CompletionStage<Void>> write) {
        CompletableFuture<Void> publication = enqueueDeferred(write, ignored -> {});
        pump();
        return publication;
    }

    CompletableFuture<Void> enqueueDeferred(
            Supplier<CompletionStage<Void>> write, Consumer<Throwable> writeFailure) {
        Write operation = new Write(write, writeFailure);
        synchronized (lock) {
            queue.addLast(operation);
        }
        return operation.publication;
    }

    /**
     * Fails the writes of the connection that ended - the ones not yet written and the one being
     * written - with {@code Disconnected}, before another connection can use the queue. The
     * connection ending closes the transport before this runs, so no frame reaches the peer after
     * its operation failed here.
     */
    void reset() {
        List<Write> abandoned;
        synchronized (lock) {
            abandoned = new ArrayList<>(queue);
            if (active != null) {
                abandoned.add(active);
            }
            queue.clear();
            active = null;
        }
        ZLinkStreamException disconnected =
                ZLinkStreamException.disconnected("connection ended before frame write completed");
        abandoned.forEach(write -> write.publication.completeExceptionally(disconnected));
    }

    void pump() {
        synchronized (lock) {
            if (pumping) {
                return;
            }
            pumping = true;
        }
        while (true) {
            Write next;
            synchronized (lock) {
                if (active != null) {
                    pumping = false;
                    return;
                }
                next = queue.pollFirst();
                if (next == null) {
                    pumping = false;
                    return;
                }
                active = next;
            }
            try {
                CompletionStage<Void> publication = next.write.get();
                publication.whenComplete((ignored, failure) -> finish(next, failure));
            } catch (Throwable failure) {
                finish(next, failure);
            }
            synchronized (lock) {
                if (active == next) {
                    pumping = false;
                    return;
                }
            }
        }
    }

    private void finish(Write operation, Throwable failure) {
        boolean startNext;
        synchronized (lock) {
            if (active != operation) {
                return;
            }
            active = null;
            startNext = !pumping;
        }
        if (failure == null) {
            operation.publication.complete(null);
        } else {
            operation.publication.completeExceptionally(failure);
            operation.writeFailure.accept(failure);
        }
        if (startNext) {
            pump();
        }
    }

    private static final class Write {
        private final Supplier<CompletionStage<Void>> write;
        private final CompletableFuture<Void> publication = new CompletableFuture<>();
        private final Consumer<Throwable> writeFailure;

        private Write(Supplier<CompletionStage<Void>> write, Consumer<Throwable> writeFailure) {
            this.write = write;
            this.writeFailure = writeFailure;
        }
    }
}
