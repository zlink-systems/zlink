package systems.zlink.framework.runtime.spots;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertTrue;

import org.junit.jupiter.api.Test;

import systems.zlink.framework.runtime.internal.drain.AsyncDrainProbe;

import java.util.List;
import java.util.concurrent.CompletableFuture;

final class ZLinkSpotCloseCoordinatorTest {
    @Test
    void closeOwnsEachStepUntilItsCompletion() {
        CompletableFuture<Void> release = new CompletableFuture<>();
        var coordinator =
                new ZLinkSpotCloseCoordinator(
                        () -> CompletableFuture.completedFuture(true),
                        List.of(ZLinkSpotCloseCoordinator.Step.operation(() -> release)),
                        ignored -> {});

        var closing = coordinator.close();
        assertEquals(
                List.of(new AsyncDrainProbe.Pending("spot-close-step:0", "Spot Close")),
                pending(coordinator));
        release.complete(null);
        assertTrue(closing.toCompletableFuture().join());
        assertTrue(pending(coordinator).isEmpty());
    }

    private static List<AsyncDrainProbe.Pending> pending(Object owner) {
        try {
            var field = owner.getClass().getDeclaredField("debugProbe");
            field.setAccessible(true);
            return ((AsyncDrainProbe) field.get(owner)).pending();
        } catch (ReflectiveOperationException failure) {
            throw new AssertionError(failure);
        }
    }
}
