package systems.zlink.framework.runtime.host;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertNotEquals;
import static org.junit.jupiter.api.Assertions.assertThrows;
import static org.junit.jupiter.api.Assertions.assertTrue;

import org.junit.jupiter.api.Test;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.actors.ZLinkActor;
import systems.zlink.framework.actors.ZLinkActorContext;
import systems.zlink.framework.actors.ZLinkActorFactory;
import systems.zlink.framework.locationprovider.ZLinkLocationStore;
import systems.zlink.framework.locationprovider.ZLinkStoreCancellation;
import systems.zlink.framework.locationprovider.ZLinkStoreKey;
import systems.zlink.framework.locationprovider.ZLinkStoreReadResult;
import systems.zlink.framework.locationprovider.ZLinkStoreScanRequest;
import systems.zlink.framework.locationprovider.ZLinkStoreScanResult;
import systems.zlink.framework.locationprovider.ZLinkStoreWriteRequest;
import systems.zlink.framework.locationprovider.ZLinkStoreWriteResult;
import systems.zlink.framework.locations.ZLinkPageRequest;
import systems.zlink.framework.messaging.ZLinkMessage;
import systems.zlink.framework.runtime.binding.ZLinkJavaBackendAdapterFactory;
import systems.zlink.framework.runtime.configuration.DefaultZLinkFrameworkOptions;
import systems.zlink.framework.runtime.internal.locations.ZLinkProviderLocationRepository;
import systems.zlink.framework.runtime.locations.ZLinkInMemoryProviderLocationStore;
import systems.zlink.framework.spots.ZLinkSpot;
import systems.zlink.framework.spots.ZLinkSpotActorJoinResult;
import systems.zlink.framework.spots.ZLinkSpotContext;
import systems.zlink.framework.spots.ZLinkSpotRequestHandler;
import systems.zlink.framework.spots.ZLinkSpotTimerHandler;
import systems.zlink.framework.spots.ZLinkTimerOptions;
import systems.zlink.framework.spots.ZLinkTimerOverrunPolicy;
import systems.zlink.framework.spots.ZLinkTimerTick;

import java.time.Duration;
import java.util.UUID;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.function.BooleanSupplier;

final class ZLinkLocationRuntimeHostStartupTest {
    @Test
    void degradedHostCompletesStartupBeforeOwnerLeaseRecovery() throws Exception {
        RecoveringLocationStore store = new RecoveringLocationStore();
        DefaultZLinkFrameworkOptions options = new DefaultZLinkFrameworkOptions();
        options.addLocationStore(store);
        options.configureLocations().setOwnerLeaseTtl(Duration.ofSeconds(1));
        options.configureLocations().setOwnerLeaseRenewInterval(Duration.ofMillis(20));
        options.configureLocations().setOwnerLeaseRenewTimeout(Duration.ofMillis(10));
        options.configureLocations().setOwnerLeaseFencingMargin(Duration.ofMillis(100));
        options.addRouteMesh("game")
                .listen("inproc://degraded-host-" + UUID.randomUUID())
                .setRoutingId(RoutingId.from("degraded-host"));

        try (ZLinkFrameworkRuntime runtime =
                ZLinkFrameworkRuntimeTestAccess.start(
                        options, new ZLinkJavaBackendAdapterFactory())) {
            assertTrue(store.unavailableObserved.await(1, TimeUnit.SECONDS));
            ZLinkFrameworkRuntimeTestAccess.startupCompletion(runtime)
                    .toCompletableFuture()
                    .get(1, TimeUnit.SECONDS);
            await(() -> store.failedRequests.get() >= 3, Duration.ofSeconds(1));

            assertFalse(runtime.isReady());
            assertTrue(store.descriptors().isEmpty());

            store.available.set(true);

            await(() -> runtime.isReady() && !store.descriptors().isEmpty(), Duration.ofSeconds(2));
        }
    }

