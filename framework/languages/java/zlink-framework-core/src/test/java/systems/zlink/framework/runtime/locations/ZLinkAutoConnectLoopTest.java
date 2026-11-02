package systems.zlink.framework.runtime.locations;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertThrows;
import static org.junit.jupiter.api.Assertions.assertTrue;

import org.junit.jupiter.api.Test;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.locations.ZLinkLocationOptions;
import systems.zlink.framework.locations.ZLinkLocationRole;
import systems.zlink.framework.runtime.internal.locations.ZLinkAutoConnectPeer;
import systems.zlink.framework.runtime.internal.locations.ZLinkAutoConnectPeerResolver;
import systems.zlink.framework.runtime.internal.locations.ZLinkAutoConnectType;

import java.time.Duration;
import java.time.Instant;
import java.util.ArrayList;
import java.util.List;
import java.util.Map;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicInteger;

final class ZLinkAutoConnectLoopTest {
    @Test
    void immediateReconcileRefreshesAfterInFlightReadWithoutWaitingForPollingPeriod()
            throws Exception {
        var first = new CompletableFuture<List<ZLinkAutoConnectPeer>>();
        var second = new CompletableFuture<List<ZLinkAutoConnectPeer>>();
        var secondRequested = new CompletableFuture<Void>();
        var reads = new java.util.concurrent.atomic.AtomicInteger();
        ZLinkAutoConnectPeerResolver resolver =
                (type, mesh, role) -> {
                    if (reads.incrementAndGet() == 1) return first;
                    secondRequested.complete(null);
                    return second;
                };
        var options = new ZLinkLocationOptions();
        options.setPollingInterval(java.time.Duration.ofSeconds(30));
        var reconciler =
                new ZLinkAutoConnectReconciler(
                        new ZLinkAutoConnectPlanner.Local(
                                ZLinkAutoConnectType.CLIENT_SERVER,
                                "orders",
                                ZLinkLocationRole.DEALER,
                                RoutingId.from("client"),
                                "inproc://client"),
                        resolver,
                        new RecordingExecutor(),
                        options,
                        System::nanoTime,
                        failure -> {
                            throw failure;
                        });
        var loop = new ZLinkAutoConnectLoop(reconciler, options);
        try {
            loop.start();
            loop.reconcileNow();
            assertFalse(
                    secondRequested.isDone(), "an in-flight resolver read remains the sole owner");
            first.complete(List.of());
            secondRequested.get(2, TimeUnit.SECONDS);
            assertEquals(2, reads.get());
        } finally {
            first.complete(List.of());
            second.complete(List.of());
            loop.stop().toCompletableFuture().get(2, TimeUnit.SECONDS);
        }
    }

    @Test
    void startupUsesConfiguredPollingInterval() throws Exception {
        PendingResolver resolver = new PendingResolver();
        ZLinkLocationOptions options = new ZLinkLocationOptions();
        options.setPollingInterval(java.time.Duration.ofSeconds(30));
        ZLinkAutoConnectReconciler reconciler =
                new ZLinkAutoConnectReconciler(
                        new ZLinkAutoConnectPlanner.Local(
                                ZLinkAutoConnectType.CLIENT_SERVER,
                                "orders",
                                ZLinkLocationRole.DEALER,
                                RoutingId.from("client"),
                                "inproc://client"),
                        resolver,
                        new RecordingExecutor(),
                        options,
                        System::nanoTime,
                        failure -> {
                            throw failure;
                        });
        ZLinkAutoConnectLoop loop = new ZLinkAutoConnectLoop(reconciler, options);
        try {
            loop.start();
            resolver.complete(List.of());
            assertTrue(
                    TimeUnit.NANOSECONDS.toSeconds(loop.scheduledDelayNanosForTest()) > 20,
                    "startup must use the configured polling interval");
        } finally {
            loop.stop().toCompletableFuture().get(1, TimeUnit.SECONDS);
        }
    }

