package systems.zlink.framework.runtime.internal.drain;

import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertNull;
import static org.junit.jupiter.api.Assertions.assertTrue;

import java.util.List;
import org.junit.jupiter.api.Test;

final class ZLinkMeshDrainCoordinatorTest {
    @Test
    void sealAtomicallyRejectsNewClaimsAndWaitsForAcceptedClaim() {
        ZLinkMeshDrainCoordinator coordinator =
            new ZLinkMeshDrainCoordinator(List.of("play", "api"));
        ZLinkMeshDrainCoordinator.Claim accepted = coordinator.tryClaim("play");

        var barrier = coordinator.sealAndAwaitZero("play").toCompletableFuture();

        assertTrue(coordinator.isSealed("play"));
        assertNull(coordinator.tryClaim("play"));
        assertFalse(barrier.isDone());
        assertFalse(coordinator.isSealed("api"));

        accepted.close();

        assertTrue(barrier.isDone());
    }

    @Test
    void claimReleaseIsIdempotent() {
        ZLinkMeshDrainCoordinator coordinator =
            new ZLinkMeshDrainCoordinator(List.of("mesh"));
        ZLinkMeshDrainCoordinator.Claim accepted = coordinator.tryClaim("mesh");
        var barrier = coordinator.sealAndAwaitZero("mesh").toCompletableFuture();

        accepted.close();
        accepted.close();

        assertTrue(barrier.isDone());
    }
}
