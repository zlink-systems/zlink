package systems.zlink.framework.runtime.actors;

import static org.junit.jupiter.api.Assertions.*;

import org.junit.jupiter.api.Test;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.ZLinkMessageContext;
import systems.zlink.framework.actors.*;
import systems.zlink.framework.errors.ZLinkFrameworkErrorKind;
import systems.zlink.framework.locationprovider.*;
import systems.zlink.framework.messaging.ZLinkMessage;
import systems.zlink.framework.runtime.configuration.DefaultZLinkFrameworkOptions;
import systems.zlink.framework.runtime.host.ZLinkFrameworkRuntime;
import systems.zlink.framework.runtime.host.ZLinkFrameworkRuntimeTestAccess;
import systems.zlink.framework.runtime.internal.locations.ZLinkAuthoritySnapshot;
import systems.zlink.framework.runtime.internal.locations.ZLinkProviderLocationRepository;
import systems.zlink.framework.runtime.locations.ZLinkActorAuthorityPayloadCodec;
import systems.zlink.framework.runtime.locations.ZLinkAuthorityKeyCodec;
import systems.zlink.framework.runtime.locations.ZLinkInMemoryLocationStore;
import systems.zlink.framework.spots.*;

import java.time.Duration;
import java.util.List;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.CopyOnWriteArrayList;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.concurrent.atomic.AtomicInteger;

final class ZLinkSameNodeJoinCommitTest {
    private static final String ACTOR = "commit-player";
    private static final String TARGET = "commit-room";
    private static final List<String> EVENTS = new CopyOnWriteArrayList<>();
    private static final AtomicBoolean FAIL_STORE = new AtomicBoolean();
    private static volatile CompletableFuture<ZLinkActorJoinCompletion> completion;
    private static volatile CompletableFuture<Void> sourceLeave;
    private static volatile CompletableFuture<Void> sourceEntered;

