package systems.zlink.framework.runtime.locations;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertTrue;

import org.junit.jupiter.params.ParameterizedTest;
import org.junit.jupiter.params.provider.CsvSource;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.locationprovider.*;
import systems.zlink.framework.runtime.binding.ZLinkJavaBackendAdapterFactory;
import systems.zlink.framework.runtime.configuration.DefaultZLinkFrameworkOptions;
import systems.zlink.framework.runtime.host.ZLinkFrameworkRuntimeState;
import systems.zlink.framework.runtime.host.ZLinkFrameworkRuntimeTestAccess;
import systems.zlink.framework.runtime.internal.execution.ZLinkStateLane;
import systems.zlink.framework.runtime.internal.locations.ZLinkProviderLocationRepository;

import java.time.Duration;
import java.util.UUID;
import java.util.concurrent.Callable;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.ScheduledFuture;
import java.util.concurrent.ScheduledThreadPoolExecutor;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicLong;

final class ZLinkUnansweredDescriptorPublicationTest {
    @ParameterizedTest
    @CsvSource({"READ, true", "WRITE, true", "CONFIRM, true", "WRITE, false"})
    void unansweredPublicationEndsPreparingAtRenewTimeoutAndRetries(
            Phase phase, boolean lateSuccess) throws Exception {
        var descriptorClock = new AtomicLong();
        var store = new PendingStore(phase);
        var renewTimeout = Duration.ofSeconds(3);
        var options = new DefaultZLinkFrameworkOptions();
        options.addLocationStore(store);
        options.configureLocations().setOwnerLeaseTtl(Duration.ofHours(1));
        options.configureLocations().setOwnerLeaseRenewInterval(Duration.ofMinutes(10));
        options.configureLocations().setOwnerLeaseRenewTimeout(renewTimeout);
        options.configureLocations().setOwnerLeaseFencingMargin(Duration.ofNanos(1));
        options.addRouteMesh("game")
                .listen("inproc://unanswered-" + UUID.randomUUID())
                .setRoutingId(RoutingId.from("unanswered"));
        var scheduler = new PublicationScheduler(renewTimeout, store);
        var clock = new AtomicLong(System.nanoTime());
        Duration fencingMargin = Duration.ofNanos(1);
        var locations =
                ZLinkRegisteredLocationStores.fromUnified(
                        new ZLinkProviderLocationRepository(
                                store, descriptorClock::get, fencingMargin));
        var runtime =
                new ZLinkLocationRuntime(
                        locations,
                        "unanswered",
                        Duration.ofHours(1),
                        Duration.ofMinutes(10),
                        renewTimeout,
                        fencingMargin,
                        clock::get,
                        scheduler,
                        new ZLinkStateLane());
        try (var host =
                ZLinkFrameworkRuntimeTestAccess.startWithConstruction(
                        options,
                        new ZLinkJavaBackendAdapterFactory(),
                        systems.zlink.framework.runtime.internal.handlers.ZLinkHandlerActivator
                                .reflection(),
                        null,
                        new ZLinkFrameworkRuntimeTestAccess.Construction()
                                .locationRuntime(runtime)
                                .locationStores(locations))) {
            // The claim is held before startup continues. Observe the publication deadline
            // at its scheduler boundary rather than assuming the claim completes startup inline.
            try {
                store.claimRequested.get(renewTimeout.toNanos(), TimeUnit.NANOSECONDS);
                store.releaseClaim();
                var startup =
                        ZLinkFrameworkRuntimeTestAccess.startupCompletion(host)
                                .toCompletableFuture();
                startup.get(renewTimeout.toNanos(), TimeUnit.NANOSECONDS);
                store.descriptorReadObserved.get(renewTimeout.toNanos(), TimeUnit.NANOSECONDS);
                runtime.renewOwnerLeaseOnce()
                        .toCompletableFuture()
                        .get(renewTimeout.toNanos(), TimeUnit.NANOSECONDS);
                String descriptorKey = "mesh-node\0game\0" + RoutingId.from("unanswered").toHex();
                assertEquals(0, store.targetDescriptorWrites.get());
                assertEquals(ZLinkFrameworkRuntimeState.PREPARING, host.status().state());
                assertFalse(host.isReady());
                store.blocking = true;
                descriptorClock.set(fencingMargin.multipliedBy(3).toNanos());
                var pendingPublication = runtime.renewOwnerLeaseOnce().toCompletableFuture();
                ZLinkLocationFirstHeartbeatTest.ScheduledTask<?> deadline;
                try {
                    deadline =
                            scheduler.publicationDeadline.get(
                                    renewTimeout.toNanos(), TimeUnit.NANOSECONDS);
                } catch (java.util.concurrent.TimeoutException failure) {
                    throw new AssertionError(
                            "publication boundary missing: phase="
                                    + phase
                                    + " blocked="
                                    + store.blocked
                                    + " descriptorReads="
                                    + store.descriptorReads
                                    + " descriptorWrites="
                                    + store.descriptorWrites
                                    + " startupDone="
                                    + startup.isDone()
                                    + " retryDone="
                                    + pendingPublication.isDone(),
                            failure);
                }
                assertTrue(store.blocked, "fixture must reach the selected Store boundary");
                assertFalse(pendingPublication.isDone());
                // Advance only recorded deadlines; no sleep or elapsed-time assertion.
                deadline.run();
                pendingPublication.get(renewTimeout.toNanos(), TimeUnit.NANOSECONDS);
                assertEquals(renewTimeout.toNanos(), deadline.getDelay(TimeUnit.NANOSECONDS));
                assertEquals(ZLinkFrameworkRuntimeState.PREPARING, host.status().state());
                assertFalse(host.isReady());
                assertTrue(ZLinkFrameworkRuntimeTestAccess.ownerAdmissionOpen(host));
                assertFalse(store.pending.isDone(), "timeout must preserve the Store operation");
                assertFalse(store.cancellation.isCancellationRequested());

                store.blocking = false;
                int requests = store.descriptorRequests;
                assertTrue(runtime.renewOwnerLeaseOnce().toCompletableFuture().join());
                assertTrue(store.descriptorRequests > requests, "the next renew must republish");
                assertEquals(ZLinkFrameworkRuntimeState.SERVING, host.status().state());
                assertTrue(host.isReady());
                // Complete the old Store call after a newer publication. Its continuation
                // must finish naturally without publishing an old host state.
                store.releasePending(lateSuccess);
                assertEquals(ZLinkFrameworkRuntimeState.SERVING, host.status().state());
                assertTrue(ZLinkFrameworkRuntimeTestAccess.ownerAdmissionOpen(host));
            } finally {
                store.blocking = false;
                store.releaseClaim();
                store.releasePending(true);
            }
        }
    }

