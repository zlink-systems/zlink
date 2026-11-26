package systems.zlink.framework.runtime.spots;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertThrows;
import static org.junit.jupiter.api.Assertions.assertTrue;

import org.junit.jupiter.params.ParameterizedTest;
import org.junit.jupiter.params.provider.ValueSource;

import systems.zlink.framework.runtime.locations.ZLinkInMemoryLocationStore;
import systems.zlink.framework.runtime.locations.ZLinkOwnerLeaseTestClock;
import systems.zlink.framework.runtime.locations.ZLinkRegisteredLocationStores;

import java.time.Duration;
import java.time.Instant;
import java.util.concurrent.CompletionException;

final class ZLinkRestoreAdmissionTest {
    @ParameterizedTest
    @ValueSource(booleans = {false, true})
    void restoreChecksLocalLeaseDeadlineBeforeStorePrepare(boolean expired) {
        var clock = new ZLinkOwnerLeaseTestClock();
        try (var owner =
                clock.runtime(
                        ZLinkRegisteredLocationStores.fromUnified(
                                new ZLinkInMemoryLocationStore()))) {
            var fixture =
                    new ZLinkCanonicalRelocationStateMachineTest()
                            .restoreAdmissionFixture(owner::isOwnerAdmissionOpen);
            owner.start(fixture.targetRid()).toCompletableFuture().join();
            assertTrue(owner.isOwnerAdmissionOpen());
            if (expired) {
                clock.expire();
                assertFalse(owner.isOwnerAdmissionOpen());
            }

            var result =
                    fixture.source()
                            .stage(
                                    fixture.targetRid(),
                                    fixture.request(new byte[] {1}),
                                    Duration.ofSeconds(2),
                                    Instant.now().plusSeconds(10))
                            .toCompletableFuture();
            if (expired) {
                assertThrows(CompletionException.class, result::join);
            } else {
                result.join();
            }
            assertEquals(expired ? 0 : 1, fixture.prepareInvocations().get());
        }
    }
}
