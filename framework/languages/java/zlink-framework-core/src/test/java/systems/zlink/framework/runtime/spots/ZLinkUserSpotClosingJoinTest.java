package systems.zlink.framework.runtime.spots;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertInstanceOf;
import static org.junit.jupiter.api.Assertions.assertTrue;

import org.junit.jupiter.api.Test;

import systems.zlink.framework.ZLinkMessageContext;
import systems.zlink.framework.actors.ZLinkActor;
import systems.zlink.framework.actors.ZLinkActorContext;
import systems.zlink.framework.actors.ZLinkActorCreateResult;
import systems.zlink.framework.actors.ZLinkActorFactory;
import systems.zlink.framework.actors.ZLinkActorJoinCompletion;
import systems.zlink.framework.errors.ZLinkFrameworkErrorKind;
import systems.zlink.framework.locationprovider.ZLinkLocationStore;
import systems.zlink.framework.locationprovider.ZLinkStoreCancellation;
import systems.zlink.framework.locationprovider.ZLinkStoreDelete;
import systems.zlink.framework.locationprovider.ZLinkStoreKey;
import systems.zlink.framework.locationprovider.ZLinkStorePut;
import systems.zlink.framework.locationprovider.ZLinkStoreReadResult;
import systems.zlink.framework.locationprovider.ZLinkStoreScanRequest;
import systems.zlink.framework.locationprovider.ZLinkStoreScanResult;
import systems.zlink.framework.locationprovider.ZLinkStoreWriteRequest;
import systems.zlink.framework.locationprovider.ZLinkStoreWriteResult;
import systems.zlink.framework.runtime.configuration.DefaultZLinkFrameworkOptions;
import systems.zlink.framework.runtime.host.ZLinkFrameworkRuntime;
import systems.zlink.framework.runtime.host.ZLinkFrameworkRuntimeTestAccess;
import systems.zlink.framework.runtime.locations.ZLinkInMemoryLocationStore;
import systems.zlink.framework.spots.SpotRef;
import systems.zlink.framework.spots.ZLinkEntrySpot;
import systems.zlink.framework.spots.ZLinkEntrySpotActorRequestHandler;
import systems.zlink.framework.spots.ZLinkEntrySpotContext;
import systems.zlink.framework.spots.ZLinkSpot;
import systems.zlink.framework.spots.ZLinkSpotActorJoinResult;
import systems.zlink.framework.spots.ZLinkSpotContext;
import systems.zlink.framework.spots.ZLinkSpotCreateResult;
import systems.zlink.framework.spots.ZLinkSpotCreateState;
import systems.zlink.framework.spots.ZLinkSpotRequestHandler;

import java.time.Duration;
import java.util.UUID;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicBoolean;

final class ZLinkUserSpotClosingJoinTest {
    private static final int WAIT_SECONDS = 10;
    private static volatile CompletableFuture<Void> onClosingEntered;
    private static volatile CompletableFuture<Void> releaseOnClosing;
    private static volatile CompletableFuture<CompletableFuture<Boolean>> selfCloseStarted;
    private static volatile CompletableFuture<ZLinkActorJoinCompletion> joinCompleted;

    @Test
    void deferredJoinFailsNotFoundWhileSelfCloseWaitsForOnClosing() throws Exception {
        TestRig rig = start(true);
        try {
            SpotRef ref = rig.existingSpot();
            rig.requestSelfClose();
            CompletableFuture<Boolean> close = selfCloseStarted.get(WAIT_SECONDS, TimeUnit.SECONDS);
            onClosingEntered.get(WAIT_SECONDS, TimeUnit.SECONDS);

            rig.requestDeferredJoin();
            assertJoinFailedNotFound();
            assertFalse(close.isDone(), "OnClosing keeps the accepted close pending");

            releaseOnClosing.complete(null);
            assertTrue(close.get(WAIT_SECONDS, TimeUnit.SECONDS));
            assertFalse(
                    rig.runtime
                            .spotManager()
                            .close(ref)
                            .toCompletableFuture()
                            .get(WAIT_SECONDS, TimeUnit.SECONDS));
        } finally {
            releaseOnClosing.complete(null);
            rig.close();
        }
    }

