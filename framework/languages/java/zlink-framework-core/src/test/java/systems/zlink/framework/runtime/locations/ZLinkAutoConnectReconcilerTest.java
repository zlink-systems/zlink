package systems.zlink.framework.runtime.locations;

import static org.junit.jupiter.api.Assertions.*;

import java.time.Duration;
import java.time.Instant;
import java.util.ArrayList;
import java.util.List;
import java.util.Map;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.atomic.AtomicLong;
import org.junit.jupiter.api.Test;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.locations.ZLinkLocationOptions;
import systems.zlink.framework.locations.ZLinkLocationRole;
import systems.zlink.framework.locations.ZLinkMeshNodeObjectRole;
import systems.zlink.framework.runtime.internal.locations.*;

final class ZLinkAutoConnectReconcilerTest {
    @Test
    void storeFailureRetriesOnlyThePreviouslyDesiredPendingTargetWithinGrace() {
        MutableResolver resolver = new MutableResolver();
        RecordingExecutor executor = new RecordingExecutor();
        executor.connectSucceeds = false;
        AtomicLong now = new AtomicLong();
        ZLinkLocationOptions options = new ZLinkLocationOptions();
        options.setStoreFailureGrace(Duration.ofSeconds(5));
        var reconciler = reconciler(
            resolver, executor, options, now);

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
        assertEquals(2, executor.connects,
            "retry must stop after the configured Store failure grace");
    }

    @Test
    void firstRecoveredSnapshotKeepsExistingConnectionUntilLeaseWindowEnds() {
        MutableResolver resolver = new MutableResolver();
        RecordingExecutor executor = new RecordingExecutor();
        AtomicLong now = new AtomicLong();
        ZLinkLocationOptions options = new ZLinkLocationOptions();
        options.setOwnerLeaseRenewInterval(Duration.ofSeconds(1));
        options.setOwnerLeaseTtl(Duration.ofSeconds(3));
        var reconciler = reconciler(
            resolver, executor, options, now);

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

        now.set(Duration.ofSeconds(4).toNanos());
        reconciler.tick().toCompletableFuture().join();
        assertEquals(1, executor.disconnects);
    }

    @Test
    void objectClientDescriptorIsPublishedAsNotRequiredWithoutConnecting() {
        MutableResolver resolver = new MutableResolver();
        RecordingExecutor executor = new RecordingExecutor();
        AtomicLong now = new AtomicLong();
        var reconciler = new ZLinkAutoConnectReconciler(
            new ZLinkAutoConnectPlanner.Local(
                ZLinkAutoConnectType.ROUTE_MESH,
                "mesh",
                ZLinkLocationRole.ROUTER,
                RoutingId.from("client-a"),
                "inproc://client-a",
                ZLinkMeshNodeObjectRole.CLIENT,
                false),
            null,
            null,
            resolver,
            executor,
            new ZLinkLocationOptions(),
            now::get);
        resolver.rows = List.of(new ZLinkAutoConnectPeer(
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
            null,
            null,
            resolver,
            executor,
            options,
            now::get);
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

    private static final class MutableResolver
        implements ZLinkAutoConnectPeerResolver {
        private List<ZLinkAutoConnectPeer> rows = List.of();
        private RuntimeException failure;

        @Override
        public java.util.concurrent.CompletionStage<List<ZLinkAutoConnectPeer>>
            listPeers(
                ZLinkAutoConnectType type,
                String meshName,
                ZLinkLocationRole role) {
            return failure == null
                ? CompletableFuture.completedFuture(rows)
                : CompletableFuture.failedFuture(failure);
        }
    }

    private static final class RecordingExecutor
        implements ZLinkAutoConnectExecutor {
        private int connects;
        private int disconnects;
        private int notRequiredMarks;
        private int notRequiredClears;
        private int admissionExpectations;
        private int forgottenAdmissionExpectations;
        private boolean connectSucceeds = true;

        @Override
        public boolean connect(ZLinkAutoConnectPlanner.Target target) {
            connects++;
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
        public void observeAdmissionExpectation(
            ZLinkAutoConnectPlanner.Target target) {
            admissionExpectations++;
        }

        @Override
        public void forgetAdmissionExpectation(
            ZLinkAutoConnectPlanner.Target target) {
            forgottenAdmissionExpectations++;
        }
    }
}
