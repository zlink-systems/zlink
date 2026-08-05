package systems.zlink.framework.runtime;

import systems.zlink.framework.runtime.configuration.DefaultZLinkFrameworkOptions;
import systems.zlink.framework.runtime.host.ZLinkFrameworkRuntime;

import systems.zlink.framework.runtime.internal.backend.*;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertTrue;

import java.time.Duration;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.concurrent.atomic.AtomicReference;
import org.junit.jupiter.api.Test;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.core.Zlink;
import systems.zlink.framework.actors.ZLinkActor;
import systems.zlink.framework.messaging.ZLinkMessage;
import systems.zlink.framework.spots.ZLinkSpot;
import systems.zlink.framework.spots.ZLinkSpotContext;
import systems.zlink.framework.spots.ZLinkSpotCreateResponse;
import systems.zlink.framework.spots.ZLinkSpotCreateResult;
import systems.zlink.framework.spots.ZLinkSpotCreateState;
import systems.zlink.framework.spots.ZLinkSpotTimerHandler;
import systems.zlink.framework.spots.ZLinkTimerTick;
import systems.zlink.framework.runtime.binding.ZLinkJavaBackendAdapterFactory;
import systems.zlink.framework.runtime.locations.ZLinkInMemoryLocationStore;

final class SpotManagerTest {
    @Test
    void spotManager_createListCloseAndPublish_workThroughFrameworkRuntime() {
        Zlink.version();
        String suffix = Long.toUnsignedString(System.nanoTime(), 36);
        DefaultZLinkFrameworkOptions options = new DefaultZLinkFrameworkOptions();
        options.addLocationStore(new ZLinkInMemoryLocationStore());
        { var node = options.addRouteMesh("game"); node.listen("inproc://spot-manager-router-" + suffix).setRoutingId(RoutingId.from("spot-manager-node-" + suffix));
            node.objects().server().addSpotFactory("GameSpot", GameSpot.class, factory -> factory.disableRelocation()); }
        try (ZLinkFrameworkRuntime runtime =
                 RuntimeTestSupport.startFramework(options, new ZLinkJavaBackendAdapterFactory())) {
            ZLinkSpotCreateResult created = runtime.spotManager()
                .create("GameSpot")
                .submit()
                .toCompletableFuture()
                .join();
            assertEquals(ZLinkSpotCreateState.CREATED, created.state());
            assertEquals(created.spot(), runtime.spotManager()
                .find(created.spot().spotId())
                .toCompletableFuture()
                .join()
                .orElseThrow());
            assertTrue(runtime.spotManager()
                .close(created.spot())
                .toCompletableFuture()
                .join());
        }
    }

    @Test
    void spotManager_getOrCreate_createsOnceAndReusesExistingSpot() {
        Zlink.version();
        String suffix = Long.toUnsignedString(System.nanoTime(), 36);
        DefaultZLinkFrameworkOptions options = new DefaultZLinkFrameworkOptions();
        options.addLocationStore(new ZLinkInMemoryLocationStore());
        { var node = options.addRouteMesh("game"); node.listen("inproc://spot-once-router-" + suffix).setRoutingId(RoutingId.from("spot-once-node-" + suffix));
            node.objects().server().addSpotFactory("GameSpot", GameSpot.class, factory -> factory.disableRelocation()); }
        String spotId = "game-once-" + suffix;

        try (ZLinkFrameworkRuntime runtime =
                 RuntimeTestSupport.startFramework(options, new ZLinkJavaBackendAdapterFactory())) {
            assertEquals(ZLinkSpotCreateState.CREATED, runtime.spotManager()
                .getOrCreate(spotId, "GameSpot")
                .submit()
                .toCompletableFuture()
                .join()
                .state());
            assertEquals(ZLinkSpotCreateState.EXISTING, runtime.spotManager()
                .getOrCreate(spotId, "GameSpot")
                .submit()
                .toCompletableFuture()
                .join()
                .state());
        }
    }