    @Test
    void deferredJoinFailsNotFoundWhileManagerCloseWaitsForOnClosing() throws Exception {
        TestRig rig = start(true);
        try {
            SpotRef ref = rig.existingSpot();
            CompletableFuture<Boolean> close =
                    rig.runtime.spotManager().close(ref).toCompletableFuture();
            onClosingEntered.get(WAIT_SECONDS, TimeUnit.SECONDS);

            rig.requestDeferredJoin();
            assertJoinFailedNotFound();
            assertFalse(close.isDone(), "OnClosing keeps the accepted close pending");

            releaseOnClosing.complete(null);
            assertTrue(close.get(WAIT_SECONDS, TimeUnit.SECONDS));
            assertFalse(
                    rig.runtime
                            .spotManager()
                            .close(ref)
                            .toCompletableFuture()
                            .get(WAIT_SECONDS, TimeUnit.SECONDS));
        } finally {
            releaseOnClosing.complete(null);
            rig.close();
        }
    }

    @Test
    void deferredJoinDuringClosingCasWaitsForCommitBeforeFailingNotFound() throws Exception {
        TestRig rig = start(true);
        try {
            SpotRef ref = rig.existingSpot();
            rig.store.holdNextAuthorityPut();
            CompletableFuture<Boolean> close =
                    rig.runtime.spotManager().close(ref).toCompletableFuture();
            rig.store.closingWriteEntered.get(WAIT_SECONDS, TimeUnit.SECONDS);

            assertEquals(ZLinkSpotCreateState.EXISTING, rig.getOrCreate().state());
            rig.requestDeferredJoin();
            assertFalse(
                    joinCompleted.isDone(),
                    "Join remains pending while the Closing compare-and-set write is held");

            rig.store.releaseClosingWrite.complete(null);
            onClosingEntered.get(WAIT_SECONDS, TimeUnit.SECONDS);
            assertFalse(close.isDone(), "OnClosing keeps the accepted close pending");

            releaseOnClosing.complete(null);
            assertTrue(close.get(WAIT_SECONDS, TimeUnit.SECONDS));
            assertJoinFailedNotFound();
        } finally {
            releaseOnClosing.complete(null);
            rig.store.releaseClosingWrite.complete(null);
            rig.close();
        }
    }

    @Test
    void secondCloseOfClosingIncarnationJoinsInProgressClose() throws Exception {
        TestRig rig = start(false);
        try {
            SpotRef ref = rig.existingSpot();
            rig.store.holdNextAuthorityDelete();
            rig.requestSelfClose();
            CompletableFuture<Boolean> selfClose =
                    selfCloseStarted.get(WAIT_SECONDS, TimeUnit.SECONDS);
            rig.store.authorityDeleteEntered.get(WAIT_SECONDS, TimeUnit.SECONDS);
            CompletableFuture<Boolean> secondClose =
                    rig.runtime.spotManager().close(ref).toCompletableFuture();

            assertTrue(
                    rig.runtime
                            .spotManager()
                            .find(rig.spotId)
                            .toCompletableFuture()
                            .get(WAIT_SECONDS, TimeUnit.SECONDS)
                            .isEmpty(),
                    "Find hides the Closing authority");
            assertFalse(secondClose.isDone(), "A second Close waits for the in-progress Close");

            rig.store.releaseAuthorityDelete.complete(null);
            boolean firstCloseResult = selfClose.get(WAIT_SECONDS, TimeUnit.SECONDS);
            assertTrue(firstCloseResult);
            assertEquals(
                    Boolean.valueOf(firstCloseResult),
                    secondClose.get(WAIT_SECONDS, TimeUnit.SECONDS),
                    "A second Close joins and returns the in-progress Close result");
            assertFalse(
                    rig.runtime
                            .spotManager()
                            .close(ref)
                            .toCompletableFuture()
                            .get(WAIT_SECONDS, TimeUnit.SECONDS));
        } finally {
            releaseOnClosing.complete(null);
            rig.store.releaseAuthorityDelete.complete(null);
            rig.close();
        }
    }

