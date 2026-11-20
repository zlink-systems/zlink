package systems.zlink.framework.runtime.locations;

import static org.junit.jupiter.api.Assertions.*;

import org.junit.jupiter.api.RepeatedTest;
import org.junit.jupiter.api.Test;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.locations.ZLinkPageRequest;
import systems.zlink.framework.runtime.binding.ZLinkJavaBackendAdapterFactory;
import systems.zlink.framework.runtime.configuration.DefaultZLinkFrameworkOptions;
import systems.zlink.framework.runtime.host.ZLinkFrameworkRuntimeTestAccess;
import systems.zlink.framework.runtime.internal.execution.ZLinkStateLane;
import systems.zlink.framework.runtime.internal.handlers.ZLinkHandlerActivator;
import systems.zlink.framework.runtime.internal.locations.*;
import systems.zlink.framework.testing.ZLinkLocationStoreTestAdapter;

import java.time.Clock;
import java.time.Duration;
import java.time.Instant;
import java.time.ZoneOffset;
import java.util.UUID;
import java.util.concurrent.*;
import java.util.concurrent.atomic.*;

final class ZLinkLocationHeartbeatBoundaryTest {
    @RepeatedTest(40)
    void pendingRenewalTimesOutAndNextHeartbeatKeepsTheSameLease() throws Exception {
        var renewals = new AtomicInteger();
        var pending = new CompletableFuture<ZLinkOwnerLeaseRenewResult>();
        var store =
                new LeaseStore() {
                    @Override
                    public CompletionStage<ZLinkOwnerLeaseRenewResult> renewOwnerLease(
                            ZLinkLocationOwnerToken token, Duration ttl) {
                        renewals.incrementAndGet();
                        return pending;
                    }
                };
        var clock = new AtomicLong(1L);
        var scheduler = new ZLinkLocationFirstHeartbeatTest.RecordingScheduler();
        try (var runtime = runtime(store, clock, scheduler)) {
            runtime.start(RoutingId.from("node")).toCompletableFuture().join();
            var owner = runtime.currentOwnerToken();
            var heartbeat = heartbeat(scheduler);
            clock.addAndGet(Duration.ofSeconds(1).toNanos());
            var timeoutScheduled = scheduler.nextTask(t -> t.getDelay(TimeUnit.SECONDS) == 3);
            heartbeat.run();
            assertEquals(1, renewals.get());
            var timeout = timeoutScheduled.get(100, TimeUnit.MILLISECONDS);
            clock.addAndGet(Duration.ofSeconds(3).toNanos());
            var nextScheduled = scheduler.nextTask(t -> t.getDelay(TimeUnit.NANOSECONDS) == 0);
            timeout.run();
            var next = nextScheduled.get(100, TimeUnit.MILLISECONDS);
            assertTrue(
                    runtime.ownerLeaseHealthy(),
                    "communication failure does not revoke the live lease");
            assertTrue(runtime.lastError().contains("timed out"));
            assertEquals(owner, runtime.currentOwnerToken());
            assertFalse(pending.isDone(), "timeout must not mutate the provider future");
            next.run();
            assertEquals(2, renewals.get());
        } finally {
            pending.completeExceptionally(new CancellationException());
        }
    }

    @RepeatedTest(40)
    void pendingRemovalAndDiscardCleanupDoNotHoldTheNextHeartbeat() throws Exception {
        var renewals = new AtomicInteger();
        var pending = new CompletableFuture<Void>();
        var removal = new CompletableFuture<ZLinkLocationWriteStatus>();
        var removals = new AtomicInteger();
        var store =
                new LeaseStore() {
                    @Override
                    public CompletionStage<ZLinkOwnerLeaseRenewResult> renewOwnerLease(
                            ZLinkLocationOwnerToken token, Duration ttl) {
                        renewals.incrementAndGet();
                        return CompletableFuture.completedFuture(
                                new ZLinkOwnerLeaseRenewed(Instant.EPOCH.plus(ttl), Instant.EPOCH));
                    }

                    @Override
                    public CompletionStage<ZLinkLocationWriteStatus> removeMeshNode(
                            ZLinkMeshNodeDescriptorKey key, ZLinkLocationOwnerToken owner) {
                        removals.incrementAndGet();
                        return removal;
                    }
                };
        var clock = new AtomicLong(1L);
        var scheduler = new ZLinkLocationFirstHeartbeatTest.RecordingScheduler();
        var publications = new AtomicInteger();
        try (var runtime = runtime(store, clock, scheduler)) {
            runtime.start(RoutingId.from("node")).toCompletableFuture().join();
            var execution =
                    runtime.executionAsync(
                            () -> null,
                            () -> false,
                            ignored -> CompletableFuture.completedFuture(null),
                            () -> null,
                            () -> pending);
            execution.ready().toCompletableFuture().join();
            var cleanup = execution.discardAsync().toCompletableFuture();
            runtime.setOwnerLeaseRecoveryListener(
                    ignored -> {
                        publications.incrementAndGet();
                        return store.removeMeshNode(
                                        new ZLinkMeshNodeDescriptorKey(
                                                "game", RoutingId.from("node")),
                                        runtime.currentOwnerToken())
                                .thenApply(removed -> null);
                    });
            // Keep renewing beyond the original lease TTL while both waits remain pending.
            for (int i = 0; i < 35; i++) {
                var task = heartbeat(scheduler);
                clock.addAndGet(Duration.ofSeconds(1).toNanos());
                var next = scheduler.nextTask(t -> t.getDelay(TimeUnit.SECONDS) == 1);
                task.run();
                next.get(100, TimeUnit.MILLISECONDS);
                assertEquals(i + 1, renewals.get());
                assertTrue(runtime.isOwnerAdmissionOpen());
            }
            assertEquals(1, publications.get(), "reuse the existing recovery single flight");
            assertEquals(1, removals.get());
            assertFalse(removal.isDone());
            assertFalse(cleanup.isDone());
        } finally {
            pending.complete(null);
            removal.complete(ZLinkLocationWriteStatus.STORED);
        }
    }

