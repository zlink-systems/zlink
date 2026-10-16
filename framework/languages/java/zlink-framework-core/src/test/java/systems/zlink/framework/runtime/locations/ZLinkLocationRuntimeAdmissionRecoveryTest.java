package systems.zlink.framework.runtime.locations;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertTrue;

import org.junit.jupiter.params.ParameterizedTest;
import org.junit.jupiter.params.provider.CsvSource;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.runtime.internal.execution.ZLinkStateLane;
import systems.zlink.framework.runtime.internal.locations.ZLinkLocationOwnerToken;
import systems.zlink.framework.runtime.internal.locations.ZLinkOwnerLeaseClaimResult;
import systems.zlink.framework.runtime.internal.locations.ZLinkOwnerLeaseClaimed;
import systems.zlink.framework.runtime.internal.locations.ZLinkOwnerLeaseRenewResult;
import systems.zlink.framework.runtime.internal.locations.ZLinkOwnerLeaseRenewed;
import systems.zlink.framework.testing.ZLinkLocationStoreTestAdapter;

import java.time.Duration;
import java.time.Instant;
import java.util.concurrent.CancellationException;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.concurrent.atomic.AtomicLong;

final class ZLinkLocationRuntimeAdmissionRecoveryTest {
    @ParameterizedTest
    @CsvSource({"false, false", "true, false", "true, true"})
    void renewalResumesOwnerBoundWorkOnlyWhenAdmissionClosedBeforeCompletion(
            boolean expiresDuringRenewal, boolean queuedResult) throws Exception {
        var clock = new AtomicLong(Duration.ofSeconds(1).toNanos());
        var scheduler = new ZLinkLocationFirstHeartbeatTest.RecordingScheduler();
        var response = new CompletableFuture<ZLinkOwnerLeaseRenewResult>();
        var submitted = new CompletableFuture<Void>();
        var recoveries = new AtomicInteger();
        var releaseTurn = new CompletableFuture<Void>();
        var store =
                new ZLinkLocationStoreTestAdapter() {
                    @Override
                    public CompletionStage<ZLinkOwnerLeaseClaimResult> claimOwnerLease(
                            String ownerId, Duration ttl) {
                        return CompletableFuture.completedFuture(
                                new ZLinkOwnerLeaseClaimed(
                                        new ZLinkLocationOwnerToken(ownerId, 1),
                                        Instant.EPOCH.plus(ttl),
                                        Instant.EPOCH));
                    }

                    @Override
                    public CompletionStage<ZLinkOwnerLeaseRenewResult> renewOwnerLease(
                            ZLinkLocationOwnerToken token, Duration ttl) {
                        submitted.complete(null);
                        return response;
                    }
                };
        try (var runtime =
                new ZLinkLocationRuntime(
                        ZLinkRegisteredLocationStores.fromUnified(store),
                        "owner",
                        Duration.ofSeconds(15),
                        Duration.ofSeconds(5),
                        Duration.ofSeconds(3),
                        Duration.ofSeconds(5),
                        clock::get,
                        scheduler)) {
            try {
                runtime.start(RoutingId.from("node"))
                        .toCompletableFuture()
                        .get(30, TimeUnit.SECONDS);
                runtime.setOwnerLeaseRecoveryListener(
                        () -> {
                            recoveries.incrementAndGet();
                            assertTrue(runtime.isOwnerAdmissionOpen());
                            return CompletableFuture.completedFuture(null);
                        });
                clock.set(Duration.ofSeconds(10).toNanos());
                assertTrue(runtime.isOwnerAdmissionOpen());
                var renewal = runtime.renewOwnerLeaseOnce().toCompletableFuture();
                submitted.get(30, TimeUnit.SECONDS);
                assertFalse(renewal.isDone());
                CompletionStage<Void> heldTurn = CompletableFuture.completedFuture(null);
                var renewed =
                        new ZLinkOwnerLeaseRenewed(Instant.EPOCH.plusSeconds(15), Instant.EPOCH);
                if (queuedResult) {
                    var field = ZLinkLocationRuntime.class.getDeclaredField("stateLane");
                    field.setAccessible(true);
                    var lane = (ZLinkStateLane) field.get(runtime);
                    var entered = new CompletableFuture<Void>();
                    heldTurn =
                            lane.runAsync(
                                    () -> {
                                        entered.complete(null);
                                        releaseTurn.join();
                                    });
                    entered.get(30, TimeUnit.SECONDS);
                    response.complete(renewed);
                }
                if (expiresDuringRenewal) {
                    clock.set(Duration.ofSeconds(11).toNanos());
                    if (!queuedResult) assertFalse(runtime.isOwnerAdmissionOpen());
                }
                if (!queuedResult) response.complete(renewed);
                assertFalse(
                        queuedResult && renewal.isDone(),
                        "queued result must wait for its state turn");
                releaseTurn.complete(null);
                heldTurn.toCompletableFuture().get(30, TimeUnit.SECONDS);
                assertTrue(renewal.get(30, TimeUnit.SECONDS));
                assertTrue(runtime.isOwnerAdmissionOpen());
                assertEquals(expiresDuringRenewal ? 1 : 0, recoveries.get());
                clock.set(Duration.ofSeconds(20).toNanos());
                assertFalse(
                        runtime.isOwnerAdmissionOpen(),
                        "renewal keeps the pre-request deadline bound after a delayed response");
            } finally {
                releaseTurn.complete(null);
                response.completeExceptionally(new CancellationException("test cleanup"));
            }
        }
    }
}
