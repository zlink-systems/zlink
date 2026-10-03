package systems.zlink.framework.runtime.locations;

import static org.junit.jupiter.api.Assertions.*;

import org.junit.jupiter.api.Test;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.errors.ConnectResult;
import systems.zlink.contracts.errors.ZlinkConnectException;
import systems.zlink.framework.locations.ZLinkLocationOptions;
import systems.zlink.framework.locations.ZLinkLocationRole;
import systems.zlink.framework.locations.ZLinkMeshNodeObjectRole;
import systems.zlink.framework.runtime.internal.locations.*;

import java.time.Duration;
import java.time.Instant;
import java.util.List;
import java.util.Map;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.atomic.AtomicLong;

final class ZLinkAutoConnectReconcilerTest {
    @Test
    void bindingRejectionReportsTypedErrorAndRetainsIntentForNextReconcile() {
        MutableResolver resolver = new MutableResolver();
        resolver.rows = List.of(peer());
        RecordingExecutor executor = new RecordingExecutor();
        var rejection = new ZlinkConnectException(ConnectResult.BUSY);
        executor.connectFailure = rejection;
        var errors = new java.util.ArrayList<Throwable>();
        var reconciler =
                new ZLinkAutoConnectReconciler(
                        new ZLinkAutoConnectPlanner.Local(
                                ZLinkAutoConnectType.CLIENT_SERVER,
                                "orders",
                                ZLinkLocationRole.DEALER,
                                RoutingId.from("client"),
                                "inproc://client"),
                        resolver,
                        executor,
                        new ZLinkLocationOptions(),
                        System::nanoTime,
                        errors::add);
        reconciler.tick().toCompletableFuture().join();
        assertEquals(1, executor.connects);
        assertEquals(List.of(rejection), errors);
        assertSame(rejection, errors.get(0));
        executor.connectFailure = null;
        reconciler.tick().toCompletableFuture().join();
        assertEquals(2, executor.connects);
        reconciler.tick().toCompletableFuture().join();
        assertEquals(2, executor.connects);
        assertEquals(1, errors.size());
    }

    @Test
    void manualReplacementRejectionKeepsThePreviouslyObservedIntent() {
        MutableResolver resolver = new MutableResolver();
        var initial = peer();
        resolver.rows = List.of(initial);
        RecordingExecutor executor = new RecordingExecutor();
        executor.manual = true;
        var errors = new java.util.ArrayList<Throwable>();
        var reconciler =
                new ZLinkAutoConnectReconciler(
                        new ZLinkAutoConnectPlanner.Local(
                                ZLinkAutoConnectType.CLIENT_SERVER,
                                "orders",
                                ZLinkLocationRole.ROUTER,
                                RoutingId.from("client"),
                                "inproc://client"),
                        resolver,
                        executor,
                        new ZLinkLocationOptions(),
                        System::nanoTime,
                        errors::add);
        reconciler.tick().toCompletableFuture().join();
        resolver.rows =
                List.of(
                        new ZLinkAutoConnectPeer(
                                initial.autoConnectType(),
                                initial.meshName(),
                                initial.nodeRid(),
                                initial.role(),
                                "inproc://replacement",
                                initial.weight(),
                                initial.draining(),
                                initial.generation(),
                                initial.metadata(),
                                initial.capabilities(),
                                "replacement-owner",
                                initial.ownerLeaseGeneration(),
                                initial.updatedAt()));
        var rejection = new ZlinkConnectException(ConnectResult.BUSY);
        executor.connectFailure = rejection;
        reconciler.tick().toCompletableFuture().join();
        assertEquals(1, executor.connects);
        assertSame(rejection, errors.get(0));
        executor.connectFailure = null;
        reconciler.tick().toCompletableFuture().join();
        assertEquals(2, executor.connects);
        reconciler.tick().toCompletableFuture().join();
        assertEquals(2, executor.connects);
        assertEquals(1, errors.size());
    }

