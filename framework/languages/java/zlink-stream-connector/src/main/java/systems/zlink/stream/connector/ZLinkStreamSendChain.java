package systems.zlink.stream.connector;

import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.atomic.AtomicReference;
import java.util.function.Supplier;

/**
 * Serialises outbound writes so frames reach the transport in submission order.
 *
 * <p>The tail is an {@link AtomicReference} rather than a field guarded by the connector monitor: a
 * single field needs no object-wide monitor, and the connector instance is an object the
 * application also holds.
 *
 * <p>A failed write must not end the chain. {@code thenCompose} on a failed predecessor never calls
 * its function and propagates the same failure, so one failed send would leave every later send
 * failing without ever reaching the transport. The predecessor's outcome is therefore absorbed
 * before the next write is composed, and each write reports only its own outcome.
 */
final class ZLinkStreamSendChain {
    private final AtomicReference<CompletableFuture<Void>> tail =
            new AtomicReference<>(CompletableFuture.completedFuture(null));

    /**
     * Appends {@code write} to the chain and returns the future that carries that write's own
     * outcome.
     */
    CompletableFuture<Void> enqueue(Supplier<CompletionStage<Void>> write) {
        CompletableFuture<Void> publication = new CompletableFuture<>();
        //  Publish the new tail before a completed previous tail starts the
        //  write inline and synchronously submits another frame.
        CompletableFuture<Void> previous = tail.getAndSet(publication);
        previous.handle((ignored, previousFailure) -> (Void) null)
                .thenCompose(ignored -> write.get())
                .whenComplete(
                        (ignored, failure) -> {
                            if (failure == null) {
                                publication.complete(null);
                            } else {
                                publication.completeExceptionally(failure);
                            }
                        });
        return publication;
    }

    /**
     * Returns the chain to a completed tail. A new connection starts with no outstanding write, so
     * the writes of the connection that ended must not hold up the writes of the one that replaced
     * it.
     */
    void reset() {
        tail.set(CompletableFuture.completedFuture(null));
    }
}
