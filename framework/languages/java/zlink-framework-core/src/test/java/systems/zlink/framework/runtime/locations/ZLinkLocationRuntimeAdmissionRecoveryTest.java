package systems.zlink.framework.runtime.locations;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertThrows;
import static org.junit.jupiter.api.Assertions.assertTrue;

import org.junit.jupiter.api.Test;
import org.junit.jupiter.params.ParameterizedTest;
import org.junit.jupiter.params.provider.CsvSource;
import org.junit.jupiter.params.provider.ValueSource;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.runtime.internal.execution.ZLinkStateLane;
import systems.zlink.framework.runtime.internal.locations.ZLinkLocationOwnerToken;
import systems.zlink.framework.runtime.internal.locations.ZLinkOwnerLeaseClaimResult;
import systems.zlink.framework.runtime.internal.locations.ZLinkOwnerLeaseClaimed;
import systems.zlink.framework.runtime.internal.locations.ZLinkOwnerLeaseRenewResult;
import systems.zlink.framework.runtime.internal.locations.ZLinkOwnerLeaseRenewed;
import systems.zlink.framework.testing.ZLinkLocationStoreTestAdapter;

import java.time.Clock;
import java.time.Duration;
import java.time.Instant;
import java.time.ZoneOffset;
import java.util.concurrent.CancellationException;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.concurrent.atomic.AtomicLong;

final class ZLinkLocationRuntimeAdmissionRecoveryTest {
    @Test
    void stopCancelsRecoveryPublicationAndLateStoreCompletionDoesNotContinue() throws Exception {
        var store = new ZLinkInMemoryLocationStore(Clock.fixed(Instant.EPOCH, ZoneOffset.UTC));
        var scheduler = new ZLinkLocationFirstHeartbeatTest.RecordingScheduler();
        var descriptorWrite = new CompletableFuture<Void>();
        var descriptorRequested = new CompletableFuture<Void>();
        var publications = new AtomicInteger();
        var timeout = Duration.ofMillis(500);
        try (var runtime =
                new ZLinkLocationRuntime(
                        ZLinkRegisteredLocationStores.fromUnified(store),
                        "recovery-stop",
                        Duration.ofSeconds(15),
                        Duration.ofSeconds(1),
                        timeout,
                        Duration.ZERO,
                        () -> 0L,
                        scheduler)) {
            runtime.start(RoutingId.from("node-a")).toCompletableFuture().join();
            runtime.setOwnerLeaseRecoveryListener(
                    ignored -> {
                        publications.incrementAndGet();
                        return runtime.writeDescriptor(
                                        () -> {
                                            descriptorRequested.complete(null);
                                            return descriptorWrite;
                                        })
                                .thenRun(publications::incrementAndGet);
                    });
            var publication = runtime.publishWithStartupOwnerLease().toCompletableFuture();
            descriptorRequested.get(1, TimeUnit.SECONDS);
            assertFalse(publication.isDone());
            runtime.stop().toCompletableFuture().get(timeout.toNanos(), TimeUnit.NANOSECONDS);
            try {
                assertTrue(publication.isDone(), "stop cancels the recovery publication wait");
            } finally {
                descriptorWrite.complete(null);
            }
            assertTrue(publication.isCompletedExceptionally());
            assertEquals(1, publications.get(), "a late Store completion must not publish again");
        }
    }