    @Test
    void pendingPreviousIntentCloseRetriesTheMeshReplacementOnTheNextReconcile() {
        MutableResolver resolver = new MutableResolver();
        resolver.rows = List.of(peer());
        var replacements = new java.util.concurrent.atomic.AtomicInteger();
        var node =
                (systems.zlink.framework.runtime.internal.backend.ZLinkInternalMeshNode)
                        java.lang.reflect.Proxy.newProxyInstance(
                                getClass().getClassLoader(),
                                new Class<?>[] {
                                    systems.zlink.framework.runtime.internal.backend
                                            .ZLinkInternalMeshNode.class
                                },
                                (proxy, method, arguments) -> {
                                    if (method.getName().equals("replacePeerConnection")) {
                                        if (replacements.incrementAndGet() == 1) {
                                            throw new systems.zlink.framework.runtime.internal
                                                    .backend.ZLinkPeerIntentClosePendingException();
                                        }
                                        return 7L;
                                    }
                                    if (method.isDefault()) {
                                        return java.lang.reflect.InvocationHandler.invokeDefault(
                                                proxy, method, arguments);
                                    }
                                    throw new UnsupportedOperationException(method.getName());
                                });
        var errors = new java.util.ArrayList<Throwable>();
        var reconciler =
                new ZLinkAutoConnectReconciler(
                        new ZLinkAutoConnectPlanner.Local(
                                ZLinkAutoConnectType.CLIENT_SERVER,
                                "orders",
                                ZLinkLocationRole.DEALER,
                                RoutingId.from("client"),
                                "inproc://client"),
                        resolver,
                        new ZLinkLocationAutoConnectHost.MeshNodeExecutor(
                                node, java.util.Set.of(), Map.of()),
                        new ZLinkLocationOptions(),
                        System::nanoTime,
                        errors::add);

        reconciler.tick().toCompletableFuture().join();
        assertEquals(1, replacements.get());
        reconciler.tick().toCompletableFuture().join();
        assertEquals(2, replacements.get());
        reconciler.tick().toCompletableFuture().join();
        assertEquals(2, replacements.get());
        assertEquals(List.of(), errors);
    }

    @Test
    void programmingFailureIsNotConvertedToARejectedAttempt() {
        MutableResolver resolver = new MutableResolver();
        resolver.rows = List.of(peer());
        RecordingExecutor executor = new RecordingExecutor();
        var failure = new IllegalStateException("executor defect");
        executor.connectFailure = failure;
        var reconciler =
                reconciler(resolver, executor, new ZLinkLocationOptions(), new AtomicLong());
        var actual =
                assertThrows(
                        java.util.concurrent.CompletionException.class,
                        () -> reconciler.tick().toCompletableFuture().join());
        assertSame(failure, actual.getCause());
    }

    @Test
    void unchangedTargetDoesNotResubmitItsConnectionIntent() {
        MutableResolver resolver = new MutableResolver();
        resolver.rows = List.of(peer());
        java.util.ArrayList<String> connectionOperations = new java.util.ArrayList<>();
        ZLinkAutoConnectExecutor executor =
                (ZLinkAutoConnectExecutor)
                        java.lang.reflect.Proxy.newProxyInstance(
                                ZLinkAutoConnectExecutor.class.getClassLoader(),
                                new Class<?>[] {ZLinkAutoConnectExecutor.class},
                                (proxy, method, arguments) -> {
                                    if (method.getName().equals("isManual")) return false;
                                    if (method.getName().equals("observeAdmissionExpectation"))
                                        return null;
                                    connectionOperations.add(method.getName());
                                    return method.getReturnType() == boolean.class ? true : null;
                                });
        var reconciler =
                new ZLinkAutoConnectReconciler(
                        new ZLinkAutoConnectPlanner.Local(
                                ZLinkAutoConnectType.CLIENT_SERVER,
                                "orders",
                                ZLinkLocationRole.DEALER,
                                RoutingId.from("client"),
                                "inproc://client"),
                        resolver,
                        executor,
                        new ZLinkLocationOptions(),
                        System::nanoTime,
                        failure -> {
                            throw failure;
                        });
        reconciler.tick().toCompletableFuture().join();
        reconciler.tick().toCompletableFuture().join();
        assertEquals(List.of("connect"), connectionOperations);
    }

