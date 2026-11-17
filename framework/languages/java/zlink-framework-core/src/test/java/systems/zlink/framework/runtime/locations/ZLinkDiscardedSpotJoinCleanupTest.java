package systems.zlink.framework.runtime.locations;

import static org.junit.jupiter.api.Assertions.assertArrayEquals;
import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertInstanceOf;
import static org.junit.jupiter.api.Assertions.assertTrue;

import org.junit.jupiter.params.ParameterizedTest;
import org.junit.jupiter.params.provider.EnumSource;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.ZLinkMessageContext;
import systems.zlink.framework.actors.ZLinkActor;
import systems.zlink.framework.actors.ZLinkActorContext;
import systems.zlink.framework.actors.ZLinkActorFactory;
import systems.zlink.framework.configuration.ZLinkUserSpotExecutionMode;
import systems.zlink.framework.locationprovider.ZLinkLocationStore;
import systems.zlink.framework.locationprovider.ZLinkStoreCancellation;
import systems.zlink.framework.locationprovider.ZLinkStoreDelete;
import systems.zlink.framework.locationprovider.ZLinkStoreKey;
import systems.zlink.framework.locationprovider.ZLinkStorePut;
import systems.zlink.framework.locationprovider.ZLinkStoreReadResult;
import systems.zlink.framework.locationprovider.ZLinkStoreScanRequest;
import systems.zlink.framework.locationprovider.ZLinkStoreScanResult;
import systems.zlink.framework.locationprovider.ZLinkStoreWriteApplied;
import systems.zlink.framework.locationprovider.ZLinkStoreWriteRequest;
import systems.zlink.framework.locationprovider.ZLinkStoreWriteResult;
import systems.zlink.framework.messaging.ZLinkMessage;
import systems.zlink.framework.runtime.binding.ZLinkJavaBackendAdapterFactory;
import systems.zlink.framework.runtime.configuration.DefaultZLinkFrameworkOptions;
import systems.zlink.framework.runtime.host.ZLinkFrameworkRuntimeTestAccess;
import systems.zlink.framework.runtime.internal.execution.ZLinkStateLane;
import systems.zlink.framework.runtime.internal.handlers.ZLinkHandlerActivator;
import systems.zlink.framework.runtime.internal.locations.ZLinkAuthorityMissing;
import systems.zlink.framework.runtime.internal.locations.ZLinkAuthoritySnapshot;
import systems.zlink.framework.runtime.internal.locations.ZLinkProviderLocationRepository;
import systems.zlink.framework.runtime.spots.ZLinkSpotRuntimeTestAccess;
import systems.zlink.framework.spots.ZLinkEntrySpot;
import systems.zlink.framework.spots.ZLinkEntrySpotActorRequestHandler;
import systems.zlink.framework.spots.ZLinkEntrySpotContext;
import systems.zlink.framework.spots.ZLinkSpot;
import systems.zlink.framework.spots.ZLinkSpotActorJoinResult;
import systems.zlink.framework.spots.ZLinkSpotContext;

import java.time.Duration;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.TimeUnit;

final class ZLinkDiscardedSpotJoinCleanupTest {
    private static final String ACTOR = "discard-join-player";
    private static final String SPOT = "discard-join-room";
    private static volatile Room room;

    @ParameterizedTest
    @EnumSource(ZLinkUserSpotExecutionMode.class)
    void discardedSpotWaitsForAcceptedJoinBeforeMembershipWrite(ZLinkUserSpotExecutionMode mode)
            throws Exception {
        exercise(false, false, mode);
    }

    @ParameterizedTest
    @EnumSource(ZLinkUserSpotExecutionMode.class)
    void discardedSpotWaitsForFailedAcceptedJoin(ZLinkUserSpotExecutionMode mode) throws Exception {
        exercise(true, false, mode);
    }

    @ParameterizedTest
    @EnumSource(ZLinkUserSpotExecutionMode.class)
    void discardedSpotWaitsForAppliedJoinResponse(ZLinkUserSpotExecutionMode mode)
            throws Exception {
        exercise(false, true, mode);
    }