    @Test
    void spotManager_getOrCreate_concurrentCallReturnsExistingAndUsesFirstRequest() throws Exception {
        Zlink.version();
        SlowCreateSpot.reset();
        String suffix = Long.toUnsignedString(System.nanoTime(), 36);
        DefaultZLinkFrameworkOptions options = new DefaultZLinkFrameworkOptions();
        options.addLocationStore(new ZLinkInMemoryLocationStore());
        { var node = options.addRouteMesh("game"); node.listen("inproc://spot-concurrent-router-" + suffix).setRoutingId(RoutingId.from("spot-concurrent-node-" + suffix));
            node.objects().server().addSpotFactory("SlowCreateSpot", SlowCreateSpot.class, factory -> factory.disableRelocation()); }
        String spotId = "game-concurrent-" + suffix;

        try (ZLinkFrameworkRuntime runtime =
                 RuntimeTestSupport.startFramework(options, new ZLinkJavaBackendAdapterFactory())) {
            CompletionStage<ZLinkSpotCreateResult> first = runtime.spotManager()
                .getOrCreate(spotId, "SlowCreateSpot")
                .request(ZLinkMessage.of("first"))
                .submit();
            assertTrue(SlowCreateSpot.createStarted.await(3, TimeUnit.SECONDS));

            CompletionStage<ZLinkSpotCreateResult> second = runtime.spotManager()
                .getOrCreate(spotId, "SlowCreateSpot")
                .request(ZLinkMessage.of("second"))
                .submit();
            SlowCreateSpot.release.complete(null);

            ZLinkSpotCreateState firstState =
                first.toCompletableFuture().get(3, TimeUnit.SECONDS).state();
            ZLinkSpotCreateState secondState =
                second.toCompletableFuture().get(3, TimeUnit.SECONDS).state();

            assertEquals(ZLinkSpotCreateState.CREATED, firstState);
            assertEquals(ZLinkSpotCreateState.EXISTING, secondState);
            assertEquals(1, SlowCreateSpot.createCalls.get());
            assertEquals("first", SlowCreateSpot.createRequest.get());
        }
    }

    @Test
    void spot_publishTimerAndClose_stopCallbacksWork() throws InterruptedException {
        Zlink.version();
        PublishingSpot.reset();
        String suffix = Long.toUnsignedString(System.nanoTime(), 36);
        DefaultZLinkFrameworkOptions options = new DefaultZLinkFrameworkOptions();
        options.addLocationStore(new ZLinkInMemoryLocationStore());
        { var node = options.addRouteMesh("game"); node.listen("inproc://spot-timer-router-" + suffix).setRoutingId(RoutingId.from("spot-timer-router-node-" + suffix));
                node.objects().server().addSpotFactory("PublishingSpot", PublishingSpot.class, factory -> factory.disableRelocation()); }
        try (ZLinkFrameworkRuntime runtime =
                 RuntimeTestSupport.startFramework(options, new ZLinkJavaBackendAdapterFactory())) {
            ZLinkSpotCreateResult created = runtime.spotManager()
                .create("PublishingSpot")
                .submit()
                .toCompletableFuture()
                .join();
            assertEquals(ZLinkSpotCreateState.CREATED, created.state());
            assertTrue(PublishingSpot.initializedOnVirtualThread.get());
            assertTrue(PublishingSpot.timerPublished.await(3, TimeUnit.SECONDS));
            assertTrue(PublishingSpot.timerOnVirtualThread.get());

            assertTrue(runtime.spotManager()
                .close(created.spot())
                .toCompletableFuture()
                .join());
            assertTrue(PublishingSpot.closed.await(1, TimeUnit.SECONDS));
            assertTrue(PublishingSpot.closedOnVirtualThread.get());

            int ticksAtRemove = PublishingSpot.ticks.get();
            PublishingSpot.removed.set(true);
            assertFalse(PublishingSpot.afterRemoveTick.await(100, TimeUnit.MILLISECONDS));
            assertEquals(ticksAtRemove, PublishingSpot.ticks.get());
        }
    }

    @Test
    void spotManager_create_returnsRejectedAndDoesNotRegisterSpotWhenOnCreateRejects() {
        Zlink.version();
        String suffix = Long.toUnsignedString(System.nanoTime(), 36);
        DefaultZLinkFrameworkOptions options = new DefaultZLinkFrameworkOptions();
        options.addLocationStore(new ZLinkInMemoryLocationStore());
        { var node = options.addRouteMesh("game"); node.listen("inproc://spot-reject-router-" + suffix).setRoutingId(RoutingId.from("spot-reject-node-" + suffix));
            node.objects().server().addSpotFactory("RejectingSpot", RejectingSpot.class, factory -> factory.disableRelocation()); }
        try (ZLinkFrameworkRuntime runtime =
                 RuntimeTestSupport.startFramework(options, new ZLinkJavaBackendAdapterFactory())) {
            var rejected = runtime.spotManager()
                .create("RejectingSpot")
                .request(ZLinkMessage.of("closed"))
                .submit()
                .toCompletableFuture()
                .join();

            assertEquals(ZLinkSpotCreateState.REJECTED, rejected.state());
            assertEquals("reject:closed", rejected.reply().decode(String.class));
            assertTrue(runtime.spotManager()
                .find(rejected.spot().spotId())
                .toCompletableFuture()
                .join()
                .isEmpty());
        }
    }

    public static final class GameSpot implements ZLinkSpot<ZLinkActor> {
        @Override
        public ZLinkSpotContext context() {
            return null;
        }

        @Override public CompletionStage<Void> onJoinedActor(ZLinkActor actor) { return CompletableFuture.completedFuture(null); }
        @Override public CompletionStage<Void> onLeaveActor(ZLinkActor actor) { return CompletableFuture.completedFuture(null); }

