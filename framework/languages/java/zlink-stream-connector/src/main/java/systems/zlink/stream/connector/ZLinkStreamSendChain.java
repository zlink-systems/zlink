package systems.zlink.stream.connector;

import java.util.ArrayDeque;
import java.util.ArrayList;
import java.util.List;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.function.BooleanSupplier;
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
        admit(operation, () -> {});
        return operation.publication;
    }

    CompletableFuture<ZLinkStreamEncodedPayload> enqueueRequestDeferred(
            Supplier<CompletionStage<Void>> write,
            Consumer<Throwable> writeFailure,
            Consumer<CompletableFuture<ZLinkStreamEncodedPayload>> onAccepted) {
        Write operation = new Write(write, writeFailure);
        CompletableFuture<ZLinkStreamEncodedPayload> pending =
                new CompletableFuture<>() {
                    @Override
                    public boolean cancel(boolean mayInterruptIfRunning) {
                        return cancelWrite(
                                operation,
                                () -> {
                                    boolean completed = super.cancel(mayInterruptIfRunning);
                                    operation.publication.cancel(false);
                                    return completed;
                                });
                    }

                    @Override
                    public boolean completeExceptionally(Throwable failure) {
                        return cancelWrite(
                                operation,
                                () -> {
                                    boolean completed = super.completeExceptionally(failure);
                                    operation.publication.cancel(false);
                                    return completed;
                                });
                    }
                };
        admit(operation, () -> onAccepted.accept(pending));
        return pending;
    }

    private void admit(Write operation, Runnable onAccepted) {
        synchronized (lock) {
            queue.addLast(operation);
            onAccepted.run();
        }
    }

    private boolean cancelWrite(Write operation, BooleanSupplier complete) {
        synchronized (lock) {
            if (queue.remove(operation)) {
                operation.writeFailure = null;
            }
            operation.write = null;
        }
        return complete.getAsBoolean();
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
            abandoned.forEach(
                    write -> {
                        write.write = null;
                        write.writeFailure = null;
                    });
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
            Supplier<CompletionStage<Void>> write;
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
                write = next.write;
                next.write = null;
            }
            try {
                CompletionStage<Void> publication = write.get();
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
        Consumer<Throwable> writeFailure;
        synchronized (lock) {
            if (active != operation) {
                return;
            }
            active = null;
            writeFailure = operation.writeFailure;
            operation.writeFailure = null;
            startNext = !pumping;
        }
        if (failure == null) {
            operation.publication.complete(null);
        } else {
            operation.publication.completeExceptionally(failure);
            writeFailure.accept(failure);
        }
        if (startNext) {
            pump();
        }
    }

    private final class Write {
        private Supplier<CompletionStage<Void>> write;
        private final CompletableFuture<Void> publication =
                new CompletableFuture<>() {
                    @Override
                    public boolean cancel(boolean mayInterruptIfRunning) {
                        return cancelWrite(Write.this, () -> super.cancel(mayInterruptIfRunning));
                    }
                };
        private Consumer<Throwable> writeFailure;

        private Write(Supplier<CompletionStage<Void>> write, Consumer<Throwable> writeFailure) {
            this.write = write;
            this.writeFailure = writeFailure;
        }
    }
}