    @Test
    void storeFailureRetriesOnlyThePreviouslyDesiredPendingTargetWithinGrace() {
        MutableResolver resolver = new MutableResolver();
        RecordingExecutor executor = new RecordingExecutor();
        executor.connectSucceeds = false;
        AtomicLong now = new AtomicLong();
        ZLinkLocationOptions options = new ZLinkLocationOptions();
        options.setStoreFailureGrace(Duration.ofSeconds(5));
        var reconciler = reconciler(resolver, executor, options, now);

        resolver.rows = List.of(peer());
        reconciler.tick().toCompletableFuture().join();
        assertEquals(1, executor.connects);

        resolver.failure = new IllegalStateException("store unavailable");
        executor.connectSucceeds = true;
        now.set(Duration.ofSeconds(1).toNanos());
        reconciler.tick().toCompletableFuture().join();
        assertEquals(2, executor.connects);

        executor.connectSucceeds = false;
        now.set(Duration.ofSeconds(7).toNanos());
        reconciler.tick().toCompletableFuture().join();
        assertEquals(
                2, executor.connects, "retry must stop after the configured Store failure grace");
    }

    @Test
    void recoveredSnapshotDefersMissingTargetUntilOwnerLeaseTtl() {
        MutableResolver resolver = new MutableResolver();
        RecordingExecutor executor = new RecordingExecutor();
        AtomicLong now = new AtomicLong();
        ZLinkLocationOptions options = new ZLinkLocationOptions();
        options.setOwnerLeaseRenewInterval(Duration.ofSeconds(1));
        options.setOwnerLeaseTtl(Duration.ofSeconds(3));
        var reconciler = reconciler(resolver, executor, options, now);

        resolver.rows = List.of(peer());
        reconciler.tick().toCompletableFuture().join();
        assertEquals(1, executor.connects);

        resolver.failure = new IllegalStateException("store unavailable");
        reconciler.tick().toCompletableFuture().join();
        resolver.failure = null;
        resolver.rows = List.of();
        now.set(Duration.ofMillis(100).toNanos());
        reconciler.tick().toCompletableFuture().join();
        assertEquals(0, executor.disconnects);
        now.set(Duration.ofMillis(3099).toNanos());
        reconciler.tick().toCompletableFuture().join();
        assertEquals(0, executor.disconnects);
        now.set(Duration.ofMillis(3100).toNanos());
        reconciler.tick().toCompletableFuture().join();
        assertEquals(1, executor.disconnects);
    }

    @Test
    void recoveredSnapshotConnectsNewTargetImmediately() {
        MutableResolver resolver = new MutableResolver();
        RecordingExecutor executor = new RecordingExecutor();
        AtomicLong now = new AtomicLong();
        var reconciler = reconciler(resolver, executor, new ZLinkLocationOptions(), now);

        resolver.rows = List.of(peer());
        reconciler.tick().toCompletableFuture().join();
        resolver.failure = new IllegalStateException("store unavailable");
        reconciler.tick().toCompletableFuture().join();

        resolver.failure = null;
        resolver.rows = List.of(peer("server-new"));
        reconciler.tick().toCompletableFuture().join();
        assertEquals(2, executor.connects);
        assertEquals(0, executor.disconnects);
    }

    @Test
    void recoveryPreservesUnconnectedTargetIntentUntilOwnerLeaseTtl() {
        MutableResolver resolver = new MutableResolver();
        RecordingExecutor executor = new RecordingExecutor();
        executor.connectSucceeds = false;
        AtomicLong now = new AtomicLong();
        var reconciler = reconciler(resolver, executor, new ZLinkLocationOptions(), now);

        resolver.rows = List.of(peer());
        reconciler.tick().toCompletableFuture().join();
        resolver.failure = new IllegalStateException("store unavailable");
        reconciler.tick().toCompletableFuture().join();
        int attemptsBeforeRecovery = executor.connects;
        resolver.failure = null;
        resolver.rows = List.of();
        executor.connectSucceeds = true;
        reconciler.tick().toCompletableFuture().join();

        assertEquals(attemptsBeforeRecovery + 1, executor.connects);
        assertEquals(0, executor.disconnects);
    }