    @Test
    void stopCancelsPendingSnapshotAndIgnoresItsLateCompletion() throws Exception {
        PendingResolver resolver = new PendingResolver();
        RecordingExecutor executor = new RecordingExecutor();
        ZLinkLocationOptions options = new ZLinkLocationOptions();
        ZLinkAutoConnectReconciler reconciler =
                new ZLinkAutoConnectReconciler(
                        new ZLinkAutoConnectPlanner.Local(
                                ZLinkAutoConnectType.CLIENT_SERVER,
                                "orders",
                                ZLinkLocationRole.DEALER,
                                RoutingId.from("client"),
                                "inproc://client"),
                        resolver,
                        executor,
                        options,
                        System::nanoTime,
                        failure -> {
                            throw failure;
                        });
        ZLinkAutoConnectLoop loop = new ZLinkAutoConnectLoop(reconciler, options);

        options.setOwnerLeaseRenewTimeout(Duration.ofMillis(500));
        CompletionStage<Void> tick = loop.start();
        CompletableFuture<Void> stop = loop.stop().toCompletableFuture();

        try {
            stop.get(options.ownerLeaseRenewTimeout().toNanos(), TimeUnit.NANOSECONDS);
        } finally {
            resolver.complete(List.of(peer()));
        }
        assertEquals(List.of(), executor.operations);
        assertTrue(loop.stop() == stop, "stop must share its terminal result");
        assertTrue(tick.toCompletableFuture().isDone());
    }

    @Test
    void stopCancelsPendingPublicationAndLateStoreCompletionCannotPublishAgain() throws Exception {
        var storeWrite = new CompletableFuture<Void>();
        var publicationRequested = new CompletableFuture<Void>();
        var reads = new AtomicInteger();
        var publications = new AtomicInteger();
        var executor = new RecordingExecutor();
        var options = new ZLinkLocationOptions();
        options.setOwnerLeaseRenewTimeout(Duration.ofMillis(500));
        options.setPollingInterval(Duration.ofSeconds(30));
        ZLinkAutoConnectPeerResolver resolver =
                (type, mesh, role) -> {
                    reads.incrementAndGet();
                    return CompletableFuture.completedFuture(List.of(peer()));
                };
        var loop =
                new ZLinkAutoConnectLoop(
                        reconciler(resolver, executor, options),
                        options,
                        () -> {
                            publications.incrementAndGet();
                            publicationRequested.complete(null);
                            return storeWrite.minimalCompletionStage();
                        });
        try {
            var startup = loop.start();
            publicationRequested.get(1, TimeUnit.SECONDS);
            loop.reconcileNow();
            assertFalse(startup.toCompletableFuture().isDone());
            assertEquals(1, reads.get(), "the next snapshot must await this publication");
            loop.stop()
                    .toCompletableFuture()
                    .get(options.ownerLeaseRenewTimeout().toNanos(), TimeUnit.NANOSECONDS);
            assertTrue(startup.toCompletableFuture().isDone());
            storeWrite.complete(null);
            loop.reconcileNow();
            assertEquals(1, reads.get());
            assertEquals(1, publications.get());
            assertEquals(List.of("connect", "disconnect"), executor.operations);
        } finally {
            storeWrite.complete(null);
            loop.stop().toCompletableFuture().get(1, TimeUnit.SECONDS);
        }
    }

    @Test
    void snapshotsFollowPublicationCompletionInOrder() throws Exception {
        var firstPublication = new CompletableFuture<Void>();
        var secondPublication = new CompletableFuture<Void>();
        var operations = java.util.Collections.synchronizedList(new ArrayList<String>());
        var options = new ZLinkLocationOptions();
        options.setPollingInterval(Duration.ofSeconds(30));
        var reads = new AtomicInteger();
        var publications = new AtomicInteger();
        ZLinkAutoConnectPeerResolver resolver =
                (type, mesh, role) -> {
                    operations.add("snapshot-" + reads.incrementAndGet());
                    return CompletableFuture.completedFuture(List.of());
                };
        var loop =
                new ZLinkAutoConnectLoop(
                        reconciler(resolver, new RecordingExecutor(), options),
                        options,
                        () -> {
                            int round = publications.incrementAndGet();
                            operations.add("publish-" + round);
                            if (round == 1) return firstPublication;
                            secondPublication.complete(null);
                            return CompletableFuture.completedFuture(null);
                        });
        try {
            loop.start();
            loop.reconcileNow();
            assertEquals(List.of("snapshot-1", "publish-1"), operations);
            firstPublication.complete(null);
            secondPublication.get(1, TimeUnit.SECONDS);
            loop.stop().toCompletableFuture().get(1, TimeUnit.SECONDS);
            assertEquals(List.of("snapshot-1", "publish-1", "snapshot-2", "publish-2"), operations);
            loop.reconcileNow();
            assertEquals(2, reads.get());
        } finally {
            firstPublication.complete(null);
            loop.stop().toCompletableFuture().get(1, TimeUnit.SECONDS);
        }
    }