    @Test
    void ownerAdmissionDeadlineGatesAndResumesAllOwnerBoundWork() throws Exception {
        ProbeSpot.reset();
        ProbeActorFactory.reset();
        RecoveringLocationStore store = new RecoveringLocationStore(true);
        DefaultZLinkFrameworkOptions options = new DefaultZLinkFrameworkOptions();
        options.addLocationStore(store);
        options.configureLocations().setOwnerLeaseTtl(Duration.ofSeconds(1));
        options.configureLocations().setOwnerLeaseRenewInterval(Duration.ofMillis(20));
        options.configureLocations().setOwnerLeaseRenewTimeout(Duration.ofMillis(10));
        options.configureLocations().setOwnerLeaseFencingMargin(Duration.ofMillis(700));
        var mesh =
                options.addRouteMesh("game")
                        .listen("inproc://owner-admission-" + UUID.randomUUID())
                        .setRoutingId(RoutingId.from("owner-admission"));
        mesh.objects()
                .server()
                .addSpotFactory("probe", ProbeSpot.class, factory -> factory.disableRelocation());
        mesh.objects()
                .server()
                .addActorFactory(
                        "probe-actor",
                        ProbeActor.class,
                        ProbeActorFactory.class,
                        factory -> factory.disableRelocation());

        try (ZLinkFrameworkRuntime runtime =
                ZLinkFrameworkRuntimeTestAccess.start(
                        options, new ZLinkJavaBackendAdapterFactory())) {
            await(runtime::isReady, Duration.ofSeconds(2));
            runtime.spotManager()
                    .getOrCreate("probe", "probe")
                    .submit()
                    .toCompletableFuture()
                    .get(2, TimeUnit.SECONDS);
            long descriptorRevision = store.descriptors().getFirst().descriptorRevision();
            assertEquals("pong:open", request("open"));
            await(() -> ProbeSpot.timerTicks.get() > 0, Duration.ofSeconds(1));
            ProbeActorFactory.block();
            CompletableFuture<?> factoryAcrossDeadline =
                    runtime.actorManager()
                            .create("crossing-actor", "probe-actor")
                            .submit()
                            .toCompletableFuture();
            await(() -> ProbeActorFactory.invocations.get() == 1, Duration.ofSeconds(1));

            store.available.set(false);
            await(
                    () -> !ZLinkFrameworkRuntimeTestAccess.ownerAdmissionOpen(runtime),
                    Duration.ofSeconds(1));

            assertThrows(
                    IllegalStateException.class,
                    () ->
                            runtime.routeMeshRuntimeOptionsInternal()
                                    .mesh("game")
                                    .setPlacementWeight(90));
            int messagesWhileClosed = ProbeSpot.messages.get();
            assertThrows(Exception.class, () -> request("closed"));
            assertEquals(messagesWhileClosed, ProbeSpot.messages.get());
            Thread.sleep(30);
            int ticksWhileClosed = ProbeSpot.timerTicks.get();
            Thread.sleep(50);
            assertEquals(ticksWhileClosed, ProbeSpot.timerTicks.get());
            ProbeActorFactory.release.complete(null);
            assertThrows(Exception.class, () -> factoryAcrossDeadline.get(1, TimeUnit.SECONDS));
            assertEquals(1, ProbeActorFactory.invocations.get());
            assertEquals(
                    ZLinkFrameworkRelocationReason.RUNTIME_NOT_READY, relocate(runtime).reason());

            store.available.set(true);
            await(
                    () ->
                            ZLinkFrameworkRuntimeTestAccess.ownerAdmissionOpen(runtime)
                                    && store.descriptors().getFirst().descriptorRevision()
                                            > descriptorRevision,
                    Duration.ofSeconds(1));
            runtime.routeMeshRuntimeOptionsInternal().mesh("game").setPlacementWeight(80);
            assertEquals("pong:recovered", request("recovered"));
            await(() -> ProbeSpot.timerTicks.get() > ticksWhileClosed, Duration.ofSeconds(1));
            ProbeActorFactory.unblock();
            runtime.actorManager()
                    .create("recovered-actor", "probe-actor")
                    .submit()
                    .toCompletableFuture()
                    .get(2, TimeUnit.SECONDS);
            assertEquals(2, ProbeActorFactory.invocations.get());
            assertNotEquals(
                    ZLinkFrameworkRelocationReason.RUNTIME_NOT_READY, relocate(runtime).reason());
        }
    }

    private static String request(String value) throws Exception {
        return ProbeSpot.last
                .context()
                .outbound()
                .requestToSpot("probe", new Ping(value))
                .timeout(Duration.ofMillis(250))
                .submit(Pong.class)
                .toCompletableFuture()
                .get(1, TimeUnit.SECONDS)
                .value();
    }

    private static ZLinkFrameworkRelocationResult relocate(ZLinkFrameworkRuntime runtime)
            throws Exception {
        return runtime.relocate(
                        new ZLinkFrameworkRelocationOptions(
                                ZLinkFrameworkRelocationMode.PLANNED_MAINTENANCE,
                                null,
                                Duration.ofMillis(100)))
                .toCompletableFuture()
                .get(1, TimeUnit.SECONDS);
    }

    private static void await(BooleanSupplier condition, Duration timeout)
            throws InterruptedException {
        long deadline = System.nanoTime() + timeout.toNanos();
        while (!condition.getAsBoolean() && System.nanoTime() < deadline) {
            Thread.sleep(1);
        }
        assertTrue(condition.getAsBoolean());
    }