    @Test
    void publicJoinCommitsBeforeLifecycleAndPreservesEntryNoOpRejectionAndStoreFailure()
            throws Exception {
        EVENTS.clear();
        FAIL_STORE.set(false);
        sourceLeave = CompletableFuture.completedFuture(null);
        sourceEntered = new CompletableFuture<>();
        AtomicInteger records = new AtomicInteger();
        DefaultZLinkFrameworkOptions options = new DefaultZLinkFrameworkOptions();
        RecordingStore store = new RecordingStore();
        options.addLocationStore(store);
        options.configureDispatch()
                .messageFlow(systems.zlink.framework.configuration.ZLinkMessageFlowLogMode.ERRORS);
        var node = options.addRouteMesh("game");
        node.listen("inproc://join-commit-" + System.nanoTime())
                .setRoutingId(RoutingId.from("join-commit"));
        var objects = node.objects().server();
        objects.addEntrySpot(Entry.class);
        objects.addSpotFactory("room", Room.class, factory -> factory.disableRelocation());
        objects.addActorFactory(
                "player", Player.class, Factory.class, factory -> factory.disableRelocation());
        try (ZLinkFrameworkRuntime runtime =
                ZLinkFrameworkRuntimeTestAccess.start(
                        options, ZLinkSameNodeJoinBarrierTest.observeMeshJoins(records))) {
            runtime.spotManager()
                    .getOrCreate(TARGET, "room")
                    .submit()
                    .toCompletableFuture()
                    .get(3, TimeUnit.SECONDS);
            var actor =
                    assertInstanceOf(
                            ZLinkActorCreateResult.Created.class,
                            runtime.actorManager()
                                    .create(ACTOR, "player")
                                    .submit()
                                    .toCompletableFuture()
                                    .get(3, TimeUnit.SECONDS));
            var originalEntry = authority(store);

            sourceLeave = new CompletableFuture<>();
            var joining =
                    beginJoin(runtime, actor.actor().actorId(), new Join(TARGET, false, false));
            sourceEntered.get(3, TimeUnit.SECONDS);
            assertInstanceOf(
                    ZLinkActorJoinCompletion.Accepted.class, joining.get(3, TimeUnit.SECONDS));
            assertFalse(sourceLeave.isDone(), "source OnLeaveActor completion is one-way");
            assertEquals(
                    "target-ready",
                    runtime.route()
                            .requestToSpot(TARGET, new TargetProbe())
                            .submit(String.class)
                            .toCompletableFuture()
                            .get(3, TimeUnit.SECONDS),
                    "target gate is free while source OnLeaveActor remains pending");
            sourceLeave.complete(null);
            assertInstanceOf(
                    ZLinkActorJoinCompletion.Accepted.class, joining.get(3, TimeUnit.SECONDS));
            assertOrdered("admission", "store", "room-joined", "completion");
            assertEquals("completion", EVENTS.getLast());
            var userAuthority = authority(store);
            assertEquals(TARGET, userAuthority.currentSpotId());
            assertEquals(ZLinkSpotKind.USER.value(), userAuthority.currentSpotKind());

            assertInstanceOf(
                    ZLinkActorJoinCompletion.Accepted.class,
                    join(runtime, ACTOR, new Join(TARGET, false, false)));
            assertEquals(List.of("completion"), EVENTS);

            var entryFailed =
                    assertInstanceOf(
                            ZLinkActorJoinCompletion.Failed.class,
                            join(runtime, ACTOR, new Join(null, false, true)));
            assertEquals(ZLinkFrameworkErrorKind.INTERNAL_FAILURE, entryFailed.kind());
            assertEquals(List.of("store-failed", "completion"), EVENTS);
            assertEquals(
                    userAuthority,
                    authority(store),
                    "failed Entry commit preserves the durable membership");

            assertInstanceOf(
                    ZLinkActorJoinCompletion.Accepted.class,
                    join(runtime, ACTOR, new Join(null, false, false)));
            assertOrdered("store", "entry-joined", "completion");
            assertEquals("completion", EVENTS.getLast());
            assertFalse(EVENTS.contains("admission"));
            assertEquals(
                    originalEntry,
                    authority(store),
                    "Entry return restores actual Entry id/generation/kind and preserves Actor authority");

            assertInstanceOf(
                    ZLinkActorJoinCompletion.Accepted.class,
                    join(runtime, ACTOR, new Join(null, false, false)));
            assertEquals(List.of("completion"), EVENTS);

            assertInstanceOf(
                    ZLinkActorJoinCompletion.Rejected.class,
                    join(runtime, ACTOR, new Join(TARGET, true, false)));
            assertEquals(List.of("admission", "completion"), EVENTS);

            var failed =
                    assertInstanceOf(
                            ZLinkActorJoinCompletion.Failed.class,
                            join(runtime, ACTOR, new Join(TARGET, false, true)));
            assertEquals(ZLinkFrameworkErrorKind.INTERNAL_FAILURE, failed.kind());
            assertEquals(List.of("admission", "store-failed", "completion"), EVENTS);

            var spotsField = runtime.getClass().getDeclaredField("spots");
            spotsField.setAccessible(true);
            var spots = spotsField.get(runtime);
            var errorsField = spots.getClass().getDeclaredField("dispatchErrors");
            errorsField.setAccessible(true);
            var errors =
                    (systems.zlink.framework.runtime.diagnostics.ZLinkDispatchErrorReporter)
                            errorsField.get(spots);
            long reportsBefore = errors.reportedCount();
            sourceLeave = new CompletableFuture<>();
            sourceEntered = new CompletableFuture<>();
            assertInstanceOf(
                    ZLinkActorJoinCompletion.Accepted.class,
                    join(runtime, ACTOR, new Join(TARGET, false, false)));
            sourceEntered.get(3, TimeUnit.SECONDS);
            sourceLeave.completeExceptionally(new IllegalStateException("source lifecycle failed"));
            long deadline = System.nanoTime() + TimeUnit.SECONDS.toNanos(3);
            while (errors.reportedCount() == reportsBefore && System.nanoTime() < deadline) {
                Thread.sleep(1);
            }
            assertEquals(
                    reportsBefore + 1,
                    errors.reportedCount(),
                    "source error is reported exactly once");
            assertInstanceOf(
                    ZLinkActorJoinCompletion.Accepted.class, completion.get(3, TimeUnit.SECONDS));
            assertOrdered("admission", "store", "room-joined", "completion");
            assertEquals(0, records.get(), "all same-node Join outcomes avoid Mesh Join records");
        }
    }

    private static ZLinkActorJoinCompletion join(
            ZLinkFrameworkRuntime runtime, String actorId, Join request) throws Exception {
        return beginJoin(runtime, actorId, request).get(3, TimeUnit.SECONDS);
    }

    private static CompletableFuture<ZLinkActorJoinCompletion> beginJoin(
            ZLinkFrameworkRuntime runtime, String actorId, Join request) throws Exception {
        EVENTS.clear();
        completion = new CompletableFuture<>();
        assertEquals(
                "scheduled",
                runtime.actorClient()
                        .requestToActor(actorId, request)
                        .timeout(Duration.ofSeconds(3))
                        .submit(String.class)
                        .toCompletableFuture()
                        .get(3, TimeUnit.SECONDS));
        return completion;
    }

    private static void assertOrdered(String... expected) {
        int previous = -1;
        for (String event : expected) {
            assertTrue(EVENTS.contains(event), "missing " + event + " in " + EVENTS);
            int index = EVENTS.indexOf(event);
            assertTrue(index > previous, EVENTS.toString());
            previous = index;
        }
    }

    private static ZLinkActorAuthorityPayloadCodec.ActorAuthority authority(RecordingStore store) {
        var snapshot =
                assertInstanceOf(
                        ZLinkAuthoritySnapshot.class,
                        new ZLinkProviderLocationRepository(store)
                                .read(ZLinkAuthorityKeyCodec.actor(ACTOR), () -> false)
                                .toCompletableFuture()
                                .join());
        return new ZLinkActorAuthorityPayloadCodec().decode(snapshot.payload()).orElseThrow();
    }

    private static final class RecordingStore implements ZLinkLocationStore {
        private final ZLinkLocationStore inner = new ZLinkInMemoryLocationStore();

