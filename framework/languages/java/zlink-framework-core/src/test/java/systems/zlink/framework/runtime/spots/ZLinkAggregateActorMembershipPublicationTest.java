package systems.zlink.framework.runtime.spots;

import static org.junit.jupiter.api.Assertions.*;

import org.junit.jupiter.api.Test;

import systems.zlink.framework.actors.ZLinkActor;
import systems.zlink.framework.actors.ZLinkActorContext;
import systems.zlink.framework.actors.ZLinkActorFactory;
import systems.zlink.framework.runtime.configuration.DefaultZLinkFrameworkOptions;
import systems.zlink.framework.runtime.host.ZLinkFrameworkRuntimeTestAccess;
import systems.zlink.framework.runtime.internal.handlers.ZLinkHandlerActivator;
import systems.zlink.framework.runtime.internal.relocation.ZLinkRelocationAdapterRegistry;
import systems.zlink.framework.runtime.locations.ZLinkInMemoryLocationStore;
import systems.zlink.framework.spots.ZLinkEntrySpot;
import systems.zlink.framework.spots.ZLinkEntrySpotContext;
import systems.zlink.framework.spots.ZLinkSpot;
import systems.zlink.framework.spots.ZLinkSpotContext;

import java.util.List;
import java.util.Map;
import java.util.Optional;
import java.util.UUID;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;

final class ZLinkAggregateActorMembershipPublicationTest {
    private static final String MESH = "aggregate-membership";
    private static final String ACTOR_ID = "restored-player";
    private static final String ROOM_ID = "restored-room";
    private static final long TARGET_OWNER_GENERATION = 11;

    @Test
    void aggregatePublicationInstallsTheSameNativeAndFrameworkMembership() throws Exception {
        var options = new DefaultZLinkFrameworkOptions();
        options.addLocationStore(new ZLinkInMemoryLocationStore());
        var objects =
                options.addRouteMesh(MESH)
                        .listen("inproc://aggregate-membership-" + UUID.randomUUID())
                        .objects()
                        .server();
        objects.addEntrySpot(Entry.class);
        objects.addSpotFactory("room", Room.class, factory -> factory.disableRelocation());
        objects.addActorFactory(
                "player", Player.class, Factory.class, factory -> factory.disableRelocation());
        try (var host = ZLinkFrameworkRuntimeTestAccess.start(options)) {
            var spots = (ZLinkSpotRuntime) host.spotManager();
            var owner =
                    new ZLinkUserSpotAggregateStagingOwner(
                            spots,
                            new ZLinkRelocationAdapterRegistry(
                                    options.registration(), ZLinkHandlerActivator.reflection()),
                            MESH);
            var request =
                    new ZLinkUserSpotAggregateStagingOwner.Request(
                            Room.class,
                            "room",
                            ROOM_ID,
                            1,
                            new byte[0],
                            false,
                            ZLinkSpotTimerRelocationEnvelope.encodeCanonical(List.of()),
                            List.of(
                                    new ZLinkUserSpotAggregateStagingOwner.ActorParticipant(
                                            ACTOR_ID, "player", new byte[0], false, null)),
                            Map.of());
            var staged = owner.stage(request, () -> false).toCompletableFuture().join();
            var backlog =
                    owner.closeDurableBacklog(
                                    staged,
                                    request,
                                    (lane, record) -> CompletableFuture.completedFuture(null))
                            .toCompletableFuture()
                            .join();
            owner.publishHidden(backlog, Map.of(ACTOR_ID, TARGET_OWNER_GENERATION))
                    .toCompletableFuture()
                    .join();
            owner.openAdmission(staged);
            owner.drainDurableBacklog(backlog).toCompletableFuture().join();

            var actors = spots.actorSessions().runtime();
            var actor = actors.localActor(ACTOR_ID).orElseThrow();
            var nativeActors = spots.primaryNode().getClass().getDeclaredField("actors");
            nativeActors.setAccessible(true);
            var nativeStay = ((Map<?, ?>) nativeActors.get(spots.primaryNode())).get(ACTOR_ID);
            assertNotNull(nativeStay);
            var nativeSpotId = nativeStay.getClass().getDeclaredMethod("spotId");
            nativeSpotId.setAccessible(true);
            assertEquals(ROOM_ID, nativeSpotId.invoke(nativeStay));
            assertSame(
                    spots.spotFor(ROOM_ID),
                    actors.currentSpot(actor),
                    "Published native Actor stay and Framework membership must name the same Spot");
            assertEquals(Optional.of(ROOM_ID), actors.spotId(actor));
            assertSame(spots.spotFor(ROOM_ID), spots.localActorSpotSurface(actor));
        }
    }

    public record Player(ZLinkActorContext context) implements ZLinkActor {}

    public static final class Factory implements ZLinkActorFactory {
        @Override
        public CompletionStage<ZLinkActor> create(ZLinkActorContext context) {
            return CompletableFuture.completedFuture(new Player(context));
        }
    }

    public record Entry(ZLinkEntrySpotContext context) implements ZLinkEntrySpot<Player> {
        @Override
        public CompletionStage<Void> onJoinedActor(Player actor) {
            return CompletableFuture.completedFuture(null);
        }

        @Override
        public CompletionStage<Void> onLeaveActor(Player actor) {
            return CompletableFuture.completedFuture(null);
        }
    }

    public record Room(ZLinkSpotContext context) implements ZLinkSpot<Player> {
        @Override
        public CompletionStage<Void> onJoinedActor(Player actor) {
            return CompletableFuture.completedFuture(null);
        }

        @Override
        public CompletionStage<Void> onLeaveActor(Player actor) {
            return CompletableFuture.completedFuture(null);
        }
    }
}