    private void exercise(boolean fail, boolean applied, ZLinkUserSpotExecutionMode mode)
            throws Exception {
        var store = new HeldJoinStore(applied, fail);
        var repository =
                new ZLinkProviderLocationRepository(store, System::nanoTime, Duration.ofNanos(1));
        var scheduler = new ZLinkLocationFirstHeartbeatTest.RecordingScheduler();
        var locations =
                new ZLinkLocationRuntime(
                        ZLinkRegisteredLocationStores.fromUnified(repository),
                        "discard-join-owner",
                        Duration.ofSeconds(30),
                        Duration.ofSeconds(5),
                        Duration.ofSeconds(3),
                        Duration.ofNanos(1),
                        System::nanoTime,
                        scheduler,
                        new ZLinkStateLane(Runnable::run));
        var options = new DefaultZLinkFrameworkOptions();
        if (mode == ZLinkUserSpotExecutionMode.PER_ACTOR)
            options.addRelocationStore(
                    new systems.zlink.framework.runtime.InMemoryRelocationStore());
        options.addLocationStore(store);
        options.configureLocations().setOwnerLeaseFencingMargin(Duration.ofNanos(1));
        var objects =
                options.addRouteMesh("discard-join")
                        .listen("inproc://discard-join-" + System.nanoTime())
                        .setRoutingId(RoutingId.from("discard-join"))
                        .objects()
                        .server();
        objects.addEntrySpot(Entry.class);
        objects.addSpotFactory(
                "room",
                Room.class,
                factory -> {
                    if (mode == ZLinkUserSpotExecutionMode.PER_ACTOR)
                        factory.recreateOnRelocation();
                    else factory.disableRelocation();
                    factory.executionMode(mode);
                });
        objects.addActorFactory(
                "player", Player.class, Factory.class, factory -> factory.disableRelocation());
        try (var runtime =
                ZLinkFrameworkRuntimeTestAccess.startWithConstruction(
                        options,
                        new ZLinkJavaBackendAdapterFactory(),
                        ZLinkHandlerActivator.reflection(),
                        null,
                        new ZLinkFrameworkRuntimeTestAccess.Construction()
                                .locationRuntime(locations))) {
            await(ZLinkFrameworkRuntimeTestAccess.startupCompletion(runtime));
            await(locations.renewOwnerLeaseOnce());
            await(runtime.spotManager().getOrCreate(SPOT, "room").submit());
            await(runtime.actorManager().create(ACTOR, "player").submit());
            var originalActor =
                    assertInstanceOf(
                            ZLinkAuthoritySnapshot.class,
                            await(
                                    repository.read(
                                            ZLinkAuthorityKeyCodec.actor(ACTOR), () -> false)));
            store.armed = true;
            store.countJoins = true;
            assertEquals(
                    "scheduled",
                    await(
                            runtime.actorClient()
                                    .requestToActor(ACTOR, new Join())
                                    .submit(String.class)));
            await(store.entered);
            ZLinkSpotRuntimeTestAccess.discardExecution(room);
            // The Location lane executes synchronously and the Store completes renew immediately.
            // Returning from this call has submitted the discarded execution cleanup operation.
            var cleanup = locations.renewOwnerLeaseOnce().toCompletableFuture();
            try {
                assertInstanceOf(
                        ZLinkAuthoritySnapshot.class,
                        await(repository.read(ZLinkAuthorityKeyCodec.spot(SPOT), () -> false)),
                        "authority must remain until the accepted Join settles");
                assertFalse(cleanup.isDone(), "renew must retain the pending local cleanup stage");
            } finally {
                if (fail)
                    store.release.completeExceptionally(
                            new IllegalStateException("Join not applied"));
                else store.release.complete(null);
            }
            await(cleanup);
            await(locations.renewOwnerLeaseOnce());
            assertEquals(
                    1,
                    store.joinSubmissions,
                    "discard must not resubmit the accepted membership write");
            if (!fail) {
                assertInstanceOf(
                        ZLinkAuthorityMissing.class,
                        await(repository.read(ZLinkAuthorityKeyCodec.actor(ACTOR), () -> false)),
                        "the committed local member is cleaned before the Spot");
            } else {
                var actor =
                        assertInstanceOf(
                                ZLinkAuthoritySnapshot.class,
                                await(
                                        repository.read(
                                                ZLinkAuthorityKeyCodec.actor(ACTOR), () -> false)));
                assertArrayEquals(
                        originalActor.payload(),
                        actor.payload(),
                        "a failed Join leaves the original Entry membership intact");
            }
            assertInstanceOf(
                    ZLinkAuthorityMissing.class,
                    await(repository.read(ZLinkAuthorityKeyCodec.spot(SPOT), () -> false)));
            assertEquals(fail ? 0 : 1, store.memberDeletes);
            assertTrue(
                    store.spotDeleteAfterMember,
                    "Spot deletion must follow the committed local member deletion");
        } finally {
            store.release.complete(null);
        }
    }

    private static <T> T await(CompletionStage<T> stage) throws Exception {
        return stage.toCompletableFuture().get(5, TimeUnit.SECONDS);
    }