        @Override
        public CompletionStage<ZLinkStoreReadResult> read(
                ZLinkStoreKey key, ZLinkStoreCancellation cancellation) {
            return inner.read(key, cancellation);
        }

        @Override
        public CompletionStage<ZLinkStoreWriteResult> write(
                ZLinkStoreWriteRequest request, ZLinkStoreCancellation cancellation) {
            boolean actor =
                    request.mutations().stream()
                            .anyMatch(
                                    mutation ->
                                            mutation instanceof ZLinkStorePut put
                                                    && put.key().value().contains(ACTOR));
            if (actor && FAIL_STORE.compareAndSet(true, false)) {
                EVENTS.add("store-failed");
                return CompletableFuture.failedFuture(
                        new IllegalStateException("Join Store commit failed"));
            }
            return inner.write(request, cancellation)
                    .thenApply(
                            result -> {
                                if (actor) {
                                    EVENTS.add("store");
                                }
                                return result;
                            });
        }

        @Override
        public CompletionStage<ZLinkStoreScanResult> scan(
                ZLinkStoreScanRequest request, ZLinkStoreCancellation cancellation) {
            return inner.scan(request, cancellation);
        }
    }

    public record Join(String spotId, boolean reject, boolean failStore) {}

    public record Admission(boolean reject) {}

    public static final class Player implements ZLinkActor {
        private final ZLinkActorContext context;

        public Player(ZLinkActorContext context) {
            this.context = context;
        }

        @Override
        public ZLinkActorContext context() {
            return context;
        }

        @Override
        public CompletionStage<Void> onJoinCompleted(ZLinkActorJoinCompletion result) {
            EVENTS.add("completion");
            completion.complete(result);
            return CompletableFuture.completedFuture(null);
        }
    }

    public static final class Factory implements ZLinkActorFactory {
        @Override
        public CompletionStage<ZLinkActor> create(ZLinkActorContext context) {
            return CompletableFuture.completedFuture(new Player(context));
        }
    }

    public static final class Entry implements ZLinkEntrySpot<Player> {
        private final ZLinkEntrySpotContext context;

        public Entry(ZLinkEntrySpotContext context) {
            this.context = context;
        }

        @Override
        public ZLinkEntrySpotContext context() {
            return context;
        }

        @Override
        public void configure() {
            context.handlers().addHandler(EntryJoin.class);
        }

        @Override
        public CompletionStage<Void> onJoinedActor(Player actor) {
            EVENTS.add("entry-joined");
            return CompletableFuture.completedFuture(null);
        }

        @Override
        public CompletionStage<Void> onLeaveActor(Player actor) {
            sourceEntered.complete(null);
            return sourceLeave;
        }
    }

    public static final class Room implements ZLinkSpot<Player> {
        private final ZLinkSpotContext context;

        public Room(ZLinkSpotContext context) {
            this.context = context;
        }

        @Override
        public ZLinkSpotContext context() {
            return context;
        }

        @Override
        public void configure() {
            context.handlers().addHandler(RoomJoin.class);
            context.handlers().addHandler(TargetProbeHandler.class);
        }

        @Override
        public CompletionStage<ZLinkSpotActorJoinResult> onActorJoin(
                String actorId, ZLinkMessage request) {
            EVENTS.add("admission");
            return CompletableFuture.completedFuture(
                    request.decode(Admission.class).reject()
                            ? ZLinkSpotActorJoinResult.reject()
                            : ZLinkSpotActorJoinResult.accept());
        }

        @Override
        public CompletionStage<Void> onJoinedActor(Player actor) {
            EVENTS.add("room-joined");
            return CompletableFuture.completedFuture(null);
        }

        @Override
        public CompletionStage<Void> onLeaveActor(Player actor) {
            return CompletableFuture.completedFuture(null);
        }
    }

    private static CompletionStage<String> submit(Player actor, Join request) {
        FAIL_STORE.set(request.failStore());
        if (request.spotId() == null) {
            actor.context().joinEntrySpot().defer();
        } else {
            actor.context().joinSpot(request.spotId(), new Admission(request.reject())).defer();
        }
        return CompletableFuture.completedFuture("scheduled");
    }

    public record TargetProbe() {}

    public static final class TargetProbeHandler
            implements ZLinkSpotRequestHandler<Room, TargetProbe, String> {
        @Override
        public CompletionStage<String> handle(Room spot, TargetProbe request) {
            return CompletableFuture.completedFuture("target-ready");
        }
    }

    public static final class EntryJoin
            implements ZLinkEntrySpotActorRequestHandler<Entry, Player, Join, String> {
        @Override
        public CompletionStage<String> handle(
                Entry spot, Player actor, ZLinkMessageContext context, Join request) {
            return submit(actor, request);
        }
    }

    public static final class RoomJoin
            implements ZLinkSpotActorRequestHandler<Room, Player, Join, String> {
        @Override
        public CompletionStage<String> handle(
                Room spot, Player actor, ZLinkMessageContext context, Join request) {
            return submit(actor, request);
        }
    }
}