    @Test
    void objectClientDescriptorIsPublishedAsNotRequiredWithoutConnecting() {
        MutableResolver resolver = new MutableResolver();
        RecordingExecutor executor = new RecordingExecutor();
        AtomicLong now = new AtomicLong();
        var reconciler =
                new ZLinkAutoConnectReconciler(
                        new ZLinkAutoConnectPlanner.Local(
                                ZLinkAutoConnectType.ROUTE_MESH,
                                "mesh",
                                ZLinkLocationRole.ROUTER,
                                RoutingId.from("client-a"),
                                "inproc://client-a",
                                ZLinkMeshNodeObjectRole.CLIENT,
                                false),
                        resolver,
                        executor,
                        new ZLinkLocationOptions(),
                        now::get,
                        failure -> {
                            throw failure;
                        });
        resolver.rows =
                List.of(
                        new ZLinkAutoConnectPeer(
                                ZLinkAutoConnectType.ROUTE_MESH,
                                "mesh",
                                RoutingId.from("client-b"),
                                ZLinkLocationRole.ROUTER,
                                "inproc://client-b",
                                100,
                                false,
                                3,
                                Map.of(),
                                List.of(),
                                "owner-b",
                                4,
                                Instant.EPOCH,
                                ZLinkMeshNodeObjectRole.CLIENT,
                                false));

        reconciler.tick().toCompletableFuture().join();
        assertEquals(0, executor.connects);
        assertEquals(1, executor.notRequiredMarks);
        assertEquals(1, executor.admissionExpectations);

        resolver.rows = List.of();
        reconciler.tick().toCompletableFuture().join();
        assertEquals(1, executor.notRequiredClears);
        assertEquals(1, executor.forgottenAdmissionExpectations);
    }

    private static ZLinkAutoConnectReconciler reconciler(
            MutableResolver resolver,
            RecordingExecutor executor,
            ZLinkLocationOptions options,
            AtomicLong now) {
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
                now::get,
                failure -> {
                    throw failure;
                });
    }

    private static ZLinkAutoConnectPeer peer() {
        return peer("server");
    }

    private static ZLinkAutoConnectPeer peer(String name) {
        return new ZLinkAutoConnectPeer(
                ZLinkAutoConnectType.CLIENT_SERVER,
                "orders",
                RoutingId.from(name),
                ZLinkLocationRole.ROUTER,
                "inproc://" + name,
                100,
                false,
                9,
                Map.of(),
                List.of(),
                "owner-" + name,
                4,
                Instant.parse("2026-07-27T00:00:00Z"));
    }

    private static final class MutableResolver implements ZLinkAutoConnectPeerResolver {
        private List<ZLinkAutoConnectPeer> rows = List.of();
        private RuntimeException failure;

        @Override
        public CompletionStage<List<ZLinkAutoConnectPeer>> listPeers(
                ZLinkAutoConnectType type, String meshName, ZLinkLocationRole role) {
            return failure == null
                    ? CompletableFuture.completedFuture(rows)
                    : CompletableFuture.failedFuture(failure);
        }
    }

    private static final class RecordingExecutor implements ZLinkAutoConnectExecutor {
        private int connects;
        private int disconnects;
        private int notRequiredMarks;
        private int notRequiredClears;
        private int admissionExpectations;
        private int forgottenAdmissionExpectations;
        private boolean connectSucceeds = true;
        private RuntimeException connectFailure;
        private boolean manual;

        @Override
        public boolean isManual(ZLinkAutoConnectPlanner.Target target) {
            return manual;
        }

        @Override
        public boolean connect(ZLinkAutoConnectPlanner.Target target) {
            connects++;
            if (connectFailure != null) throw connectFailure;
            return connectSucceeds;
        }

        @Override
        public boolean disconnect(ZLinkAutoConnectPlanner.Target target) {
            disconnects++;
            return true;
        }

        @Override
        public void markNotRequired(ZLinkAutoConnectPlanner.Target target) {
            notRequiredMarks++;
        }

        @Override
        public void clearNotRequired(ZLinkAutoConnectPlanner.Target target) {
            notRequiredClears++;
        }

        @Override
        public void observeAdmissionExpectation(ZLinkAutoConnectPlanner.Target target) {
            admissionExpectations++;
        }

        @Override
        public void forgetAdmissionExpectation(ZLinkAutoConnectPlanner.Target target) {
            forgottenAdmissionExpectations++;
        }
    }
}
