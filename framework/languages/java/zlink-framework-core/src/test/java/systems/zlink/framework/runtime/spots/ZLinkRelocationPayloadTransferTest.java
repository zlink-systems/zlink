package systems.zlink.framework.runtime.spots;

import static org.junit.jupiter.api.Assertions.*;

import java.util.List;
import java.util.concurrent.CompletionException;
import org.junit.jupiter.api.Test;

final class ZLinkRelocationPayloadTransferTest {
    @Test
    void manifestAndChunksPreserveTheExactPayloadCrc32c() {
        byte[] payload = new byte[17];
        for (int index = 0; index < payload.length; index++) {
            payload[index] = (byte) index;
        }

        var manifest = ZLinkRelocationPayloadTransfer.manifest(payload, 5);
        assertEquals(payload.length, manifest.totalLength());
        assertEquals(4, manifest.chunkCount());
        assertEquals(
            ZLinkRelocationPayloadTransfer.crc32c(payload),
            manifest.checksumCrc32c());

        var chunks = ZLinkRelocationPayloadTransfer.chunks(payload, 5);
        assertEquals(List.of(5, 5, 5, 2),
            chunks.stream().map(byte[]::length).toList());
        var assembly = new ZLinkRelocationPayloadTransfer.Assembler(manifest);
        for (int index = 0; index < chunks.size(); index++) {
            assembly.accept(index, chunks.get(index));
        }
        assertArrayEquals(payload, assembly.assembled().toCompletableFuture().join());
    }

    @Test
    void assemblerRejectsManifestChecksumMismatchWithoutReturningPartialPayload() {
        byte[] payload = {1, 2, 3, 4};
        var manifest = new ZLinkCanonicalRelocationProtocol.Manifest(
            payload.length, 1,
            ZLinkRelocationPayloadTransfer.crc32c(new byte[] {9}));
        var assembly = new ZLinkRelocationPayloadTransfer.Assembler(manifest);

        assembly.accept(0, payload);

        var failure = assertThrows(CompletionException.class,
            () -> assembly.assembled().toCompletableFuture().join());
        assertInstanceOf(IllegalStateException.class, failure.getCause());
    }

    @Test
    void assemblerRejectsAConflictingDuplicateButAcceptsAnIdenticalRetry() {
        byte[] payload = {1, 2, 3, 4};
        var manifest = ZLinkRelocationPayloadTransfer.manifest(payload, 2);
        var assembly = new ZLinkRelocationPayloadTransfer.Assembler(manifest);

        assembly.accept(0, new byte[] {1, 2});
        assembly.accept(0, new byte[] {1, 2});
        assembly.accept(0, new byte[] {9, 9});

        assertThrows(CompletionException.class,
            () -> assembly.assembled().toCompletableFuture().join());
    }

    @Test
    void retransmissionReplacesThePartialAssemblyInOrdinalOrder() {
        byte[] payload = {1, 2, 3, 4};
        var manifest = ZLinkRelocationPayloadTransfer.manifest(payload, 2);
        var assembly = new ZLinkRelocationPayloadTransfer.Assembler(manifest);

        assembly.accept(0, new byte[] {9, 9});
        assembly.discardPartial();
        assembly.accept(0, new byte[] {1, 2});
        assembly.accept(1, new byte[] {3, 4});

        assertArrayEquals(payload, assembly.assembled().toCompletableFuture().join());
    }

    @Test
    void budgetAdmitsOneOversizedChunkWhenIdleAndReleasesTheWaiterOnTerminal() {
        var budget = new ZLinkRelocationPayloadTransfer.Budget(
            1024, 4, 4);
        var peer = systems.zlink.contracts.core.RoutingId.from(new byte[] {1});

        budget.acquire(peer, 8).toCompletableFuture().join();
        var waiting = budget.acquire(peer, 1).toCompletableFuture();
        assertFalse(waiting.isDone());
        budget.release(peer, 8);
        waiting.join();
    }
}
