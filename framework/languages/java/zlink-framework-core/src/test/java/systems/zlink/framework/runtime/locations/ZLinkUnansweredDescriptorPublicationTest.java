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

import java.lang.reflect.Field;
import java.time.Duration;
import java.util.UUID;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.Executor;
import java.util.concurrent.FutureTask;
import java.util.concurrent.ScheduledExecutorService;
import java.util.concurrent.TimeUnit;
import java.util.function.LongSupplier;

final class ZLinkUnansweredDescriptorPublicationTest {
    @ParameterizedTest
    @CsvSource({"READ, true", "WRITE, true", "CONFIRM, true", "WRITE, false"})
    void unansweredPublicationEndsPreparingAtRenewTimeoutAndRetries(
            Phase phase, boolean lateSuccess) throws Exception {
        var store = new PendingStore(phase);
        var renewTimeout = Duration.ofSeconds(3);
        var options = new DefaultZLinkFrameworkOptions();
        options.addLocationStore(store);
        options.configureLocations().setOwnerLeaseTtl(Duration.ofHours(1));
        options.configureLocations().setOwnerLeaseRenewInterval(Duration.ofMinutes(10));
        options.configureLocations().setOwnerLeaseRenewTimeout(renewTimeout);
        options.configureLocations().setOwnerLeaseFencingMargin(Duration.ofSeconds(1));
        options.addRouteMesh("game")
                .listen("inproc://unanswered-" + UUID.randomUUID())
                .setRoutingId(RoutingId.from("unanswered"));
        try (var host =
                ZLinkFrameworkRuntimeTestAccess.start(
                        options, new ZLinkJavaBackendAdapterFactory())) {
            var runtime = (ZLinkLocationRuntime) field(host, "locationRuntime").get(host);
            var previousScheduler =
                    (ScheduledExecutorService) field(runtime, "heartbeatExecutor").get(runtime);
            var scheduler = new ZLinkLocationFirstHeartbeatTest.RecordingScheduler();
            // The claim is held before any startup continuation runs. Reuse the existing
            // scheduler seam and run state turns inline, as in AdmissionRecoveryTest.
            field(runtime, "heartbeatExecutor").set(runtime, scheduler);
            long now = System.nanoTime();
            field(runtime, "monotonicNow").set(runtime, (LongSupplier) () -> now);
            var lane = (ZLinkStateLane) field(runtime, "stateLane").get(runtime);
            field(lane, "executor").set(lane, (Executor) Runnable::run);
            try {
                store.releaseClaim();
                var startup =
                        ZLinkFrameworkRuntimeTestAccess.startupCompletion(host)
                                .toCompletableFuture();
                assertTrue(store.blocked, "fixture must reach the selected Store boundary");
                assertFalse(startup.isDone());
                // Advance only recorded deadlines; no sleep or elapsed-time assertion.
                var deadlines =
                        scheduler.tasks.stream()
                                .filter(
                                        task ->
                                                !task.isDone()
                                                        && task.getDelay(TimeUnit.NANOSECONDS)
                                                                <= renewTimeout.toNanos())
                                .toList();
                deadlines.forEach(FutureTask::run);
                assertTrue(startup.isDone(), "renew timeout must end unconfirmed startup");
                startup.join();
                assertEquals(1, deadlines.size());
                assertEquals(
                        renewTimeout.toNanos(),
                        deadlines.getFirst().getDelay(TimeUnit.NANOSECONDS));
                assertEquals(ZLinkFrameworkRuntimeState.PREPARING, host.status().state());
                assertFalse(host.isReady());
                assertTrue(ZLinkFrameworkRuntimeTestAccess.ownerAdmissionOpen(host));
                assertFalse(store.pending.isDone(), "timeout must preserve the Store operation");
                assertFalse(store.cancellation.isCancellationRequested());

                int requests = store.descriptorRequests;
                store.blocking = false;
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
                previousScheduler.shutdown();
                assertTrue(previousScheduler.awaitTermination(30, TimeUnit.SECONDS));
            }
        }
    }

    private static Field field(Object target, String name) throws Exception {
        var field = target.getClass().getDeclaredField(name);
        field.setAccessible(true);
        return field;
    }

    private enum Phase {
        READ,
        WRITE,
        CONFIRM
    }

    private static final class PendingStore implements ZLinkLocationStore {
        final ZLinkInMemoryProviderLocationStore inner = new ZLinkInMemoryProviderLocationStore();
        final CompletableFuture<ZLinkStoreWriteResult> claim = new CompletableFuture<>();
        final CompletableFuture<Object> pending = new CompletableFuture<>();
        final Phase phase;
        ZLinkStoreWriteRequest claimRequest;
        ZLinkStoreWriteRequest pendingWrite;
        ZLinkStoreKey pendingRead;
        ZLinkStoreCancellation cancellation;
        boolean blocking = true;
        boolean blocked;
        boolean confirming;
        int descriptorRequests;

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
                                    if (blocking
                                            && !blocked
                                            && (phase == Phase.CONFIRM && confirming
                                                    || phase == Phase.READ)) {
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
                return claim;
            }
            if (request.mutations().getFirst() instanceof ZLinkStorePut put
                    && put.key().value().startsWith("mesh-node\0")) {
                descriptorRequests++;
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
