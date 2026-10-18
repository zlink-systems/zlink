package systems.zlink.framework.runtime.spots;

import static org.junit.jupiter.api.Assertions.*;

import org.junit.jupiter.api.Test;

import systems.zlink.framework.ZLinkMessageContext;
import systems.zlink.framework.actors.ZLinkActor;
import systems.zlink.framework.actors.ZLinkActorContext;
import systems.zlink.framework.actors.ZLinkActorFactory;
import systems.zlink.framework.runtime.configuration.DefaultZLinkFrameworkOptions;
import systems.zlink.framework.runtime.host.ZLinkFrameworkRuntimeTestAccess;
import systems.zlink.framework.runtime.internal.handlers.ZLinkHandlerActivator;
import systems.zlink.framework.runtime.internal.relocation.ZLinkRelocationAdapterRegistry;
import systems.zlink.framework.runtime.locations.ZLinkInMemoryLocationStore;
import systems.zlink.framework.runtime.messaging.ZLinkJsonMessageSerializer;
import systems.zlink.framework.spots.ZLinkEntrySpot;
import systems.zlink.framework.spots.ZLinkEntrySpotActorRequestHandler;
import systems.zlink.framework.spots.ZLinkEntrySpotContext;

import java.nio.charset.StandardCharsets;
import java.util.List;
import java.util.Map;
import java.util.UUID;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;

final class ZLinkStandaloneActorRelocationLateIngressTest {
    private static final String MESH = "aggregate-membership";
    private static final String ACTOR_ID = "restored-player";
    private static final long WAIT_SECONDS = 5;

    @Test
    void lateIngressIsAdmittedToPreparedActorBeforePublication() throws Exception {
        var options = new DefaultZLinkFrameworkOptions();
        options.addLocationStore(new ZLinkInMemoryLocationStore());
        var objects =
                options.addRouteMesh(MESH)
                        .listen("inproc://aggregate-membership-" + UUID.randomUUID())
                        .objects()
                        .server();
        objects.addEntrySpot(Entry.class);
        objects.addActorFactory(
                "player", Player.class, Factory.class, factory -> factory.disableRelocation());
        try (var host = ZLinkFrameworkRuntimeTestAccess.start(options)) {
            var spots = (ZLinkSpotRuntime) host.spotManager();
            var owner =
                    new ZLinkStandaloneActorRelocationStagingOwner(
                            spots.primaryNode(),
                            spots.actorSessions(),
                            new ZLinkRelocationAdapterRegistry(
                                    options.registration(), ZLinkHandlerActivator.reflection()),
                            spots,
                            MESH);
            var id = UUID.randomUUID();
            var request =
                    new ZLinkStandaloneActorRelocationStagingOwner.Request(
                            id,
                            ACTOR_ID,
                            "player",
                            7,
                            11,
                            false,
                            spots.primaryNode().entrySpot().spotId());
            byte[] root =
                    ZLinkCanonicalActorRelocationEnvelope.encode(
                            id, ACTOR_ID, 7, 11, false, new byte[0], List.of());
            var staged = owner.stage(request, root).toCompletableFuture().join();
            var backlog =
                    owner.closeDurableBacklog(
                            staged,
                            root,
                            (lane, record) -> CompletableFuture.completedFuture(null));
            var failure = new CompletableFuture<Throwable>();
            var reply = new CompletableFuture<String>();
            assertTrue(
                    owner.acceptIngress(
                            staged,
                            ZLinkAcceptedJournalTestRecords.actor(
                                    ACTOR_ID,
                                    1,
                                    Probe.class.getSimpleName(),
                                    Map.of(),
                                    "{\"value\":\"late\"}".getBytes(StandardCharsets.UTF_8)),
                            parts -> {
                                assertEquals(1, parts.size());
                                try (var message = parts.getFirst()) {
                                    reply.complete(
                                            new ZLinkJsonMessageSerializer()
                                                    .deserializeBorrowed(message, String.class));
                                }
                            },
                            failure::complete));
            var terminal = owner.drainDurableBacklog(backlog).toCompletableFuture().join();
            try {
                assertFalse(
                        failure.isDone(),
                        "accepted late ingress failed before publication: " + failure.getNow(null));
                assertFalse(
                        terminal.toCompletableFuture().isDone(),
                        "prepared FIFO must stay closed before publication");
            } finally {
                owner.publishHidden(backlog, 11);
                owner.openAdmission(staged);
                terminal.toCompletableFuture()
                        .get(WAIT_SECONDS, java.util.concurrent.TimeUnit.SECONDS);
            }
            assertEquals("late", reply.join());
            assertFalse(failure.isDone());
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
        public void configure() {
            context.handlers().addHandler(Handler.class);
        }

        @Override
        public CompletionStage<Void> onJoinedActor(Player actor) {
            return CompletableFuture.completedFuture(null);
        }

        @Override
        public CompletionStage<Void> onLeaveActor(Player actor) {
            return CompletableFuture.completedFuture(null);
        }
    }

    public record Probe(String value) {}

    public static final class Handler
            implements ZLinkEntrySpotActorRequestHandler<Entry, Player, Probe, String> {
        @Override
        public CompletionStage<String> handle(
                Entry spot, Player actor, ZLinkMessageContext context, Probe request) {
            return CompletableFuture.completedFuture(request.value());
        }
    }
}