    @Test
    void recoveryPublicationTimeoutReleasesSingleFlightForTheNextRenewal() throws Exception {
        var store = new ZLinkInMemoryLocationStore(Clock.fixed(Instant.EPOCH, ZoneOffset.UTC));
        var scheduler = new ZLinkLocationFirstHeartbeatTest.RecordingScheduler();
        var storePublication = new CompletableFuture<Void>();
        var publications = new AtomicInteger();
        var clock = new AtomicLong(1L);
        var timeout = Duration.ofMillis(50);
        try (var runtime =
                new ZLinkLocationRuntime(
                        ZLinkRegisteredLocationStores.fromUnified(store),
                        "recovery-timeout",
                        Duration.ofSeconds(15),
                        Duration.ofSeconds(1),
                        timeout,
                        Duration.ZERO,
                        clock::get,
                        scheduler)) {
            runtime.start(RoutingId.from("node-a")).toCompletableFuture().join();
            runtime.setOwnerLeaseRecoveryListener(
                    ignored ->
                            publications.incrementAndGet() == 1
                                    ? runtime.writeDescriptor(() -> storePublication)
                                    : CompletableFuture.completedFuture(null));
            var scheduled = scheduler.nextTask();
            var publication = runtime.publishWithStartupOwnerLease().toCompletableFuture();
            assertFalse(publication.isDone());
            var deadline = scheduled.get(1, TimeUnit.SECONDS);
            assertEquals(timeout.toNanos(), deadline.getDelay(TimeUnit.NANOSECONDS));
            clock.addAndGet(timeout.toNanos());
            deadline.run();
            var failure =
                    assertThrows(
                            java.util.concurrent.ExecutionException.class,
                            () -> publication.get(1, TimeUnit.SECONDS));
            assertTrue(failure.getCause() instanceof java.util.concurrent.TimeoutException);
            assertTrue(runtime.renewOwnerLeaseOnce().toCompletableFuture().join());
            assertEquals(2, publications.get());
            storePublication.complete(null);
            assertEquals(2, publications.get());
        } finally {
            storePublication.complete(null);
        }
    }

    @ParameterizedTest
    @ValueSource(booleans = {true, false})
    void renewalsJoinTheStartupOrRenewalPublicationAlreadyInFlight(boolean startupPublication)
            throws Exception {
        var store = new ZLinkInMemoryLocationStore(Clock.fixed(Instant.EPOCH, ZoneOffset.UTC));
        var scheduler = new ZLinkLocationFirstHeartbeatTest.RecordingScheduler();
        var publication = new CompletableFuture<Void>();
        var publications = new AtomicInteger();
        try (var runtime =
                new ZLinkLocationRuntime(
                        ZLinkRegisteredLocationStores.fromUnified(store),
                        "owner",
                        Duration.ofSeconds(15),
                        Duration.ofSeconds(5),
                        Duration.ofSeconds(3),
                        Duration.ofSeconds(5),
                        () -> 0L,
                        scheduler,
                        new ZLinkStateLane(Runnable::run))) {
            // Run lane turns inline so return from renew proves its publication dispatch ran.
            // The pending publication remains asynchronous and never holds a lane turn.
            runtime.setOwnerLeaseRecoveryListener(
                    admissionWasOpen -> {
                        publications.incrementAndGet();
                        return publication;
                    });
            runtime.start(RoutingId.from("node")).toCompletableFuture().join();
            try {
                var first =
                        startupPublication
                                ? runtime.publishWithStartupOwnerLease()
                                : runtime.renewOwnerLeaseOnce();
                assertEquals(1, publications.get());
                var second = runtime.renewOwnerLeaseOnce();
                var third = runtime.renewOwnerLeaseOnce();
                assertEquals(1, publications.get(), "renewals must join the running publication");
                assertTrue(runtime.isOwnerAdmissionOpen());
                assertFalse(first.toCompletableFuture().isDone());
                assertFalse(second.toCompletableFuture().isDone());
                assertFalse(third.toCompletableFuture().isDone());
                publication.complete(null);
                first.toCompletableFuture().join();
                assertTrue(second.toCompletableFuture().join());
                assertTrue(third.toCompletableFuture().join());
                assertTrue(runtime.renewOwnerLeaseOnce().toCompletableFuture().join());
                assertEquals(2, publications.get(), "the next renewal retries after completion");
            } finally {
                publication.complete(null);
            }
        }
    }

    @ParameterizedTest
    @CsvSource({"false, false", "true, false", "true, true"})
    void renewalReportsPriorAdmissionAndResumesRecoveredWork(
            boolean expiresDuringRenewal, boolean queuedResult) throws Exception {
        var clock = new AtomicLong(Duration.ofSeconds(1).toNanos());
        var scheduler = new ZLinkLocationFirstHeartbeatTest.RecordingScheduler();
        var response = new CompletableFuture<ZLinkOwnerLeaseRenewResult>();
        var submitted = new CompletableFuture<Void>();
        var recoveries = new AtomicInteger();
        var renewals = new AtomicInteger();
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
                        admissionWasOpen -> {
                            renewals.incrementAndGet();
                            if (!admissionWasOpen) recoveries.incrementAndGet();
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
                    var lane = runtime.stateLane;
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
                assertEquals(1, renewals.get());
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
