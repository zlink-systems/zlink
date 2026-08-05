package systems.zlink.framework.runtime.spots;

import static org.junit.jupiter.api.Assertions.assertThrows;

import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import org.junit.jupiter.api.Test;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.errors.ZLinkConfigurationException;
import systems.zlink.framework.runtime.actors.ZLinkActorSpotRoutePackets;

final class ZLinkPendingActorTransfersTest {
    @Test
    void matchingCommitConsumesAdmissionOnce() {
        ZLinkPendingActorTransfers transfers = new ZLinkPendingActorTransfers();
        transfers.add(request("admission", "transfer-1", "actor-1", "player", 1_000), null, null);

        transfers.take(request("commit", "transfer-1", "actor-1", "player", 1_000));

        assertThrows(
            ZLinkConfigurationException.class,
            () -> transfers.take(request("commit", "transfer-1", "actor-1", "player", 1_000)));
    }

    @Test
    void duplicateAdmissionIsRejectedWithoutReplacingOriginal() {
        ZLinkPendingActorTransfers transfers = new ZLinkPendingActorTransfers();
        transfers.add(request("admission", "transfer-1", "actor-1", "player", 1_000), null, null);

        assertThrows(
            ZLinkConfigurationException.class,
            () -> transfers.add(
                request("admission", "transfer-1", "actor-2", "player", 1_000),
                null,
                null));
        transfers.take(request("commit", "transfer-1", "actor-1", "player", 1_000));
    }

    @Test
    void mismatchedCommitConsumesInvalidAdmission() {
        ZLinkPendingActorTransfers transfers = new ZLinkPendingActorTransfers();
        transfers.add(request("admission", "transfer-1", "actor-1", "player", 1_000), null, null);

        assertThrows(
            ZLinkConfigurationException.class,
            () -> transfers.take(request("commit", "transfer-1", "actor-2", "player", 1_000)));
        assertThrows(
            ZLinkConfigurationException.class,
            () -> transfers.take(request("commit", "transfer-1", "actor-1", "player", 1_000)));
    }

    @Test
    void deadlineRemovesAdmissionWithoutSourceDownSignal() throws Exception {
        ZLinkPendingActorTransfers transfers = new ZLinkPendingActorTransfers();
        transfers.add(request("admission", "transfer-1", "actor-1", "player", 10), null, null);

        CountDownLatch deadline = new CountDownLatch(1);
        java.util.concurrent.CompletableFuture.delayedExecutor(100, TimeUnit.MILLISECONDS)
            .execute(deadline::countDown);
        org.junit.jupiter.api.Assertions.assertTrue(deadline.await(1, TimeUnit.SECONDS));

        assertThrows(
            ZLinkConfigurationException.class,
            () -> transfers.take(request("commit", "transfer-1", "actor-1", "player", 10)));
    }

    private static ZLinkActorSpotRoutePackets.TransferRequest request(
        String phase,
        String transferId,
        String actorId,
        String actorType,
        long timeoutMillis) {
        return new ZLinkActorSpotRoutePackets.TransferRequest(
            phase,
            transferId,
            timeoutMillis,
            actorId,
            actorType,
            RoutingId.from("source-node"),
            7,
            RoutingId.from("source-entry-node"),
            "source-entry",
            "source-entry-router",
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
            null);
    }
}