    private static final class HeldJoinStore implements ZLinkLocationStore {
        final ZLinkLocationStore inner = new ZLinkInMemoryLocationStore();
        final CompletableFuture<Void> entered = new CompletableFuture<>();
        final CompletableFuture<Void> release = new CompletableFuture<>();
        final boolean applied;
        final boolean fail;
        volatile boolean armed;
        volatile int memberDeletes;
        volatile int joinSubmissions;
        volatile boolean countJoins;
        volatile boolean spotDeleteAfterMember;

        HeldJoinStore(boolean applied, boolean fail) {
            this.applied = applied;
            this.fail = fail;
        }

        public CompletionStage<ZLinkStoreReadResult> read(
                ZLinkStoreKey key, ZLinkStoreCancellation cancellation) {
            return inner.read(key, cancellation);
        }

        public CompletionStage<ZLinkStoreScanResult> scan(
                ZLinkStoreScanRequest request, ZLinkStoreCancellation cancellation) {
            return inner.scan(request, cancellation);
        }

        public CompletionStage<ZLinkStoreWriteResult> write(
                ZLinkStoreWriteRequest request, ZLinkStoreCancellation cancellation) {
            boolean actorPut =
                    request.mutations().stream()
                            .anyMatch(
                                    m ->
                                            m instanceof ZLinkStorePut put
                                                    && put.key()
                                                            .value()
                                                            .equals("authority\0actor\0" + ACTOR));
            if (countJoins && actorPut) joinSubmissions++;
            if (armed && actorPut) {
                armed = false;
                CompletionStage<ZLinkStoreWriteResult> result =
                        applied
                                ? inner.write(request, cancellation)
                                : release.thenCompose(
                                        ignored -> inner.write(request, cancellation));
                entered.complete(null);
                return applied
                        ? result.thenCompose(value -> release.thenApply(ignored -> value))
                        : result;
            }
            return inner.write(request, cancellation)
                    .thenApply(
                            result -> {
                                if (result instanceof ZLinkStoreWriteApplied) {
                                    for (var mutation : request.mutations()) {
                                        if (mutation instanceof ZLinkStoreDelete deletion) {
                                            if (deletion.key()
                                                    .value()
                                                    .equals("authority\0actor\0" + ACTOR))
                                                memberDeletes++;
                                            if (deletion.key()
                                                    .value()
                                                    .equals("authority\0spot\0" + SPOT))
                                                spotDeleteAfterMember = memberDeletes > 0 || fail;
                                        }
                                    }
                                }
                                return result;
                            });
        }
    }

    public record Join() {}

    public static final class Player implements ZLinkActor {
        private final ZLinkActorContext context;

        public Player(ZLinkActorContext context) {
            this.context = context;
        }

        public ZLinkActorContext context() {
            return context;
        }
    }

    public static final class Factory implements ZLinkActorFactory {
        public CompletionStage<ZLinkActor> create(ZLinkActorContext context) {
            return CompletableFuture.completedFuture(new Player(context));
        }
    }

    public static final class Entry implements ZLinkEntrySpot<Player> {
        private final ZLinkEntrySpotContext context;

        public Entry(ZLinkEntrySpotContext context) {
            this.context = context;
        }

        public ZLinkEntrySpotContext context() {
            return context;
        }

        public void configure() {
            context.handlers().addHandler(JoinHandler.class);
        }

        public CompletionStage<Void> onJoinedActor(Player actor) {
            return CompletableFuture.completedFuture(null);
        }

        public CompletionStage<Void> onLeaveActor(Player actor) {
            return CompletableFuture.completedFuture(null);
        }
    }

    public static final class JoinHandler
            implements ZLinkEntrySpotActorRequestHandler<Entry, Player, Join, String> {
        public CompletionStage<String> handle(
                Entry spot, Player actor, ZLinkMessageContext context, Join request) {
            actor.context().joinSpot(SPOT).defer();
            return CompletableFuture.completedFuture("scheduled");
        }
    }

    public static final class Room implements ZLinkSpot<Player> {
        private final ZLinkSpotContext context;

        public Room(ZLinkSpotContext context) {
            this.context = context;
            room = this;
        }

        public ZLinkSpotContext context() {
            return context;
        }

        public CompletionStage<ZLinkSpotActorJoinResult> onActorJoin(
                String actorId, ZLinkMessage request) {
            return CompletableFuture.completedFuture(ZLinkSpotActorJoinResult.accept());
        }

        public CompletionStage<Void> onJoinedActor(Player actor) {
            return CompletableFuture.completedFuture(null);
        }

        public CompletionStage<Void> onLeaveActor(Player actor) {
            return CompletableFuture.completedFuture(null);
        }
    }
}