    private static final class PublicationScheduler extends ScheduledThreadPoolExecutor {
        final CompletableFuture<ZLinkLocationFirstHeartbeatTest.ScheduledTask<?>>
                publicationDeadline = new CompletableFuture<>();
        private final long renewTimeoutNanos;
        private final PendingStore store;

        PublicationScheduler(Duration renewTimeout, PendingStore store) {
            super(1);
            renewTimeoutNanos = renewTimeout.toNanos();
            this.store = store;
        }

        @Override
        public ScheduledFuture<?> schedule(Runnable command, long delay, TimeUnit unit) {
            return record(
                    new ZLinkLocationFirstHeartbeatTest.ScheduledTask<Void>(
                            command, unit.toNanos(delay)));
        }

        @Override
        public <V> ScheduledFuture<V> schedule(Callable<V> command, long delay, TimeUnit unit) {
            return record(
                    new ZLinkLocationFirstHeartbeatTest.ScheduledTask<V>(
                            command, unit.toNanos(delay)));
        }

        private <V> ScheduledFuture<V> record(
                ZLinkLocationFirstHeartbeatTest.ScheduledTask<V> task) {
            if (store.blocked && task.getDelay(TimeUnit.NANOSECONDS) <= renewTimeoutNanos) {
                var first = publicationDeadline.getNow(null);
                assertTrue(first == null || first.isDone(), "one pending publication deadline");
                publicationDeadline.complete(task);
            }
            return task;
        }
    }