    @Test
    void servingDescriptorIsRepublishedAfterAdmissionDeadlineAcrossThreeRenewals()
            throws Exception {
        var clock = new AtomicLong(1L);
        var store = new ZLinkInMemoryLocationStore(Clock.fixed(Instant.EPOCH, ZoneOffset.UTC));
        var scheduler = new ZLinkLocationFirstHeartbeatTest.RecordingScheduler();
        var locations = ZLinkRegisteredLocationStores.fromUnified(store);
        var runtime =
                new ZLinkLocationRuntime(
                        locations,
                        "descriptor-owner",
                        Duration.ofSeconds(30),
                        Duration.ofSeconds(1),
                        Duration.ofSeconds(3),
                        Duration.ofSeconds(5),
                        clock::get,
                        scheduler,
                        new ZLinkStateLane());
        var options = new DefaultZLinkFrameworkOptions();
        options.addLocationStore(store);
        options.configureLocations().setOwnerLeaseTtl(Duration.ofSeconds(30));
        options.configureLocations().setOwnerLeaseRenewInterval(Duration.ofSeconds(1));
        options.configureLocations().setOwnerLeaseRenewTimeout(Duration.ofSeconds(3));
        options.configureLocations().setOwnerLeaseFencingMargin(Duration.ofSeconds(5));
        options.addRouteMesh("game").listen("inproc://heartbeat-" + UUID.randomUUID());
        try (var host =
                ZLinkFrameworkRuntimeTestAccess.startWithConstruction(
                        options,
                        new ZLinkJavaBackendAdapterFactory(),
                        ZLinkHandlerActivator.reflection(),
                        null,
                        new ZLinkFrameworkRuntimeTestAccess.Construction()
                                .locationRuntime(runtime)
                                .locationStores(locations))) {
            ZLinkFrameworkRuntimeTestAccess.startupCompletion(host)
                    .toCompletableFuture()
                    .get(3, TimeUnit.SECONDS);
            assertTrue(host.isReady());
            var execution = runtime.execution(() -> true, () -> {});
            execution.ready().toCompletableFuture().join();
            var owner = runtime.currentOwnerToken();
            var row =
                    store.listMeshNodes("game", ZLinkPageRequest.firstPage())
                            .toCompletableFuture()
                            .join()
                            .items()
                            .getFirst();
            assertEquals(
                    ZLinkLocationWriteStatus.STORED,
                    store.removeMeshNode(new ZLinkMeshNodeDescriptorKey("game", row.rid()), owner)
                            .toCompletableFuture()
                            .join());
            assertTrue(
                    store.listMeshNodes("game", ZLinkPageRequest.firstPage())
                            .toCompletableFuture()
                            .join()
                            .items()
                            .isEmpty());
            clock.addAndGet(Duration.ofSeconds(26).toNanos());
            assertFalse(runtime.isOwnerAdmissionOpen());
            for (int i = 0; i < 3; i++) {
                assertTrue(
                        runtime.renewOwnerLeaseOnce()
                                .toCompletableFuture()
                                .get(3, TimeUnit.SECONDS));
                assertEquals(owner, runtime.currentOwnerToken());
                assertDoesNotThrow(() -> execution.check(true));
                assertTrue(host.isReady());
                assertEquals(
                        1,
                        store.listMeshNodes("game", ZLinkPageRequest.firstPage())
                                .toCompletableFuture()
                                .join()
                                .items()
                                .size());
                clock.addAndGet(Duration.ofSeconds(1).toNanos());
            }
        }
    }

    private abstract static class LeaseStore extends ZLinkLocationStoreTestAdapter {
        @Override
        public CompletionStage<ZLinkOwnerLeaseClaimResult> claimOwnerLease(
                String ownerId, Duration ttl) {
            return CompletableFuture.completedFuture(
                    new ZLinkOwnerLeaseClaimed(
                            new ZLinkLocationOwnerToken(ownerId, 1),
                            Instant.EPOCH.plus(ttl),
                            Instant.EPOCH));
        }
    }

    private static ZLinkLocationFirstHeartbeatTest.ScheduledTask<?> heartbeat(
            ZLinkLocationFirstHeartbeatTest.RecordingScheduler scheduler) {
        return scheduler.tasks.stream()
                .filter(t -> !t.isDone() && !t.isCancelled() && t.getDelay(TimeUnit.SECONDS) == 1)
                .findFirst()
                .orElseThrow();
    }

    private static ZLinkLocationRuntime runtime(
            ZLinkLocationStoreTestAdapter store,
            AtomicLong clock,
            ZLinkLocationFirstHeartbeatTest.RecordingScheduler scheduler) {
        return new ZLinkLocationRuntime(
                ZLinkRegisteredLocationStores.fromUnified(store),
                "owner",
                Duration.ofSeconds(30),
                Duration.ofSeconds(1),
                Duration.ofSeconds(3),
                Duration.ofSeconds(5),
                clock::get,
                scheduler,
                new ZLinkStateLane(Runnable::run));
    }
}
