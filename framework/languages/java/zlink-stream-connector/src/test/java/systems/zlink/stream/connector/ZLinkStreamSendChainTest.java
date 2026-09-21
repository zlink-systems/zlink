package systems.zlink.stream.connector;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertNull;
import static org.junit.jupiter.api.Assertions.assertTrue;

import org.junit.jupiter.api.Test;

import java.io.IOException;
import java.util.ArrayList;
import java.util.List;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.atomic.AtomicInteger;

/**
 * The outbound chain carries every send of a connector, so a single failed write must not decide
 * the fate of the ones that follow it.
 */
final class ZLinkStreamSendChainTest {
    @Test
    void writesReachTheTransportInSubmissionOrder() {
        ZLinkStreamSendChain chain = new ZLinkStreamSendChain();
        List<Integer> started = new ArrayList<>();
        List<CompletableFuture<Void>> gates = new ArrayList<>();
        for (int index = 0; index < 3; index++) {
            int position = index;
            CompletableFuture<Void> gate = new CompletableFuture<>();
            gates.add(gate);
            chain.enqueue(
                    () -> {
                        started.add(position);
                        return gate;
                    });
        }

        assertEquals(List.of(0), started);
        gates.get(0).complete(null);
        assertEquals(List.of(0, 1), started);
        gates.get(1).complete(null);
        assertEquals(List.of(0, 1, 2), started);
        gates.get(2).complete(null);
    }

    @Test
    void aFailedWriteStillLetsTheNextOneReachTheTransport() {
        ZLinkStreamSendChain chain = new ZLinkStreamSendChain();
        AtomicInteger writes = new AtomicInteger();

        CompletableFuture<Void> first =
                chain.enqueue(
                        () -> {
                            writes.incrementAndGet();
                            return CompletableFuture.failedFuture(new IOException("write failed"));
                        });
        CompletableFuture<Void> second =
                chain.enqueue(
                        () -> {
                            writes.incrementAndGet();
                            return CompletableFuture.completedFuture(null);
                        });

        assertTrue(first.isCompletedExceptionally());
        //  The second write ran, and it reports its own outcome rather than
        //  the first one's failure.
        assertEquals(2, writes.get());
        assertNull(second.join());
    }

    @Test
    void oneFailureDoesNotPoisonTheChainForGood() {
        ZLinkStreamSendChain chain = new ZLinkStreamSendChain();
        AtomicInteger writes = new AtomicInteger();
        chain.enqueue(
                () -> {
                    writes.incrementAndGet();
                    return CompletableFuture.failedFuture(new IOException("write failed"));
                });

        for (int index = 0; index < 5; index++) {
            assertNull(
                    chain.enqueue(
                                    () -> {
                                        writes.incrementAndGet();
                                        return CompletableFuture.completedFuture(null);
                                    })
                            .join());
        }

        assertEquals(6, writes.get());
    }

    @Test
    void aFailureInFlightDoesNotStopTheWriteBehindIt() {
        ZLinkStreamSendChain chain = new ZLinkStreamSendChain();
        CompletableFuture<Void> gate = new CompletableFuture<>();
        AtomicInteger writes = new AtomicInteger();

        CompletableFuture<Void> first =
                chain.enqueue(
                        () -> {
                            writes.incrementAndGet();
                            return gate;
                        });
        CompletableFuture<Void> second =
                chain.enqueue(
                        () -> {
                            writes.incrementAndGet();
                            return CompletableFuture.completedFuture(null);
                        });
        assertEquals(1, writes.get());

        gate.completeExceptionally(new IOException("the connection went away"));

        assertTrue(first.isCompletedExceptionally());
        assertEquals(2, writes.get());
        assertNull(second.join());
    }

    @Test
    void resetReleasesTheChainFromAWriteThatNeverCompleted() {
        ZLinkStreamSendChain chain = new ZLinkStreamSendChain();
        CompletableFuture<Void> abandoned = new CompletableFuture<>();
        CompletableFuture<Void> first = chain.enqueue(() -> abandoned);
        assertFalse(first.isDone());

        //  A new connection starts with no outstanding write.
        chain.reset();

        AtomicInteger writes = new AtomicInteger();
        CompletableFuture<Void> afterReset =
                chain.enqueue(
                        () -> {
                            writes.incrementAndGet();
                            return CompletableFuture.completedFuture(null);
                        });

        assertEquals(1, writes.get());
        assertNull(afterReset.join());
        abandoned.completeExceptionally(new IOException("the connection went away"));
    }
}
