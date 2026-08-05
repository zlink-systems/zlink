package systems.zlink.framework.runtime.spots;

import static org.junit.jupiter.api.Assertions.*;

import java.time.Instant;
import java.util.ArrayList;
import java.util.List;
import java.util.Map;
import java.util.Optional;
import java.util.UUID;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import org.junit.jupiter.api.Test;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.actors.ZLinkActor;
import systems.zlink.framework.execution.ZLinkAsyncSerialQueue;
import systems.zlink.framework.locations.*;
import systems.zlink.framework.runtime.internal.locations.*;
import systems.zlink.framework.runtime.internal.locations
    .ZLinkAggregateRelocationCoordinator;
import systems.zlink.framework.runtime.internal.locations
    .ZLinkRelocationStored;
import systems.zlink.framework.spots.ZLinkSpot;
import systems.zlink.framework.spots.ZLinkSpotContext;

final class ZLinkUserSpotRetireSchedulerTest {
    private static final ZLinkStoreCancellation NEVER = () -> false;

    @Test
    void admissionWaitsForCompletedCasAndEveryBindingRouteAck() {
        List<String> order = new ArrayList<>();
        CompletableFuture<ZLinkAggregateRelocationCoordinator.Published>
            completedCas = new CompletableFuture<>();
        CompletableFuture<Void> routeAck = new CompletableFuture<>();
        FakeBackend backend = new FakeBackend(order, completedCas);
        var scheduler = new ZLinkUserSpotRetireScheduler(backend);
        var admissionOpened = new boolean[1];

        CompletionStage<ZLinkUserSpotRetireScheduler.Result> operation =
            scheduler.execute(new ZLinkUserSpotRetireScheduler.Request(
                prepared(),
                staged(),
                (lane, record) -> {
                    order.add("journal");
                    return CompletableFuture.completedFuture(null);
                },
                () -> {
                    order.add("source-cleanup");
                    return CompletableFuture.completedFuture(null);
                },
                new byte[] {4},
                List.of(() -> {
                    order.add("route-44");
                    return routeAck.thenRun(() -> order.add("route-45"));
                }),
                () -> {
                    order.add("steady");
                    return CompletableFuture.completedFuture(null);
                },
                () -> {
                    admissionOpened[0] = true;
                    order.add("admission");
                },
                () -> CompletableFuture.completedFuture(null)), NEVER);

        assertEquals(List.of(
            "commit", "publish-replay", "source-cleanup", "completed-cas"),
            order);
        assertFalse(admissionOpened[0]);

        completedCas.complete(published(8));
        assertEquals("route-44", order.get(order.size() - 1));
        assertFalse(admissionOpened[0],
            "command 45 ACK must precede target admission");

        routeAck.complete(null);
        operation.toCompletableFuture().join();
        assertEquals(List.of(
            "commit", "publish-replay", "source-cleanup", "completed-cas",
            "route-44", "route-45", "steady", "admission"), order);
        assertTrue(admissionOpened[0]);
    }

    @Test
    void precommitFailureRunsDurableTargetSourceCleanupInReverseOrder() {
        List<String> order = new ArrayList<>();
        FakeBackend backend = new FakeBackend(order, new CompletableFuture<>());
        backend.commitFailure = new IllegalStateException("commit failed");
        var scheduler = new ZLinkUserSpotRetireScheduler(backend);

        var failure = assertThrows(
            java.util.concurrent.CompletionException.class,
            () -> scheduler.execute(new ZLinkUserSpotRetireScheduler.Request(
                    prepared(),
                    staged(),
                    (lane, record) -> CompletableFuture.completedFuture(null),
                    () -> CompletableFuture.completedFuture(null),
                    new byte[] {4},
                    List.of(),
                    () -> CompletableFuture.completedFuture(null),
                    () -> fail("admission must remain closed"),
                    () -> {
                        order.add("source-resume");
                        return CompletableFuture.completedFuture(null);
                    }), NEVER)
                .toCompletableFuture().join());

        assertSame(backend.commitFailure, failure.getCause());
        assertEquals(List.of(
            "commit", "aggregate-abort", "target-discard", "source-resume"),
            order);
    }

    private static ZLinkAggregateRelocationCoordinator.Prepared prepared() {
        var request = request();
        return new ZLinkAggregateRelocationCoordinator.Prepared(
            new ZLinkAggregateFence(request.aggregateId(), 7),
            new ZLinkRelocationStored(
                "root-a", 3, Instant.now().plusSeconds(60), Instant.now()),
            request,
            new byte[32]);
    }

    private static ZLinkAggregateRelocationCoordinator.Published published(
        long generation) {
        var request = request(generation);
        return new ZLinkAggregateRelocationCoordinator.Published(
            new ZLinkAggregateFence(request.aggregateId(), generation),
            new ZLinkRelocationStored(
                "root-" + generation,
                generation,
                Instant.now().plusSeconds(60),
                Instant.now()),
            request,
            new byte[32],
            Map.of("spot:room-a", 6L));
    }

    private static ZLinkAggregateRelocationCoordinator.Request request() {
        return request(7);
    }

