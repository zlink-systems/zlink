package systems.zlink.framework.runtime.host;

import static org.junit.jupiter.api.Assertions.assertDoesNotThrow;
import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertInstanceOf;
import static org.junit.jupiter.api.Assertions.assertNotEquals;
import static org.junit.jupiter.api.Assertions.assertNotSame;
import static org.junit.jupiter.api.Assertions.assertThrows;
import static org.junit.jupiter.api.Assertions.assertTrue;

import org.junit.jupiter.api.Test;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.actors.ZLinkActor;
import systems.zlink.framework.actors.ZLinkActorContext;
import systems.zlink.framework.actors.ZLinkActorFactory;
import systems.zlink.framework.errors.ZLinkFrameworkErrorKind;
import systems.zlink.framework.errors.ZLinkFrameworkException;
import systems.zlink.framework.locationprovider.ZLinkLocationStore;
import systems.zlink.framework.locationprovider.ZLinkStoreCancellation;
import systems.zlink.framework.locationprovider.ZLinkStoreKey;
import systems.zlink.framework.locationprovider.ZLinkStorePut;
import systems.zlink.framework.locationprovider.ZLinkStoreReadResult;
import systems.zlink.framework.locationprovider.ZLinkStoreScanRequest;
import systems.zlink.framework.locationprovider.ZLinkStoreScanResult;
import systems.zlink.framework.locationprovider.ZLinkStoreWriteApplied;
import systems.zlink.framework.locationprovider.ZLinkStoreWriteRequest;
import systems.zlink.framework.locationprovider.ZLinkStoreWriteResult;
import systems.zlink.framework.locations.ZLinkPageRequest;
import systems.zlink.framework.messaging.ZLinkMessage;
import systems.zlink.framework.runtime.binding.ZLinkJavaBackendAdapterFactory;
import systems.zlink.framework.runtime.configuration.DefaultZLinkFrameworkOptions;
import systems.zlink.framework.runtime.internal.locations.ZLinkProviderLocationRepository;
import systems.zlink.framework.runtime.locations.ZLinkInMemoryProviderLocationStore;
import systems.zlink.framework.runtime.locations.ZLinkRegisteredLocationStores;
import systems.zlink.framework.runtime.locations.ZLinkTimerAdmissionTestRuntime;
import systems.zlink.framework.spots.ZLinkSpot;
import systems.zlink.framework.spots.ZLinkSpotActorJoinResult;
import systems.zlink.framework.spots.ZLinkSpotContext;
import systems.zlink.framework.spots.ZLinkSpotRequestHandler;
import systems.zlink.framework.spots.ZLinkSpotTimerHandler;
import systems.zlink.framework.spots.ZLinkTimer;
import systems.zlink.framework.spots.ZLinkTimerOptions;
import systems.zlink.framework.spots.ZLinkTimerOverrunPolicy;
import systems.zlink.framework.spots.ZLinkTimerTick;

import java.nio.charset.StandardCharsets;
import java.time.Clock;
import java.time.Duration;
import java.time.Instant;
import java.time.ZoneOffset;
import java.util.List;
import java.util.UUID;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.ExecutionException;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.concurrent.atomic.AtomicLong;
import java.util.function.BooleanSupplier;

final class ZLinkLocationRuntimeHostStartupTest {
    @Test
    void malformedOwnerCounterEndsStartupWithDataLost() throws Exception {
        var store = new ZLinkInMemoryProviderLocationStore();
        store.write(
                        new ZLinkStoreWriteRequest(
                                List.of(),
                                List.of(
                                        new ZLinkStorePut(
                                                new ZLinkStoreKey("zlink:v11:owner-counter"),
                                                "01".getBytes(StandardCharsets.UTF_8),
                                                null))),
                        () -> false)
                .toCompletableFuture()
                .join();
        var options = new DefaultZLinkFrameworkOptions();
        options.addLocationStore(store);
        options.addRouteMesh("game").listen("inproc://invalid-owner-counter-" + UUID.randomUUID());
        try (var runtime =
                ZLinkFrameworkRuntimeTestAccess.start(
                        options, new ZLinkJavaBackendAdapterFactory())) {
            var failure =
                    assertThrows(
                            ExecutionException.class,
                            () ->
                                    ZLinkFrameworkRuntimeTestAccess.startupCompletion(runtime)
                                            .toCompletableFuture()
                                            .get(10, TimeUnit.SECONDS));
            assertEquals(
                    ZLinkFrameworkErrorKind.DATA_LOST,
                    assertInstanceOf(ZLinkFrameworkException.class, failure.getCause()).kind());
        }
    }

