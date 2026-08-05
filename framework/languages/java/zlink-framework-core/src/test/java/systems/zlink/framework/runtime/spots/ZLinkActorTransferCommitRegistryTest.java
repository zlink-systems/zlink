package systems.zlink.framework.runtime.spots;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertThrows;

import java.util.List;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionException;
import java.util.concurrent.atomic.AtomicInteger;
import org.junit.jupiter.api.Test;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.framework.runtime.actors.ZLinkActorSpotRoutePackets;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendActorRef;
import systems.zlink.framework.spots.ZLinkSpotActorJoinResult;

final class ZLinkActorTransferCommitRegistryTest {
    @Test
    void duplicateCommitSharesOneTargetTerminal() {
        ZLinkActorTransferCommitRegistry registry =
            new ZLinkActorTransferCommitRegistry();
        ZLinkActorSpotRoutePackets.TransferRequest request = request("transfer-1", "actor");
        CompletableFuture<ZLinkActorSpotAdmission.RoutedJoin> operation =
            new CompletableFuture<>();
        AtomicInteger starts = new AtomicInteger();

        var first = registry.execute(
            request,
            () -> {
                starts.incrementAndGet();
                return operation;
            });
        var second = registry.execute(
            request,
            () -> {
                starts.incrementAndGet();
                return CompletableFuture.failedFuture(
                    new AssertionError("duplicate commit was executed"));
            });

        operation.complete(new ZLinkActorSpotAdmission.RoutedJoin(
            new ZLinkBackendActorRef(RoutingId.from("target"), "actor", 7),
            ZLinkSpotActorJoinResult.accept(),
            List.of(Message.from("reply"))));

        assertEquals(1, starts.get());
        try (var firstJoin = new ReplyOwner(first.toCompletableFuture().join());
             var secondJoin = new ReplyOwner(second.toCompletableFuture().join())) {
            assertEquals(firstJoin.value.actorRef(), secondJoin.value.actorRef());
            assertEquals(1, firstJoin.value.handoffReplies().size());
            assertEquals(1, secondJoin.value.handoffReplies().size());
        }
    }

    @Test
    void duplicateTransferIdWithDifferentFingerprintIsRejected() {
        ZLinkActorTransferCommitRegistry registry =
            new ZLinkActorTransferCommitRegistry();
        ZLinkActorSpotRoutePackets.TransferRequest first = request("transfer-2", "actor");
        ZLinkActorSpotRoutePackets.TransferRequest different =
            request("transfer-2", "other-actor");
        registry.execute(
            first,
            () -> CompletableFuture.completedFuture(
                new ZLinkActorSpotAdmission.RoutedJoin(
                    new ZLinkBackendActorRef(RoutingId.from("target"), "actor", 7),
                    ZLinkSpotActorJoinResult.accept(),
                    List.of())))
            .toCompletableFuture().join();

        assertThrows(
            CompletionException.class,
            () -> registry.execute(
                    different,
                    () -> CompletableFuture.failedFuture(
                        new AssertionError("mismatched commit executed")))
                .toCompletableFuture().join());
    }

    private static ZLinkActorSpotRoutePackets.TransferRequest request(
        String transferId,
        String actorId) {
        return new ZLinkActorSpotRoutePackets.TransferRequest(
            ZLinkActorSpotRoutePackets.COMMIT_PHASE,
            transferId,
            1_000,
            actorId,
            "Player",
            RoutingId.from("source"),
            7,
            RoutingId.from("entry"),
            "entry",
            "router",
            null,
            null,
            null,
            0,
            false,
            0,
            0,
            0,
            0,
            0,
            0,
            null,
            new byte[0]);
    }

    private static final class ReplyOwner implements AutoCloseable {
        private final ZLinkActorSpotAdmission.RoutedJoin value;

        private ReplyOwner(ZLinkActorSpotAdmission.RoutedJoin value) {
            this.value = value;
        }

        @Override
        public void close() {
            value.handoffReplies().forEach(Message::close);
        }
    }
}