    private static ZLinkAggregateRelocationCoordinator.Request request(
        long generation) {
        return new ZLinkAggregateRelocationCoordinator.Request(
            UUID.fromString("00112233-4455-6677-8899-aabbccddeeff"),
            generation,
            List.of(new ZLinkAggregateRelocationCoordinator.Participant(
                "spot:room-a",
                ZLinkPlacementObjectKind.USER_SPOT,
                3,
                5,
                "version-1",
                ZLinkAuthorityGenerationTransition.NEW_OWNER,
                new byte[] {1},
                new byte[] {2})),
            new byte[] {9},
            new ZLinkMeshNodeDescriptorKey("game", RoutingId.from("node-b")),
            4,
            ZLinkPlacementCapacityBundle.spot(
                ZLinkPlacementObjectKind.USER_SPOT,
                "RoomSpot",
                1),
            new ZLinkLocationOwnerToken("owner-b", 12));
    }

    private static ZLinkUserSpotAggregateStagingOwner.Staged staged() {
        var owner = new ZLinkUserSpotAggregateStagingOwner(new StageBackend());
        return owner.stage(
                new ZLinkUserSpotAggregateStagingOwner.Request(
                    TestSpot.class,
                    "RoomSpot",
                    "room-a",
                    3,
                    new byte[0],
                    false,
                    new byte[0],
                    List.of(),
                    Map.<String, List<ZLinkAsyncSerialQueue.QueuedRecord>>of()),
                () -> false)
            .toCompletableFuture().join();
    }

    private static final class FakeBackend
        implements ZLinkUserSpotRetireScheduler.Backend {
        private final List<String> order;
        private final CompletableFuture<
            ZLinkAggregateRelocationCoordinator.Published> completedCas;
        private RuntimeException commitFailure;

        private FakeBackend(
            List<String> order,
            CompletableFuture<ZLinkAggregateRelocationCoordinator.Published>
                completedCas) {
            this.order = order;
            this.completedCas = completedCas;
        }

        @Override
        public CompletionStage<ZLinkAggregateRelocationCoordinator.Published>
            commit(
                ZLinkAggregateRelocationCoordinator.Prepared prepared,
                ZLinkStoreCancellation cancellation) {
            order.add("commit");
            return commitFailure == null
                ? CompletableFuture.completedFuture(published(7))
                : CompletableFuture.failedFuture(commitFailure);
        }

        @Override
        public CompletionStage<Void> publishAndReplay(
            ZLinkUserSpotAggregateStagingOwner.Staged staged,
            ZLinkUserSpotAggregateStagingOwner.JournalReplayer replayer) {
            order.add("publish-replay");
            return CompletableFuture.completedFuture(null);
        }

        @Override
        public CompletionStage<ZLinkAggregateRelocationCoordinator.Published>
            completeSourceCleanup(
                ZLinkAggregateRelocationCoordinator.Published published,
                byte[] completedRoot,
                ZLinkStoreCancellation cancellation) {
            order.add("completed-cas");
            return completedCas;
        }

        @Override
        public CompletionStage<Void> abort(
            ZLinkAggregateRelocationCoordinator.Prepared prepared) {
            order.add("aggregate-abort");
            return CompletableFuture.completedFuture(null);
        }

        @Override
        public CompletionStage<Void> discard(
            ZLinkUserSpotAggregateStagingOwner.Staged staged) {
            order.add("target-discard");
            return CompletableFuture.completedFuture(null);
        }
    }

    private static final class StageBackend
        implements ZLinkUserSpotAggregateStagingOwner.StagingBackend {
        @Override public CompletionStage<Object> prepareSpot(
            ZLinkUserSpotAggregateStagingOwner.Request request) {
            return CompletableFuture.completedFuture(new Object());
        }
        @Override public CompletionStage<Void> restoreSpot(
            Object spot, ZLinkUserSpotAggregateStagingOwner.Request request,
            systems.zlink.framework.actors.ZLinkRelocationCancellation token) {
            return CompletableFuture.completedFuture(null);
        }
        @Override public CompletionStage<Object> prepareActor(
            ZLinkUserSpotAggregateStagingOwner.ActorParticipant participant,
            systems.zlink.framework.actors.ZLinkRelocationCancellation token) {
            return CompletableFuture.completedFuture(new Object());
        }
        @Override public void publishSpot(Object spot) { }
        @Override public void publishActor(Object actor) { }
        @Override public void completeActor(Object actor) { }
        @Override public void publishTimers(Object spot) { }
        @Override public CompletionStage<Void> discardActor(Object actor) {
            return CompletableFuture.completedFuture(null);
        }
        @Override public void discardSpot(Object spot) { }
    }

    private static final class TestSpot implements ZLinkSpot<ZLinkActor> {
        @Override public ZLinkSpotContext context() { return null; }
        @Override public CompletionStage<Void> onJoinedActor(ZLinkActor actor) {
            return CompletableFuture.completedFuture(null);
        }
        @Override public CompletionStage<Void> onLeaveActor(ZLinkActor actor) {
            return CompletableFuture.completedFuture(null);
        }
    }
}