        @Override
        public CompletionStage<Void> onInitialize() {
            return CompletableFuture.completedFuture(null);
        }
    }

    public static final class PublishingSpot implements ZLinkSpot<ZLinkActor> {
        static final AtomicInteger ticks = new AtomicInteger();
        static final AtomicBoolean removed = new AtomicBoolean();
        static final AtomicBoolean initializedOnVirtualThread = new AtomicBoolean();
        static final AtomicBoolean timerOnVirtualThread = new AtomicBoolean();
        static final AtomicBoolean closedOnVirtualThread = new AtomicBoolean();
        static CountDownLatch timerPublished;
        static CountDownLatch closed;
        static CountDownLatch afterRemoveTick;

        private final ZLinkSpotContext context;

        public PublishingSpot(ZLinkSpotContext context) {
            this.context = context;
        }

        static void reset() {
            ticks.set(0);
            removed.set(false);
            initializedOnVirtualThread.set(false);
            timerOnVirtualThread.set(false);
            closedOnVirtualThread.set(false);
            timerPublished = new CountDownLatch(1);
            closed = new CountDownLatch(1);
            afterRemoveTick = new CountDownLatch(1);
        }

        @Override
        public ZLinkSpotContext context() {
            return context;
        }

        @Override public CompletionStage<Void> onJoinedActor(ZLinkActor actor) { return CompletableFuture.completedFuture(null); }
        @Override public CompletionStage<Void> onLeaveActor(ZLinkActor actor) { return CompletableFuture.completedFuture(null); }

        @Override
        public CompletionStage<Void> onInitialize() {
            initializedOnVirtualThread.set(Thread.currentThread().isVirtual());
            return context.addTimer(
                    "heartbeat",
                    Duration.ofMillis(10),
                    HeartbeatTimerHandler.class,
                    null)
                .thenApply(timer -> null);
        }

        @Override
        public CompletionStage<Void> onClosing() {
            closedOnVirtualThread.set(Thread.currentThread().isVirtual());
            closed.countDown();
            return CompletableFuture.completedFuture(null);
        }
    }

    public static final class RejectingSpot implements ZLinkSpot<ZLinkActor> {
        private final ZLinkSpotContext context;

        public RejectingSpot(ZLinkSpotContext context) {
            this.context = context;
        }

        @Override
        public ZLinkSpotContext context() {
            return context;
        }

        @Override public CompletionStage<Void> onJoinedActor(ZLinkActor actor) { return CompletableFuture.completedFuture(null); }
        @Override public CompletionStage<Void> onLeaveActor(ZLinkActor actor) { return CompletableFuture.completedFuture(null); }

        @Override
        public CompletionStage<ZLinkSpotCreateResponse> onCreate(ZLinkMessage request) {
            return CompletableFuture.completedFuture(
                ZLinkSpotCreateResponse.reject("reject:" + request.decode(String.class)));
        }
    }

    public static final class SlowCreateSpot implements ZLinkSpot<ZLinkActor> {
        private static final AtomicInteger createCalls = new AtomicInteger();
        private static final AtomicReference<String> createRequest = new AtomicReference<>();
        private static CountDownLatch createStarted;
        private static CompletableFuture<Void> release;

        private final ZLinkSpotContext context;

        public SlowCreateSpot(ZLinkSpotContext context) {
            this.context = context;
        }

        static void reset() {
            createCalls.set(0);
            createRequest.set(null);
            createStarted = new CountDownLatch(1);
            release = new CompletableFuture<>();
        }

        @Override
        public ZLinkSpotContext context() {
            return context;
        }

        @Override public CompletionStage<Void> onJoinedActor(ZLinkActor actor) { return CompletableFuture.completedFuture(null); }
        @Override public CompletionStage<Void> onLeaveActor(ZLinkActor actor) { return CompletableFuture.completedFuture(null); }

        @Override
        public CompletionStage<ZLinkSpotCreateResponse> onCreate(ZLinkMessage request) {
            createCalls.incrementAndGet();
            createRequest.set(request.decode(String.class));
            createStarted.countDown();
            return release.thenApply(ignored -> ZLinkSpotCreateResponse.accept());
        }
    }

    public static final class HeartbeatTimerHandler implements ZLinkSpotTimerHandler<PublishingSpot> {
        @Override
        public CompletionStage<Void> handle(PublishingSpot spot, ZLinkTimerTick tick) {
            PublishingSpot.timerOnVirtualThread.set(Thread.currentThread().isVirtual());
            if (PublishingSpot.removed.get()) {
                PublishingSpot.afterRemoveTick.countDown();
            }
            PublishingSpot.ticks.incrementAndGet();
            spot.context().outbound().publish("game", "heartbeat", "tick").submit();
            PublishingSpot.timerPublished.countDown();
            return CompletableFuture.completedFuture(null);
        }
    }
}