    @Test
    void shutdownObservesPendingStartupClaimBeforeCompleting() throws Exception {
        PendingStartupStore store = new PendingStartupStore();
        DefaultZLinkFrameworkOptions options = new DefaultZLinkFrameworkOptions();
        options.addLocationStore(store);
        options.addRouteMesh("game")
                .listen("inproc://pending-startup-" + UUID.randomUUID())
                .setRoutingId(RoutingId.from("pending-startup"));
        try (ZLinkFrameworkRuntime runtime =
                ZLinkFrameworkRuntimeTestAccess.start(
                        options, new ZLinkJavaBackendAdapterFactory())) {
            assertTrue(store.claimStarted.await(1, TimeUnit.SECONDS));
            assertFalse(
                    ZLinkFrameworkRuntimeTestAccess.startupCompletion(runtime)
                            .toCompletableFuture()
                            .isDone(),
                    "fixture must keep host startup pending before shutdown");
            assertEquals(
                    0, store.renewals.get(), "fixture must not have entered degraded heartbeat");
            int renewalsBeforeShutdown = store.renewals.get();
            var stopping = runtime.shutdown(Duration.ofSeconds(3)).toCompletableFuture();
            assertFalse(stopping.isDone(), "host must observe the pending startup claim");
            store.commitClaim();
            stopping.get(3, TimeUnit.SECONDS);
            assertTrue(store.released.await(1, TimeUnit.SECONDS));
            assertTrue(store.confirmationReads.get() > 0);
            assertEquals(renewalsBeforeShutdown, store.renewals.get());
            assertTrue(
                    store.inner.read(store.ownerKey, () -> false).toCompletableFuture().join()
                            instanceof
                            systems.zlink.framework.locationprovider.ZLinkStoreReadMissing);
        }
    }

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
    void ownerAdmissionDeadlineDiscardsOldIncarnationsAndAdmitsNewWork() throws Exception {
        ProbeSpot.reset();
        ProbeActorFactory.reset();
        var store = new RecoveringLocationStore(Clock.fixed(Instant.now(), ZoneOffset.UTC), true);
        Duration leaseTtl = Duration.ofSeconds(1);
        Duration renewInterval = Duration.ofMillis(20);
        Duration renewTimeout = Duration.ofMillis(10);
        Duration fencingMargin = Duration.ofMillis(700);
        DefaultZLinkFrameworkOptions options = new DefaultZLinkFrameworkOptions();
        options.addLocationStore(store);
        options.configureLocations().setOwnerLeaseTtl(leaseTtl);
        options.configureLocations().setOwnerLeaseRenewInterval(renewInterval);
        options.configureLocations().setOwnerLeaseRenewTimeout(renewTimeout);
        options.configureLocations().setOwnerLeaseFencingMargin(fencingMargin);
        var mesh =
                options.addRouteMesh("game")
                        .listen("inproc://owner-admission-" + UUID.randomUUID())
                        .setRoutingId(RoutingId.from("owner-admission"));
        mesh.objects()
                .server()
                .addSpotFactory("probe", ProbeSpot.class, factory -> factory.disableRelocation());
        mesh.objects()
                .server()
                .addEntrySpot(ActorCreationEntry.class)
                .addActorFactory(
                        "probe-actor",
                        ProbeActor.class,
                        ProbeActorFactory.class,
                        factory -> factory.disableRelocation());

        var monotonicNanos = new AtomicLong();
        var locationStores =
                ZLinkRegisteredLocationStores.fromUnified(
                        new ZLinkProviderLocationRepository(
                                store, monotonicNanos::get, fencingMargin));
        var locationRuntime =
                ZLinkTimerAdmissionTestRuntime.createWithManualHeartbeat(
                        locationStores,
                        leaseTtl,
                        renewInterval,
                        renewTimeout,
                        fencingMargin,
                        monotonicNanos);
        try (ZLinkFrameworkRuntime runtime =
                ZLinkFrameworkRuntimeTestAccess.startHostWithConstruction(
                        options,
                        new ZLinkJavaBackendAdapterFactory(),
                        systems.zlink.framework.runtime.internal.handlers.ZLinkHandlerActivator
                                .reflection(),
                        null,
                        new ZLinkFrameworkRuntimeTestAccess.Construction()
                                .locationStores(locationStores)
                                .locationRuntime(locationRuntime))) {
            // The first fixed RID claim observes the missing descriptor and starts its release
            // wait.
            long startupRenewalNanos = fencingMargin.multipliedBy(3).toNanos();
            monotonicNanos.set(startupRenewalNanos);
            assertTrue(
                    locationRuntime
                            .renewOwnerLeaseOnce()
                            .toCompletableFuture()
                            .get(1, TimeUnit.SECONDS));
            assertTrue(runtime.isReady());
            var ownerToken = locationRuntime.currentOwnerToken();
            runtime.spotManager()
                    .getOrCreate("probe", "probe")
                    .submit()
                    .toCompletableFuture()
                    .get(2, TimeUnit.SECONDS);
            ProbeSpot originalSpot = ProbeSpot.last;
            long descriptorRevision = store.descriptors().getFirst().descriptorRevision();
            assertEquals("pong:open", request("open"));
            assertTrue(originalSpot.firstTick.await(1, TimeUnit.SECONDS));
            ProbeActorFactory.block();
            CompletableFuture<?> factoryAcrossDeadline =
                    runtime.actorManager()
                            .create("crossing-actor", "probe-actor")
                            .submit()
                            .toCompletableFuture();
            assertTrue(ProbeActorFactory.started.await(1, TimeUnit.SECONDS));

            store.available.set(false);
            assertFalse(
                    locationRuntime
                            .renewOwnerLeaseOnce()
                            .toCompletableFuture()
                            .get(1, TimeUnit.SECONDS));
            assertTrue(locationRuntime.isOwnerAdmissionOpen());
            monotonicNanos.set(startupRenewalNanos + leaseTtl.minus(fencingMargin).toNanos());
            ZLinkTimerAdmissionTestRuntime.latch(locationRuntime);
            assertFalse(locationRuntime.isOwnerAdmissionOpen());
            assertTrue(locationRuntime.ownerLeaseSnapshot().outageDiscarded());

            assertDoesNotThrow(
                    () ->
                            runtime.routeMeshRuntimeOptionsInternal()
                                    .mesh("game")
                                    .setPlacementWeight(90));
            int messagesWhileClosed = ProbeSpot.messages.get();
            assertThrows(Exception.class, () -> request("closed"));
            assertEquals(messagesWhileClosed, ProbeSpot.messages.get());
            ProbeActorFactory.release.complete(null);
            assertThrows(Exception.class, () -> factoryAcrossDeadline.get(1, TimeUnit.SECONDS));
            assertEquals(1, ProbeActorFactory.invocations.get());
            assertEquals(
                    ZLinkFrameworkRelocationReason.RUNTIME_NOT_READY, relocate(runtime).reason());

            var descriptorWrite = store.reopenAndObserveDescriptorWrite();
            // Discarded executions can finish recovery while descriptor publication is pending.
            assertTrue(
                    locationRuntime
                            .renewOwnerLeaseOnce()
                            .toCompletableFuture()
                            .get(1, TimeUnit.SECONDS));
            descriptorWrite.get(1, TimeUnit.SECONDS);
            assertDoesNotThrow(locationRuntime::ensureOwnerAdmissionOpen);
            assertEquals(ownerToken, locationRuntime.currentOwnerToken());
            assertTrue(store.descriptors().getFirst().descriptorRevision() > descriptorRevision);
            assertTrue(originalSpot.timer.isDisposed());
            int discardedTicks = originalSpot.timerTicks.get();
            runtime.routeMeshRuntimeOptionsInternal().mesh("game").setPlacementWeight(80);
            runtime.spotManager()
                    .getOrCreate("recovered-probe", "probe")
                    .submit()
                    .toCompletableFuture()
                    .get(2, TimeUnit.SECONDS);
            assertNotSame(originalSpot, ProbeSpot.last);
            assertThrows(Exception.class, () -> request("discarded-incarnation"));
            assertEquals("pong:recovered", request("recovered-probe", "recovered"));
            assertTrue(ProbeSpot.last.firstTick.await(1, TimeUnit.SECONDS));
            assertEquals(discardedTicks, originalSpot.timerTicks.get());
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
        return request("probe", value);
    }

    private static String request(String spotId, String value) throws Exception {
        return ProbeSpot.last
                .context()
                .outbound()
                .requestToSpot(spotId, new Ping(value))
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

    private static final class PendingStartupStore implements ZLinkLocationStore {
        private final ZLinkInMemoryProviderLocationStore inner =
                new ZLinkInMemoryProviderLocationStore();
        private final CountDownLatch claimStarted = new CountDownLatch(1);
        private final CountDownLatch released = new CountDownLatch(1);
        private final AtomicInteger confirmationReads = new AtomicInteger();
        private final AtomicInteger renewals = new AtomicInteger();
        private final CompletableFuture<ZLinkStoreWriteResult> claim = new CompletableFuture<>();
        private ZLinkStoreWriteRequest claimRequest;
        private ZLinkStoreKey ownerKey;

        @Override
        public CompletionStage<ZLinkStoreReadResult> read(
                ZLinkStoreKey key, ZLinkStoreCancellation cancellation) {
            if (key.equals(ownerKey) && claim.isDone()) confirmationReads.incrementAndGet();
            return inner.read(key, cancellation);
        }

        @Override
        public CompletionStage<ZLinkStoreWriteResult> write(
                ZLinkStoreWriteRequest request, ZLinkStoreCancellation cancellation) {
            if (claimRequest == null
                    && request.mutations().getFirst() instanceof ZLinkStorePut put
                    && put.key().value().startsWith("owner-lease\0")) {
                claimRequest = request;
                ownerKey = put.key();
                claimStarted.countDown();
                return claim;
            }
            if (request.mutations().getFirst()
                    instanceof systems.zlink.framework.locationprovider.ZLinkStoreDelete) {
                return inner.write(request, cancellation)
                        .thenApply(
                                result -> {
                                    released.countDown();
                                    return result;
                                });
            }
            if (request.mutations().getFirst() instanceof ZLinkStorePut put
                    && put.key().equals(ownerKey)) renewals.incrementAndGet();
            return inner.write(request, cancellation);
        }

        @Override
        public CompletionStage<ZLinkStoreScanResult> scan(
                ZLinkStoreScanRequest request, ZLinkStoreCancellation cancellation) {
            return inner.scan(request, cancellation);
        }

        void commitClaim() {
            inner.write(claimRequest, () -> false)
                    .whenComplete(
                            (result, failure) -> {
                                if (failure == null) claim.complete(result);
                                else claim.completeExceptionally(failure);
                            });
        }
    }

    private static final class RecoveringLocationStore implements ZLinkLocationStore {
        private final ZLinkInMemoryProviderLocationStore inner;
        private final ZLinkProviderLocationRepository repository;
        private final AtomicBoolean available = new AtomicBoolean();
        private final AtomicInteger failedRequests = new AtomicInteger();
        private final CountDownLatch unavailableObserved = new CountDownLatch(1);

        private volatile CompletableFuture<Void> descriptorWrite;

        RecoveringLocationStore() {
            this(Clock.systemUTC(), false);
        }

        RecoveringLocationStore(Clock clock, boolean available) {
            inner = new ZLinkInMemoryProviderLocationStore(clock);
            repository = new ZLinkProviderLocationRepository(inner);
            this.available.set(available);
        }

        CompletableFuture<Void> reopenAndObserveDescriptorWrite() {
            var completion = new CompletableFuture<Void>();
            descriptorWrite = completion;
            available.set(true);
            return completion;
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
            var completion = descriptorWrite;
            var write = inner.write(request, cancellation);
            if (completion == null
                    || !(request.mutations().getFirst() instanceof ZLinkStorePut put)
                    || !put.key().value().startsWith("mesh-node\0")) {
                return write;
            }
            return write.thenApply(
                    result -> {
                        if (result instanceof ZLinkStoreWriteApplied) completion.complete(null);
                        return result;
                    });
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
        private static ProbeSpot last;
        private final ZLinkSpotContext context;
        private final CountDownLatch firstTick = new CountDownLatch(1);
        private final AtomicInteger timerTicks = new AtomicInteger();
        private ZLinkTimer timer;

        public ProbeSpot(ZLinkSpotContext context) {
            this.context = context;
            last = this;
        }

        static void reset() {
            messages.set(0);
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
                    .thenAccept(timer -> this.timer = timer);
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
            spot.timerTicks.incrementAndGet();
            spot.firstTick.countDown();
            return CompletableFuture.completedFuture(null);
        }
    }

    public record ProbeActor(ZLinkActorContext context) implements ZLinkActor {}

    public static final class ProbeActorFactory implements ZLinkActorFactory {
        private static final AtomicInteger invocations = new AtomicInteger();
        private static CompletableFuture<Void> release;
        private static CountDownLatch started;

        static void reset() {
            invocations.set(0);
            started = new CountDownLatch(1);
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
            started.countDown();
            return release.thenApply(ignored -> new ProbeActor(context));
        }
    }

    /** An Entry Spot that accepts every Actor type, so the node can create new Actors. */
    public static final class ActorCreationEntry
            implements systems.zlink.framework.spots.ZLinkEntrySpot<
                    systems.zlink.framework.actors.ZLinkActor> {
        private final systems.zlink.framework.spots.ZLinkEntrySpotContext context;

        public ActorCreationEntry(systems.zlink.framework.spots.ZLinkEntrySpotContext context) {
            this.context = context;
        }

        @Override
        public systems.zlink.framework.spots.ZLinkEntrySpotContext context() {
            return context;
        }

        @Override
        public java.util.concurrent.CompletionStage<Void> onJoinedActor(
                systems.zlink.framework.actors.ZLinkActor actor) {
            return java.util.concurrent.CompletableFuture.completedFuture(null);
        }

        @Override
        public java.util.concurrent.CompletionStage<Void> onLeaveActor(
                systems.zlink.framework.actors.ZLinkActor actor) {
            return java.util.concurrent.CompletableFuture.completedFuture(null);
        }
    }
}