    private enum Phase {
        READ,
        WRITE,
        CONFIRM
    }

    private static final class PendingStore implements ZLinkLocationStore {
        final ZLinkInMemoryProviderLocationStore inner = new ZLinkInMemoryProviderLocationStore();
        final CompletableFuture<ZLinkStoreWriteResult> claim = new CompletableFuture<>();
        final CompletableFuture<Void> claimRequested = new CompletableFuture<>();
        final CompletableFuture<Void> descriptorReadObserved = new CompletableFuture<>();
        final CompletableFuture<Object> pending = new CompletableFuture<>();
        final Phase phase;
        final java.util.concurrent.atomic.AtomicInteger targetDescriptorWrites =
                new java.util.concurrent.atomic.AtomicInteger();
        ZLinkStoreWriteRequest claimRequest;
        ZLinkStoreWriteRequest pendingWrite;
        ZLinkStoreKey pendingRead;
        ZLinkStoreCancellation cancellation;
        boolean blocking;
        boolean blocked;
        boolean confirming;
        boolean descriptorWriteAttempted;
        int descriptorRequests;
        int descriptorReads;
        int descriptorWrites;

        PendingStore(Phase phase) {
            this.phase = phase;
        }

        @Override
        public CompletionStage<ZLinkStoreReadResult> read(
                ZLinkStoreKey key, ZLinkStoreCancellation cancellation) {
            return inner.read(key, cancellation)
                    .thenCompose(
                            result -> {
                                // Select the canonical MeshNode key without parsing payload bytes.
                                if (key.value().startsWith("mesh-node\0")) {
                                    descriptorRequests++;
                                    descriptorReads++;
                                    descriptorReadObserved.complete(null);
                                    if (blocking
                                            && !blocked
                                            && (phase == Phase.CONFIRM && confirming
                                                    || phase == Phase.READ
                                                            && descriptorWriteAttempted)) {
                                        blocked = true;
                                        pendingRead = key;
                                        this.cancellation = cancellation;
                                        return pending.thenApply(
                                                value -> (ZLinkStoreReadResult) value);
                                    }
                                }
                                return CompletableFuture.completedFuture(result);
                            });
        }

        @Override
        public CompletionStage<ZLinkStoreWriteResult> write(
                ZLinkStoreWriteRequest request, ZLinkStoreCancellation cancellation) {
            if (claimRequest == null) {
                claimRequest = request;
                claimRequested.complete(null);
                return claim;
            }
            if (request.mutations().getFirst() instanceof ZLinkStorePut put
                    && put.key().value().startsWith("mesh-node\0")) {
                if (put.key()
                        .value()
                        .equals("mesh-node\0game\0" + RoutingId.from("unanswered").toHex()))
                    targetDescriptorWrites.incrementAndGet();
                descriptorWriteAttempted = true;
                descriptorRequests++;
                descriptorWrites++;
                if (blocking && !blocked && phase != Phase.READ) {
                    if (phase == Phase.CONFIRM) {
                        confirming = true;
                        return CompletableFuture.failedFuture(
                                new IllegalStateException("lost response"));
                    }
                    blocked = true;
                    pendingWrite = request;
                    this.cancellation = cancellation;
                    return pending.thenApply(value -> (ZLinkStoreWriteResult) value);
                }
            }
            return inner.write(request, cancellation);
        }

        @Override
        public CompletionStage<ZLinkStoreScanResult> scan(
                ZLinkStoreScanRequest request, ZLinkStoreCancellation cancellation) {
            return inner.scan(request, cancellation);
        }

        void releaseClaim() {
            if (!claim.isDone())
                claim.complete(inner.write(claimRequest, () -> false).toCompletableFuture().join());
        }

        void releasePending(boolean success) {
            if (!blocked || pending.isDone()) return;
            if (!success) pending.completeExceptionally(new IllegalStateException("late failure"));
            else
                pending.complete(
                        pendingWrite != null
                                ? inner.write(pendingWrite, () -> false)
                                        .toCompletableFuture()
                                        .join()
                                : inner.read(pendingRead, () -> false)
                                        .toCompletableFuture()
                                        .join());
        }
    }
}