    private static TestRig start(boolean holdOnClosing) throws Exception {
        onClosingEntered = new CompletableFuture<>();
        releaseOnClosing =
                holdOnClosing ? new CompletableFuture<>() : CompletableFuture.completedFuture(null);
        selfCloseStarted = new CompletableFuture<>();
        joinCompleted = new CompletableFuture<>();
        return new TestRig("closing-join-" + UUID.randomUUID());
    }

    private static void assertJoinFailedNotFound() throws Exception {
        ZLinkActorJoinCompletion completion = joinCompleted.get(WAIT_SECONDS, TimeUnit.SECONDS);
        ZLinkActorJoinCompletion.Failed failed =
                assertInstanceOf(ZLinkActorJoinCompletion.Failed.class, completion);
        assertEquals(ZLinkFrameworkErrorKind.NOT_FOUND, failed.kind());
    }

    public record BeginClose() {}

    public record JoinRequest(String spotId) {}

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
        public CompletionStage<Void> onJoinCompleted(ZLinkActorJoinCompletion completion) {
            joinCompleted.complete(completion);
            return CompletableFuture.completedFuture(null);
        }
    }

    public static final class PlayerFactory implements ZLinkActorFactory {
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
            context.handlers().addHandler(JoinHandler.class);
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

    public static final class JoinHandler
            implements ZLinkEntrySpotActorRequestHandler<Entry, Player, JoinRequest, String> {
        @Override
        public CompletionStage<String> handle(
                Entry spot, Player actor, ZLinkMessageContext context, JoinRequest request) {
            actor.context().joinSpot(request.spotId()).defer();
            return CompletableFuture.completedFuture("scheduled");
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
            context.handlers().addHandler(BeginCloseHandler.class);
        }

        @Override
        public CompletionStage<ZLinkSpotActorJoinResult> onActorJoin(
                String actorId, systems.zlink.framework.messaging.ZLinkMessage request) {
            return CompletableFuture.completedFuture(ZLinkSpotActorJoinResult.accept());
        }

        @Override
        public CompletionStage<Void> onClosing() {
            onClosingEntered.complete(null);
            return releaseOnClosing;
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

    public static final class BeginCloseHandler
            implements ZLinkSpotRequestHandler<Room, BeginClose, String> {
        @Override
        public CompletionStage<String> handle(Room spot, BeginClose request) {
            selfCloseStarted.complete(spot.context().close().toCompletableFuture());
            return CompletableFuture.completedFuture("closing");
        }
    }

    private static final class TestRig implements AutoCloseable {
        private final String spotId;
        private final String actorId;
        private final HoldingStore store;
        private final ZLinkFrameworkRuntime runtime;

        TestRig(String spotId) throws Exception {
            this.spotId = spotId;
            this.actorId = spotId + "-actor";
            this.store = new HoldingStore(spotId);
            var options = new DefaultZLinkFrameworkOptions();
            options.addLocationStore(store);
            var objects =
                    options.addRouteMesh("closing-join-mesh")
                            .listen("inproc://" + spotId)
                            .objects()
                            .server();
            objects.addEntrySpot(Entry.class);
            objects.addSpotFactory("room", Room.class, factory -> factory.disableRelocation());
            objects.addActorFactory(
                    "player",
                    Player.class,
                    PlayerFactory.class,
                    factory -> factory.disableRelocation());
            runtime = ZLinkFrameworkRuntimeTestAccess.start(options);

            ZLinkSpotCreateResult created =
                    runtime.spotManager()
                            .getOrCreate(spotId, "room")
                            .submit()
                            .toCompletableFuture()
                            .get(WAIT_SECONDS, TimeUnit.SECONDS);
            assertEquals(ZLinkSpotCreateState.CREATED, created.state());
            ZLinkActorCreateResult actor =
                    runtime.actorManager()
                            .create(actorId, "player")
                            .submit()
                            .toCompletableFuture()
                            .get(WAIT_SECONDS, TimeUnit.SECONDS);
            assertInstanceOf(ZLinkActorCreateResult.Created.class, actor);
        }

        SpotRef existingSpot() throws Exception {
            ZLinkSpotCreateResult result = getOrCreate();
            assertEquals(ZLinkSpotCreateState.EXISTING, result.state());
            return result.spot();
        }

        ZLinkSpotCreateResult getOrCreate() throws Exception {
            return runtime.spotManager()
                    .getOrCreate(spotId, "room")
                    .submit()
                    .toCompletableFuture()
                    .get(WAIT_SECONDS, TimeUnit.SECONDS);
        }

        void requestSelfClose() throws Exception {
            assertEquals(
                    "closing",
                    runtime.route()
                            .requestToSpot(spotId, new BeginClose())
                            .timeout(Duration.ofSeconds(WAIT_SECONDS))
                            .submit(String.class)
                            .toCompletableFuture()
                            .get(WAIT_SECONDS, TimeUnit.SECONDS));
        }

        void requestDeferredJoin() throws Exception {
            assertEquals(
                    "scheduled",
                    runtime.actorClient()
                            .requestToActor(actorId, new JoinRequest(spotId))
                            .timeout(Duration.ofSeconds(WAIT_SECONDS))
                            .submit(String.class)
                            .toCompletableFuture()
                            .get(WAIT_SECONDS, TimeUnit.SECONDS));
        }

        @Override
        public void close() {
            runtime.close();
        }
    }

    private static final class HoldingStore implements ZLinkLocationStore {
        private final ZLinkLocationStore inner = new ZLinkInMemoryLocationStore();
        private final String spotId;
        private final AtomicBoolean holdClosingWrite = new AtomicBoolean();
        private final AtomicBoolean holdAuthorityDelete = new AtomicBoolean();
        final CompletableFuture<Void> closingWriteEntered = new CompletableFuture<>();
        final CompletableFuture<Void> releaseClosingWrite = new CompletableFuture<>();
        final CompletableFuture<Void> authorityDeleteEntered = new CompletableFuture<>();
        final CompletableFuture<Void> releaseAuthorityDelete = new CompletableFuture<>();

        HoldingStore(String spotId) {
            this.spotId = spotId;
        }

        void holdNextAuthorityPut() {
            holdClosingWrite.set(true);
        }

        void holdNextAuthorityDelete() {
            holdAuthorityDelete.set(true);
        }

        @Override
        public CompletionStage<ZLinkStoreReadResult> read(
                ZLinkStoreKey key, ZLinkStoreCancellation cancellation) {
            return inner.read(key, cancellation);
        }

        @Override
        public CompletionStage<ZLinkStoreScanResult> scan(
                ZLinkStoreScanRequest request, ZLinkStoreCancellation cancellation) {
            return inner.scan(request, cancellation);
        }

        @Override
        public CompletionStage<ZLinkStoreWriteResult> write(
                ZLinkStoreWriteRequest request, ZLinkStoreCancellation cancellation) {
            boolean authorityPut =
                    request.mutations().stream()
                            .anyMatch(
                                    mutation ->
                                            mutation instanceof ZLinkStorePut put
                                                    && isSpotAuthority(put.key()));
            boolean authorityDelete =
                    request.mutations().stream()
                            .anyMatch(
                                    mutation ->
                                            mutation instanceof ZLinkStoreDelete delete
                                                    && isSpotAuthority(delete.key()));
            if (authorityPut && holdClosingWrite.compareAndSet(true, false)) {
                closingWriteEntered.complete(null);
                return releaseClosingWrite.thenCompose(
                        ignored -> inner.write(request, cancellation));
            }

            if (authorityDelete && holdAuthorityDelete.compareAndSet(true, false)) {
                authorityDeleteEntered.complete(null);
                return releaseAuthorityDelete.thenCompose(
                        ignored -> inner.write(request, cancellation));
            }
            return inner.write(request, cancellation);
        }

        private boolean isSpotAuthority(ZLinkStoreKey key) {
            return key.value().startsWith("authority\0") && key.value().endsWith("\0" + spotId);
        }
    }
}