    private static final class RecoveringLocationStore implements ZLinkLocationStore {
        private final ZLinkInMemoryProviderLocationStore inner =
                new ZLinkInMemoryProviderLocationStore();
        private final ZLinkProviderLocationRepository repository =
                new ZLinkProviderLocationRepository(inner);
        private final AtomicBoolean available = new AtomicBoolean();
        private final AtomicInteger failedRequests = new AtomicInteger();
        private final CountDownLatch unavailableObserved = new CountDownLatch(1);

        RecoveringLocationStore() {}

        RecoveringLocationStore(boolean available) {
            this.available.set(available);
        }

        @Override
        public CompletionStage<ZLinkStoreReadResult> read(
                ZLinkStoreKey key, ZLinkStoreCancellation cancellation) {
            if (!available.get()) {
                return unavailable();
            }
            return inner.read(key, cancellation);
        }

        @Override
        public CompletionStage<ZLinkStoreWriteResult> write(
                ZLinkStoreWriteRequest request, ZLinkStoreCancellation cancellation) {
            if (!available.get()) {
                return unavailable();
            }
            return inner.write(request, cancellation);
        }

        @Override
        public CompletionStage<ZLinkStoreScanResult> scan(
                ZLinkStoreScanRequest request, ZLinkStoreCancellation cancellation) {
            if (!available.get()) {
                return unavailable();
            }
            return inner.scan(request, cancellation);
        }

        java.util.List<systems.zlink.framework.runtime.internal.locations.ZLinkMeshNodeDescriptor>
                descriptors() {
            return repository
                    .listMeshNodes("game", ZLinkPageRequest.firstPage())
                    .toCompletableFuture()
                    .join()
                    .items();
        }

        private <T> CompletionStage<T> unavailable() {
            failedRequests.incrementAndGet();
            unavailableObserved.countDown();
            return CompletableFuture.failedFuture(
                    new IllegalStateException("location store transport unavailable"));
        }
    }

    public record Ping(String value) {}

    public record Pong(String value) {}

    public static final class ProbeSpot implements ZLinkSpot<ZLinkActor> {
        private static final AtomicInteger messages = new AtomicInteger();
        private static final AtomicInteger timerTicks = new AtomicInteger();
        private static ProbeSpot last;
        private final ZLinkSpotContext context;

        public ProbeSpot(ZLinkSpotContext context) {
            this.context = context;
            last = this;
        }

        static void reset() {
            messages.set(0);
            timerTicks.set(0);
            last = null;
        }

        @Override
        public ZLinkSpotContext context() {
            return context;
        }

        @Override
        public void configure() {
            context.handlers().addHandler(PingHandler.class);
        }

        @Override
        public CompletionStage<Void> onInitialize() {
            return context.addTimer(
                            "lease-probe",
                            Duration.ofMillis(10),
                            TimerHandler.class,
                            new ZLinkTimerOptions(
                                    ZLinkTimerOverrunPolicy.SKIP_LATE_TICKS, 1, false))
                    .thenApply(ignored -> null);
        }

        @Override
        public CompletionStage<ZLinkSpotActorJoinResult> onActorJoin(
                String actorId, ZLinkMessage request) {
            return CompletableFuture.completedFuture(ZLinkSpotActorJoinResult.accept());
        }

        @Override
        public CompletionStage<Void> onJoinedActor(ZLinkActor actor) {
            return CompletableFuture.completedFuture(null);
        }

        @Override
        public CompletionStage<Void> onLeaveActor(ZLinkActor actor) {
            return CompletableFuture.completedFuture(null);
        }
    }

    public static final class PingHandler
            implements ZLinkSpotRequestHandler<ProbeSpot, Ping, Pong> {
        @Override
        public CompletionStage<Pong> handle(ProbeSpot spot, Ping request) {
            ProbeSpot.messages.incrementAndGet();
            return CompletableFuture.completedFuture(new Pong("pong:" + request.value()));
        }
    }

    public static final class TimerHandler implements ZLinkSpotTimerHandler<ProbeSpot> {
        @Override
        public CompletionStage<Void> handle(ProbeSpot spot, ZLinkTimerTick tick) {
            ProbeSpot.timerTicks.incrementAndGet();
            return CompletableFuture.completedFuture(null);
        }
    }

    public record ProbeActor(ZLinkActorContext context) implements ZLinkActor {}

    public static final class ProbeActorFactory implements ZLinkActorFactory {
        private static final AtomicInteger invocations = new AtomicInteger();
        private static CompletableFuture<Void> release;

        static void reset() {
            invocations.set(0);
            unblock();
        }

        static void block() {
            release = new CompletableFuture<>();
        }

        static void unblock() {
            release = CompletableFuture.completedFuture(null);
        }

        @Override
        public CompletionStage<ZLinkActor> create(ZLinkActorContext context) {
            invocations.incrementAndGet();
            return release.thenApply(ignored -> new ProbeActor(context));
        }
    }
}