    @Test
    void lateAdmissionCompletionCannotApplyTheSnapshotAfterStop() throws Exception {
        var admission = new CompletableFuture<Void>();
        var admissionRequested = new CompletableFuture<Void>();
        RecordingExecutor executor =
                new RecordingExecutor() {
                    @Override
                    public CompletionStage<Void> observeAdmissionExpectation(
                            ZLinkAutoConnectPlanner.Target target) {
                        admissionRequested.complete(null);
                        return admission;
                    }
                };
        var options = new ZLinkLocationOptions();
        ZLinkAutoConnectPeerResolver resolver =
                (type, mesh, role) -> CompletableFuture.completedFuture(List.of(peer()));
        var loop = new ZLinkAutoConnectLoop(reconciler(resolver, executor, options), options);
        try {
            loop.start();
            admissionRequested.get(1, TimeUnit.SECONDS);
            loop.stop().toCompletableFuture().get(1, TimeUnit.SECONDS);
            admission.complete(null);
            assertEquals(List.of(), executor.operations);
        } finally {
            admission.complete(null);
            loop.stop().toCompletableFuture().get(1, TimeUnit.SECONDS);
        }
    }

    @Test
    void stalledAdmissionCleanupEndsStopAtTheRenewTimeout() throws Exception {
        var cleanup = new CompletableFuture<Void>();
        RecordingExecutor executor =
                new RecordingExecutor() {
                    @Override
                    public CompletionStage<Void> forgetAdmissionExpectation(
                            ZLinkAutoConnectPlanner.Target target) {
                        return cleanup;
                    }
                };
        var options = new ZLinkLocationOptions();
        options.setOwnerLeaseRenewTimeout(Duration.ofMillis(50));
        ZLinkAutoConnectPeerResolver resolver =
                (type, mesh, role) -> CompletableFuture.completedFuture(List.of(peer()));
        var loop = new ZLinkAutoConnectLoop(reconciler(resolver, executor, options), options);
        try {
            loop.start().toCompletableFuture().get(1, TimeUnit.SECONDS);
            var stopping = loop.stop().toCompletableFuture();
            var failure =
                    assertThrows(
                            java.util.concurrent.ExecutionException.class,
                            () -> stopping.get(1, TimeUnit.SECONDS));
            assertTrue(failure.getCause() instanceof java.util.concurrent.TimeoutException);
            cleanup.complete(null);
            assertEquals(List.of("connect", "disconnect"), executor.operations);
        } finally {
            cleanup.complete(null);
            loop.close();
        }
    }

    private static ZLinkAutoConnectReconciler reconciler(
            ZLinkAutoConnectPeerResolver resolver,
            RecordingExecutor executor,
            ZLinkLocationOptions options) {
        return new ZLinkAutoConnectReconciler(
                new ZLinkAutoConnectPlanner.Local(
                        ZLinkAutoConnectType.CLIENT_SERVER,
                        "orders",
                        ZLinkLocationRole.DEALER,
                        RoutingId.from("client"),
                        "inproc://client"),
                resolver,
                executor,
                options,
                System::nanoTime,
                failure -> {
                    throw failure;
                });
    }

    private static ZLinkAutoConnectPeer peer() {
        return new ZLinkAutoConnectPeer(
                ZLinkAutoConnectType.CLIENT_SERVER,
                "orders",
                RoutingId.from("server"),
                ZLinkLocationRole.ROUTER,
                "inproc://server",
                100,
                false,
                9,
                Map.of(),
                List.of(),
                "owner-server",
                4,
                Instant.parse("2026-07-27T00:00:00Z"));
    }

    private static final class PendingResolver implements ZLinkAutoConnectPeerResolver {
        private final CompletableFuture<List<ZLinkAutoConnectPeer>> result =
                new CompletableFuture<>();

        @Override
        public CompletionStage<List<ZLinkAutoConnectPeer>> listPeers(
                ZLinkAutoConnectType type, String meshName, ZLinkLocationRole role) {
            return result.minimalCompletionStage();
        }

        void complete(List<ZLinkAutoConnectPeer> peers) {
            result.complete(peers);
        }
    }

    private static class RecordingExecutor implements ZLinkAutoConnectExecutor {
        private final List<String> operations = new ArrayList<>();

        @Override
        public boolean connect(ZLinkAutoConnectPlanner.Target target) {
            operations.add("connect");
            return true;
        }

        @Override
        public boolean disconnect(ZLinkAutoConnectPlanner.Target target) {
            operations.add("disconnect");
            return true;
        }
    }
}
