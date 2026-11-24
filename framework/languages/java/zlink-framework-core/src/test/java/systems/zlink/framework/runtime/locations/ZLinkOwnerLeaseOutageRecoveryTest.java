package systems.zlink.framework.runtime.locations;

import static org.junit.jupiter.api.Assertions.*;

import org.junit.jupiter.api.RepeatedTest;
import org.junit.jupiter.api.Test;
import org.junit.jupiter.params.ParameterizedTest;
import org.junit.jupiter.params.provider.CsvSource;
import org.junit.jupiter.params.provider.ValueSource;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.actors.ZLinkActor;
import systems.zlink.framework.actors.ZLinkActorContext;
import systems.zlink.framework.actors.ZLinkActorFactory;
import systems.zlink.framework.configuration.ZLinkMessageFlowLogMode;
import systems.zlink.framework.errors.ZLinkFrameworkErrorKind;
import systems.zlink.framework.errors.ZLinkFrameworkException;
import systems.zlink.framework.execution.ZLinkExecutionLanePolicy;
import systems.zlink.framework.execution.ZLinkSerialExecutionQueue;
import systems.zlink.framework.locations.ZLinkPlacementObjectKind;
import systems.zlink.framework.monitoring.ZLinkListenerKind;
import systems.zlink.framework.runtime.InMemoryRelocationStore;
import systems.zlink.framework.runtime.binding.ZLinkJavaBackendAdapterFactory;
import systems.zlink.framework.runtime.configuration.DefaultZLinkFrameworkOptions;
import systems.zlink.framework.runtime.host.ZLinkFrameworkRuntime;
import systems.zlink.framework.runtime.host.ZLinkFrameworkRuntimeTestAccess;
import systems.zlink.framework.runtime.internal.backend.ZLinkRequestFailureMapping;
import systems.zlink.framework.runtime.internal.binding.spot.MeshPeerState;
import systems.zlink.framework.runtime.internal.execution.ZLinkSpotCloseCallAccess;
import systems.zlink.framework.runtime.internal.execution.ZLinkStateLane;
import systems.zlink.framework.runtime.internal.handlers.ZLinkHandlerActivator;
import systems.zlink.framework.runtime.internal.locations.*;
import systems.zlink.framework.runtime.spots.ZLinkSpotRuntimeTestAccess;
import systems.zlink.framework.spots.ZLinkEntrySpot;
import systems.zlink.framework.spots.ZLinkEntrySpotContext;
import systems.zlink.framework.spots.ZLinkInstanceSpot;
import systems.zlink.framework.spots.ZLinkInstanceSpotContext;
import systems.zlink.framework.spots.ZLinkSpotRequestHandler;
import systems.zlink.framework.spots.ZLinkTimerOptions;
import systems.zlink.framework.spots.ZLinkTimerOverrunPolicy;
import systems.zlink.framework.spots.ZLinkTimerTick;
import systems.zlink.framework.testing.ZLinkLocationStoreTestAdapter;

import java.lang.reflect.InvocationTargetException;
import java.lang.reflect.Proxy;
import java.time.*;
import java.util.concurrent.*;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.concurrent.atomic.AtomicLong;

final class ZLinkOwnerLeaseOutageRecoveryTest {
    private static final Duration TTL = Duration.ofSeconds(30);
    private static final Duration MARGIN = Duration.ofSeconds(5);
    private static final long RECLAIM_AFTER = TTL.plus(MARGIN).toNanos();
    private static final String OUTAGE_MESH = "lease-outage";

    @ParameterizedTest
    @ValueSource(booleans = {true, false})
    void hostGatedTimerPreservesDueTickWithoutFailureOrImmediateReschedule(boolean stopOnError)
            throws Exception {
        try (var f = new OutageHost(true)) {
            var next = f.timerScheduler.nextTask();
            try (var timer =
                    get(
                            f.entry
                                    .context()
                                    .addTimer(
                                            "outage",
                                            Duration.ofNanos(1),
                                            EntryTick.class,
                                            new ZLinkTimerOptions(
                                                    ZLinkTimerOverrunPolicy.SKIP_LATE_TICKS,
                                                    1,
                                                    stopOnError)))) {
                var healthyTick = get(next);
                var outageTick = f.timerScheduler.nextTask();
                healthyTick.run();
                get(outageTick);
                assertEquals(1, f.entry.ticks.get(), "healthy control");

                f.closeLeaseAdmission();
                var retry = f.timerScheduler.nextTask();
                get(outageTick).run();
                assertAll(
                        () -> assertEquals(1, f.entry.ticks.get()),
                        () ->
                                assertFalse(
                                        timer.isDisposed(), "a lease refusal is no handler error"),
                        () ->
                                assertFalse(
                                        retry.isDone(),
                                        () ->
                                                "rescheduled during outage with delay "
                                                        + retry.join()
                                                                .getDelay(TimeUnit.NANOSECONDS)
                                                        + " ns"));
                f.store.renew = null;
                assertTrue(get(f.locations.renewOwnerLeaseOnce()));
                get(retry);
                assertEquals(2, f.entry.ticks.get(), "renewal delivers the pending tick");
            }
        }
    }

    @Test
    void closedLeaseRejectsStandaloneActorBeforeQueueAdmission() throws Exception {
        try (var f = new OutageHost(true)) {
            f.startOutage();
            get(f.host.actorManager().create("standalone", "player").submit());
            var actors = ZLinkFrameworkRuntimeTestAccess.actors(f.host);
            var actor =
                    get(actors.getOrCreateLocalActor("standalone", OutageActor.class))
                            .orElseThrow();
            assertTrue(actor.context().spotId().isEmpty());
            actors.setActorDispatchTargetResolver(null);
            var calls = new AtomicInteger();
            java.util.function.Supplier<CompletionStage<Void>> callback =
                    () -> {
                        calls.incrementAndGet();
                        return CompletableFuture.completedFuture(null);
                    };
            get(actors.submitActorDispatch("standalone", 0, callback));
            assertEquals(1, calls.get(), "healthy control");

            f.expireAdmission();
            var admitted = new CompletableFuture<Void>();
            var completion =
                    actors.submitActorDispatch(
                            null, "standalone", 0, callback, admitted, null, null);
            assertAll(
                    () -> {
                        var rejection = assertThrows(ExecutionException.class, () -> get(admitted));
                        assertEquals(
                                ZLinkFrameworkErrorKind.SHUTTING_DOWN,
                                assertInstanceOf(
                                                ZLinkFrameworkException.class, rejection.getCause())
                                        .kind());
                    },
                    () -> assertThrows(ExecutionException.class, () -> get(completion)),
                    () -> assertEquals(1, calls.get()));
        }
    }

    @Test
    void remoteInstanceRequestReportsOwnerLeaseAdmissionClosed() throws Exception {
        try (var f = new OutageHost(false)) {
            String endpoint =
                    f.host.listenerStatus(ZLinkListenerKind.ROUTE_MESH, OUTAGE_MESH).endpoint();
            var options = outageOptions(f.backing);
            var mesh = options.addRouteMesh(OUTAGE_MESH).listen("tcp://127.0.0.1:0");
            mesh.peerConnections().connect(endpoint);
            mesh.objects().client();
            try (var client = ZLinkFrameworkRuntimeTestAccess.start(options)) {
                get(ZLinkFrameworkRuntimeTestAccess.startupCompletion(client));
                assertTrue(client.isReady());
                var spots = ZLinkFrameworkRuntimeTestAccess.spots(client);
                var node = ZLinkSpotRuntimeTestAccess.routeMeshNodesByName(spots).get(OUTAGE_MESH);
                long until = System.nanoTime() + TimeUnit.SECONDS.toNanos(5);
                while (node.peers().stream()
                        .noneMatch(peer -> peer.state() == MeshPeerState.ADMITTED)) {
                    assertTrue(
                            System.nanoTime() < until, "the target peer did not become admitted");
                    Thread.sleep(10);
                }
                assertEquals("reply", get(requestInstance(client, "healthy")));
                f.closeLeaseAdmission();
                var rejection =
                        assertThrows(
                                ExecutionException.class,
                                () -> get(requestInstance(client, "closed")));
                var failure = assertInstanceOf(ZLinkFrameworkException.class, rejection.getCause());
                assertAll(
                        () ->
                                assertEquals(
                                        ZLinkFrameworkErrorKind.SHUTTING_DOWN,
                                        failure.kind(),
                                        failure::getMessage),
                        () ->
                                assertEquals(
                                        0,
                                        ZLinkRequestFailureMapping.causeCode(failure),
                                        failure::getMessage),
                        () -> assertEquals(1, f.observers.instanceCalls.get()));
            }
        }
    }

    private static CompletionStage<String> requestInstance(ZLinkFrameworkRuntime host, String id) {
        return host.route()
                .requestToSpot(id, new InstanceProbe())
                .instanceSpot("room")
                .inMesh(OUTAGE_MESH)
                .timeout(Duration.ofSeconds(5))
                .submit(String.class);
    }

    private static <T> T get(CompletionStage<T> stage) throws Exception {
        return stage.toCompletableFuture().get(8, TimeUnit.SECONDS);
    }

    private static DefaultZLinkFrameworkOptions outageOptions(ZLinkInMemoryLocationStore backing) {
        var options = new DefaultZLinkFrameworkOptions();
        options.addLocationStore(backing);
        options.addRelocationStore(new InMemoryRelocationStore());
        options.configureLocations().setStoreOutageRetention(true);
        options.configureDispatch().messageFlow(ZLinkMessageFlowLogMode.NORMAL);
        return options;
    }

    private static final class OutageHost implements AutoCloseable {
        final ZLinkInMemoryLocationStore backing = new ZLinkInMemoryLocationStore();
        final AtomicLong now = new AtomicLong(1);
        final ZLinkLocationRepository provider =
                new ZLinkProviderLocationRepository(backing, now::get, MARGIN);
        final LeaseStore store = new LeaseStore(provider);
        final ZLinkLocationFirstHeartbeatTest.RecordingScheduler timerScheduler =
                new ZLinkLocationFirstHeartbeatTest.RecordingScheduler();
        final OutageObservers observers = new OutageObservers();
        final ZLinkLocationRuntime locations;
        final ZLinkFrameworkRuntime host;
        final OutageEntry entry;

        OutageHost(boolean withEntrySpot) throws Exception {
            var options = outageOptions(backing);
            var objects =
                    options.addRouteMesh(OUTAGE_MESH)
                            .listen("tcp://127.0.0.1:0")
                            .objects()
                            .server();
            if (withEntrySpot) objects.addEntrySpot(OutageEntry.class);
            objects.addActorFactory(
                    "player",
                    OutageActor.class,
                    OutageActorFactory.class,
                    factory -> factory.disableRelocation());
            objects.addInstanceSpotFactory(
                    "room", OutageInstance.class, factory -> factory.disableRelocation());
            var heartbeat = new ZLinkLocationFirstHeartbeatTest.RecordingScheduler();
            var repository =
                    (ZLinkLocationRepository)
                            Proxy.newProxyInstance(
                                    ZLinkLocationRepository.class.getClassLoader(),
                                    new Class<?>[] {ZLinkLocationRepository.class},
                                    (proxy, method, arguments) -> {
                                        try {
                                            return method.invoke(
                                                    method.getName().equals("renewOwnerLease")
                                                            ? store
                                                            : provider,
                                                    arguments);
                                        } catch (InvocationTargetException failure) {
                                            throw failure.getCause();
                                        }
                                    });
            var registered = ZLinkRegisteredLocationStores.fromUnified(repository);
            locations =
                    new ZLinkLocationRuntime(
                            registered,
                            "lease-outage-owner",
                            TTL,
                            Duration.ofSeconds(5),
                            Duration.ofSeconds(3),
                            MARGIN,
                            now::get,
                            heartbeat,
                            new ZLinkStateLane(Runnable::run));
            host =
                    ZLinkFrameworkRuntimeTestAccess.startWithConstruction(
                            options,
                            new ZLinkJavaBackendAdapterFactory(),
                            ZLinkHandlerActivator.services().add(OutageObservers.class, observers),
                            null,
                            new ZLinkFrameworkRuntimeTestAccess.Construction()
                                    .locationRuntime(locations)
                                    .locationStores(registered)
                                    .spotTimerExecutor(timerScheduler)
                                    .spotHandlerExecutor(Runnable::run)
                                    .spotTimerStateLaneFactory(
                                            () -> new ZLinkStateLane(Runnable::run)));
            try {
                get(ZLinkFrameworkRuntimeTestAccess.startupCompletion(host));
                assertTrue(host.isReady());
                entry = withEntrySpot ? get(observers.entryReady) : null;
            } catch (Exception | Error failure) {
                close();
                throw failure;
            }
        }

        void closeLeaseAdmission() throws Exception {
            startOutage();
            expireAdmission();
        }

        void startOutage() throws Exception {
            var lastSuccess = locations.ownerLeaseSnapshot().lastSuccessfulRequestStartedNanos();
            now.set(lastSuccess + Duration.ofSeconds(5).toNanos());
            store.renew =
                    CompletableFuture.failedFuture(new IllegalStateException("Store unavailable"));
            assertFalse(get(locations.renewOwnerLeaseOnce()));
            assertNotNull(locations.ownerLeaseSnapshot().outageObservedAtNanos());
        }

        void expireAdmission() {
            now.set(locations.ownerAdmissionDeadlineNanos + 1);
            assertFalse(locations.isOwnerAdmissionOpen());
            assertFalse(locations.ownerLeaseSnapshot().outageDiscarded());
        }

        public void close() throws Exception {
            try {
                if (store.renew != null) {
                    store.renew = null;
                    assertTrue(
                            get(locations.renewOwnerLeaseOnce()), "restore the lease for shutdown");
                }
            } finally {
                try {
                    host.close();
                } finally {
                    timerScheduler.shutdownNow();
                }
            }
        }
    }

    public record OutageActor(ZLinkActorContext context) implements ZLinkActor {}

    public static final class OutageActorFactory implements ZLinkActorFactory {
        public CompletionStage<ZLinkActor> create(ZLinkActorContext context) {
            return CompletableFuture.completedFuture(new OutageActor(context));
        }
    }

    public static final class OutageObservers {
        final CompletableFuture<OutageEntry> entryReady = new CompletableFuture<>();
        final AtomicInteger instanceCalls = new AtomicInteger();
    }

    public static final class OutageEntry implements ZLinkEntrySpot<OutageActor> {
        private final ZLinkEntrySpotContext context;
        final AtomicInteger ticks = new AtomicInteger();

        public OutageEntry(ZLinkEntrySpotContext context, OutageObservers observers) {
            this.context = context;
            observers.entryReady.complete(this);
        }

        public ZLinkEntrySpotContext context() {
            return context;
        }

        public CompletionStage<Void> onJoinedActor(OutageActor actor) {
            return CompletableFuture.completedFuture(null);
        }

        public CompletionStage<Void> onLeaveActor(OutageActor actor) {
            return CompletableFuture.completedFuture(null);
        }
    }

    public static final class EntryTick {
        public void handle(OutageEntry spot, ZLinkTimerTick tick) {
            spot.ticks.incrementAndGet();
        }
    }

    public record InstanceProbe() {}

    public record OutageInstance(ZLinkInstanceSpotContext context) implements ZLinkInstanceSpot {
        public void configure() {
            context.handlers().addPacket(InstanceHandler.class);
        }
    }

    public static final class InstanceHandler
            implements ZLinkSpotRequestHandler<OutageInstance, InstanceProbe, String> {
        private final AtomicInteger calls;

        public InstanceHandler(OutageObservers observers) {
            this.calls = observers.instanceCalls;
        }

        public CompletionStage<String> handle(OutageInstance spot, InstanceProbe probe) {
            calls.incrementAndGet();
            return CompletableFuture.completedFuture("reply");
        }
    }

    static final class MutableClock extends Clock {
        Instant now = Instant.EPOCH;

        public ZoneId getZone() {
            return ZoneOffset.UTC;
        }

        public Clock withZone(ZoneId zone) {
            return this;
        }

        public Instant instant() {
            return now;
        }
    }

    static final class LeaseStore extends ZLinkLocationStoreTestAdapter {
        final ZLinkLocationRepository backing;
        CompletionStage<ZLinkOwnerLeaseRenewResult> renew;
        CompletionStage<ZLinkOwnerLeaseReadResult> exact;
        CompletionStage<ZLinkOwnerLeaseClaimResult> claim;
        Runnable onClaim;
        int claims;
        int reads;

        LeaseStore(ZLinkLocationRepository backing) {
            this.backing = backing;
        }

        public CompletionStage<ZLinkOwnerLeaseClaimResult> claimOwnerLease(
                String id, Duration ttl) {
            claims++;
            if (onClaim != null) onClaim.run();
            return claim == null ? backing.claimOwnerLease(id, ttl) : claim;
        }

        public CompletionStage<ZLinkOwnerLeaseRenewResult> renewOwnerLease(
                ZLinkLocationOwnerToken token, Duration ttl) {
            return renew == null ? backing.renewOwnerLease(token, ttl) : renew;
        }

        public CompletionStage<ZLinkOwnerLeaseReadResult> readOwnerLease(String id) {
            reads++;
            return exact == null ? backing.readOwnerLease(id) : exact;
        }

        public CompletionStage<Long> removeAllByOwner(ZLinkLocationOwnerToken token) {
            return backing.removeAllByOwner(token);
        }

        public CompletionStage<ZLinkOwnerLeaseReleaseResult> releaseOwnerLease(
                ZLinkLocationOwnerToken token) {
            return backing.releaseOwnerLease(token);
        }
    }

    static final class Fixture implements AutoCloseable {
        final MutableClock clock = new MutableClock();
        final AtomicLong now = new AtomicLong();
        final AtomicLong clockReads = new AtomicLong();
        final ZLinkLocationFirstHeartbeatTest.RecordingScheduler scheduler =
                new ZLinkLocationFirstHeartbeatTest.RecordingScheduler();
        final LeaseStore store;
        final ZLinkLocationRuntime runtime;
        final AtomicInteger disposed = new AtomicInteger();
        final AtomicInteger collected = new AtomicInteger();
        final ZLinkLocationRuntime.Execution execution;
        final ZLinkLocationOwnerToken original;

        Fixture(boolean provider, boolean retention) {
            store =
                    new LeaseStore(
                            provider
                                    ? new ZLinkProviderLocationRepository(
                                            new ZLinkInMemoryProviderLocationStore(clock))
                                    : new ZLinkInMemoryLocationStore(clock));
            runtime =
                    new ZLinkLocationRuntime(
                            ZLinkRegisteredLocationStores.fromUnified(store),
                            "owner",
                            TTL,
                            Duration.ofSeconds(5),
                            Duration.ofSeconds(3),
                            MARGIN,
                            () -> {
                                clockReads.incrementAndGet();
                                return now.get();
                            },
                            scheduler,
                            new ZLinkStateLane(Runnable::run));
            runtime.setStoreOutageRetention(retention);
            await(runtime.start(RoutingId.from("node")));
            original = runtime.currentOwnerToken();
            execution =
                    runtime.execution(
                            () -> {
                                collected.incrementAndGet();
                                return true;
                            },
                            disposed::incrementAndGet);
            await(execution.ready());
        }

        void at(long nanos) {
            now.set(nanos);
            clock.now = Instant.EPOCH.plusNanos(nanos);
        }

        void outage() {
            store.renew =
                    CompletableFuture.failedFuture(new IllegalStateException("Store unavailable"));
            assertFalse(await(runtime.renewOwnerLeaseOnce()));
            store.renew = CompletableFuture.completedFuture(new ZLinkOwnerLeaseRenewStale());
        }

        long cutoff() {
            return runtime.ownerLeaseSnapshot().outageCutoffNanos();
        }

        public void close() {
            runtime.close();
        }
    }

    @ParameterizedTest
    @CsvSource({"false,false", "false,true", "true,false", "true,true"})
    void discardedEpisodeDoesNotBlockSecondOutage(boolean provider, boolean retention) {
        try (var f = new Fixture(provider, retention)) {
            f.outage();
            long firstCutoff = f.cutoff();
            f.at(firstCutoff);
            for (var task : java.util.List.copyOf(f.scheduler.tasks))
                if (!task.isCancelled()
                        && task.getDelay(TimeUnit.NANOSECONDS) > Duration.ofSeconds(5).toNanos())
                    task.run();
            assertTrue(f.runtime.ownerLeaseSnapshot().outageDiscarded());
            assertEquals(1, f.disposed.get());
            assertStale(CompletableFuture.runAsync(() -> f.execution.check(true), Runnable::run));

            f.at(Math.max(firstCutoff, RECLAIM_AFTER) + 1);
            assertTrue(
                    await(f.runtime.renewOwnerLeaseOnce()),
                    "provider="
                            + provider
                            + ", retention="
                            + retention
                            + ", snapshot="
                            + f.runtime.ownerLeaseSnapshot());
            assertNotEquals(f.original, f.runtime.currentOwnerToken());
            var disposed = new AtomicInteger();
            var second = f.runtime.execution(() -> true, disposed::incrementAndGet);
            await(second.ready());
            second.check(true);
            long basis = f.runtime.ownerLeaseSnapshot().lastSuccessfulRequestStartedNanos();
            f.at(f.now.get() + 1);
            f.outage();
            long secondCutoff = basis + Duration.ofSeconds(retention ? 40 : 25).toNanos();
            assertEquals(f.now.get(), f.runtime.ownerLeaseSnapshot().outageObservedAtNanos());
            assertEquals(secondCutoff, f.cutoff());
            assertFalse(f.runtime.ownerLeaseSnapshot().outageDiscarded());
            f.at(secondCutoff - 1);
            second.check(true);
            assertEquals(0, disposed.get());
            assertStale(CompletableFuture.runAsync(() -> f.execution.check(true), Runnable::run));
            f.at(secondCutoff);
            for (var task : java.util.List.copyOf(f.scheduler.tasks))
                if (!task.isCancelled()
                        && task.getDelay(TimeUnit.NANOSECONDS) > Duration.ofSeconds(5).toNanos())
                    task.run();
            assertTrue(f.runtime.ownerLeaseSnapshot().outageDiscarded());
            assertEquals(1, disposed.get());
            assertStale(CompletableFuture.runAsync(() -> second.check(true), Runnable::run));
            assertStale(CompletableFuture.runAsync(() -> f.execution.check(true), Runnable::run));
        }
    }

    private static <T> T await(CompletionStage<T> stage) {
        return stage.toCompletableFuture().join();
    }

    @ParameterizedTest
    @CsvSource({"false,false", "false,true", "true,false", "true,true"})
    void deadlineLatchHoldsAcceptedWorkUntilRenewal(boolean provider, boolean actor)
            throws Exception {
        try (var f = new Fixture(provider, true)) {
            var scheduled = new LinkedBlockingQueue<Runnable>();
            var queue =
                    new ZLinkSerialExecutionQueue(
                            scheduled::add,
                            actor
                                    ? ZLinkExecutionLanePolicy.actorDelivery()
                                    : ZLinkExecutionLanePolicy.spot());
            ZLinkSpotCloseCallAccess.executionPolicy(queue, f.execution);
            var timer = ZLinkSerialExecutionQueue.spotTimer(scheduled::add, queue);
            f.execution.onResume(
                    () -> {
                        queue.resumePending();
                        timer.resumePending();
                    });
            var timerCalls = new AtomicInteger();
            var queuedTick =
                    timer.enqueue(
                                    () -> {
                                        timerCalls.incrementAndGet();
                                        return CompletableFuture.completedFuture(null);
                                    },
                                    null)
                            .toCompletableFuture();
            var calls = new AtomicInteger();
            var accepted =
                    queue.enqueue(
                                    () -> {
                                        calls.incrementAndGet();
                                        return CompletableFuture.completedFuture(null);
                                    },
                                    null)
                            .toCompletableFuture();
            f.at(Duration.ofSeconds(25).toNanos());
            for (var task : java.util.List.copyOf(f.scheduler.tasks))
                if (!task.isCancelled()
                        && task.getDelay(TimeUnit.NANOSECONDS) == Duration.ofSeconds(25).toNanos())
                    task.run();
            assertTrue(f.execution.paused(), "the deadline latch temporarily closes execution");
            while (!scheduled.isEmpty()) scheduled.remove().run();
            assertFalse(
                    accepted.isDone(), "accepted work waits while the deadline latch is closed");
            assertEquals(0, calls.get());
            assertFalse(queuedTick.isDone());
            assertEquals(0, timerCalls.get());
            var rejected =
                    assertThrows(
                            ZLinkFrameworkException.class, f.runtime::ensureOwnerAdmissionOpen);
            assertEquals(ZLinkFrameworkErrorKind.SHUTTING_DOWN, rejected.kind());
            assertTrue(await(f.runtime.renewOwnerLeaseOnce()));
            assertFalse(f.execution.paused(), "renewal reopens the execution gate");
            var completed = CompletableFuture.allOf(accepted, queuedTick);
            while (!completed.isDone()) {
                var task = scheduled.poll(2, TimeUnit.SECONDS);
                if (task == null) assertTrue(completed.isDone(), "renewal wakes accepted work");
                else task.run();
            }
            completed.get(2, TimeUnit.SECONDS);
            await(accepted);
            assertEquals(1, calls.get());
            assertTrue(queuedTick.isDone());
            await(queuedTick);
            assertEquals(1, timerCalls.get());
            timer.close();
            queue.close();
        }
    }

    @RepeatedTest(100)
    void staleDuringOutageNeedsLocalExpiryAndExactEvidence() {
        for (boolean provider : new boolean[] {false, true}) {
            for (boolean retention : new boolean[] {false, true}) {
                try (var f = new Fixture(provider, retention)) {
                    f.outage();
                    long cutoff = f.cutoff();
                    assertFalse(
                            await(f.runtime.renewOwnerLeaseOnce()),
                            "live token cannot be replaced on stale alone");
                    assertEquals(1, f.store.claims, "only the startup claim");
                    assertEquals(f.original, f.runtime.currentOwnerToken());
                    assertEquals(0, f.disposed.get());
                    f.execution.check(true);
                    f.at(RECLAIM_AFTER - 1);
                    f.store.exact = CompletableFuture.completedFuture(new ZLinkOwnerLeaseMissing());
                    assertFalse(
                            await(f.runtime.renewOwnerLeaseOnce()),
                            "Missing alone cannot shorten the monotonic wait");
                    assertEquals(1, f.store.claims);
                    f.at(RECLAIM_AFTER);
                    f.store.exact =
                            CompletableFuture.completedFuture(
                                    new ZLinkOwnerLeaseFound(
                                            f.original, f.clock.now.plus(TTL), f.clock.now));
                    assertFalse(
                            await(f.runtime.renewOwnerLeaseOnce()),
                            "monotonic expiry alone cannot replace a live exact lease");
                    assertEquals(1, f.store.claims);
                    f.store.exact = null;
                    assertTrue(await(f.runtime.renewOwnerLeaseOnce()));
                    assertEquals(2, f.store.claims);
                    assertNotEquals(f.original, f.runtime.currentOwnerToken());
                    assertEquals(cutoff, f.cutoff(), "new claim preserves the original T");
                }
                try (var f = new Fixture(provider, retention)) {
                    f.outage();
                    f.at(RECLAIM_AFTER);
                    f.store.exact =
                            CompletableFuture.completedFuture(
                                    new ZLinkOwnerLeaseFound(f.original, f.clock.now, f.clock.now));
                    assertTrue(
                            await(f.runtime.renewOwnerLeaseOnce()),
                            "exact expiration at StoreNow also allows claim");
                    assertEquals(2, f.store.claims);
                }
            }
        }
    }

    @RepeatedTest(100)
    void definitiveForeignTokenDiscardsButReadFailureDoesNot() {
        for (boolean provider : new boolean[] {false, true}) {
            try (var f = new Fixture(provider, true)) {
                f.outage();
                long cutoff = f.cutoff();
                f.at(RECLAIM_AFTER);
                f.store.exact =
                        CompletableFuture.failedFuture(
                                new IllegalStateException("exact read unavailable"));
                assertFalse(await(f.runtime.renewOwnerLeaseOnce()));
                assertEquals(0, f.disposed.get());
                assertEquals(1, f.store.claims);
                f.store.exact =
                        CompletableFuture.completedFuture(
                                new ZLinkOwnerLeaseFound(
                                        new ZLinkLocationOwnerToken(
                                                "owner", f.original.leaseGeneration() + 1),
                                        f.clock.now.plus(TTL),
                                        f.clock.now));
                assertFalse(await(f.runtime.renewOwnerLeaseOnce()));
                assertEquals(1, f.disposed.get());
                assertTrue(f.runtime.ownerLeaseSnapshot().outageDiscarded());
                assertEquals(cutoff, f.cutoff());
                assertEquals(
                        1, f.store.claims, "foreign token must not enter claim/conflict adoption");
            }
        }
    }

    @RepeatedTest(100)
    void claimConflictCannotAdoptAnotherIncarnationDuringEpisode() {
        for (boolean provider : new boolean[] {false, true}) {
            try (var f = new Fixture(provider, true)) {
                f.outage();
                f.at(RECLAIM_AFTER);
                f.store.exact = CompletableFuture.completedFuture(new ZLinkOwnerLeaseMissing());
                f.store.claim =
                        CompletableFuture.completedFuture(new ZLinkOwnerLeaseClaimConflict());
                f.store.onClaim =
                        () ->
                                f.store.exact =
                                        CompletableFuture.completedFuture(
                                                new ZLinkOwnerLeaseFound(
                                                        new ZLinkLocationOwnerToken(
                                                                "owner",
                                                                f.original.leaseGeneration() + 1),
                                                        f.clock.now.plus(TTL),
                                                        f.clock.now));
                assertFalse(await(f.runtime.renewOwnerLeaseOnce()));
                assertEquals(f.original, f.runtime.ownerLeaseSnapshot().token());
                assertEquals(1, f.disposed.get());
                assertTrue(f.runtime.ownerLeaseSnapshot().outageDiscarded());
                assertEquals(
                        2,
                        f.store.claims,
                        "one recovery attempt followed by a definitive conflict");
            }
        }
    }

    @RepeatedTest(100)
    void exactReadCannotOvertakeSuccessfulRenewalOrShutdown() {
        for (boolean provider : new boolean[] {false, true}) {
            try (var f = new Fixture(provider, true)) {
                f.outage();
                f.at(RECLAIM_AFTER);
                var exact = new CompletableFuture<ZLinkOwnerLeaseReadResult>();
                f.store.exact = exact;
                var stale = f.runtime.renewOwnerLeaseOnce();
                assertFalse(stale.toCompletableFuture().isDone());
                // A state-lane diagnostic turn completes while Store IO is pending.
                assertEquals(f.original, f.runtime.ownerLeaseSnapshot().token());
                f.store.renew =
                        CompletableFuture.completedFuture(
                                new ZLinkOwnerLeaseRenewed(f.clock.now.plus(TTL), f.clock.now));
                assertTrue(await(f.runtime.renewOwnerLeaseOnce()));
                exact.complete(new ZLinkOwnerLeaseMissing());
                assertFalse(await(stale));
                assertEquals(1, f.store.claims);
                assertEquals(f.original, f.runtime.currentOwnerToken());
                assertEquals(0, f.disposed.get());
            }
            try (var f = new Fixture(provider, true)) {
                f.outage();
                f.at(RECLAIM_AFTER);
                var exact = new CompletableFuture<ZLinkOwnerLeaseReadResult>();
                f.store.exact = exact;
                var stale = f.runtime.renewOwnerLeaseOnce();
                f.runtime.close();
                exact.complete(new ZLinkOwnerLeaseMissing());
                assertFalse(await(stale));
                assertEquals(1, f.store.claims);
            }
        }
    }

    @RepeatedTest(100)
    void exactReadAndClaimShareTheExistingRenewTimeout() {
        for (boolean provider : new boolean[] {false, true}) {
            try (var f = new Fixture(provider, true)) {
                f.outage();
                f.at(RECLAIM_AFTER);
                var exact = new CompletableFuture<ZLinkOwnerLeaseReadResult>();
                f.store.exact = exact;
                var stale = f.runtime.renewOwnerLeaseOnce();
                assertFalse(stale.toCompletableFuture().isDone());
                f.at(RECLAIM_AFTER + Duration.ofSeconds(3).toNanos());
                exact.complete(new ZLinkOwnerLeaseMissing());
                assertFalse(await(stale));
                assertEquals(
                        1,
                        f.store.claims,
                        "a completed read cannot allocate a fresh claim timeout");
                assertEquals(0, f.disposed.get());
            }
        }
    }

    @RepeatedTest(100)
    void latePreOutageResponseCannotConfirmRecoveryAndOutsideEpisodeStillClaims() {
        for (boolean provider : new boolean[] {false, true}) {
            try (var f = new Fixture(provider, true)) {
                var late = new CompletableFuture<ZLinkOwnerLeaseRenewResult>();
                f.store.renew = late;
                var pending = f.runtime.renewOwnerLeaseOnce();
                f.at(1);
                f.outage();
                long cutoff = f.cutoff();
                late.complete(new ZLinkOwnerLeaseRenewed(Instant.EPOCH.plus(TTL), Instant.EPOCH));
                assertFalse(await(pending));
                assertEquals(cutoff, f.cutoff());
                assertEquals(0, f.disposed.get());
            }
            try (var f = new Fixture(provider, false)) {
                f.store.renew = CompletableFuture.completedFuture(new ZLinkOwnerLeaseRenewStale());
                f.store.onClaim = () -> assertEquals(1, f.disposed.get(), "terminal before claim");
                assertTrue(await(f.runtime.renewOwnerLeaseOnce()));
                assertEquals(2, f.store.claims, "stale outside an episode keeps immediate claim");
                assertNull(f.runtime.ownerLeaseSnapshot().outageCutoffNanos());
            }
            try (var f = new Fixture(provider, true)) {
                f.outage();
                f.at(1);
                f.store.renew = null;
                assertTrue(await(f.runtime.renewOwnerLeaseOnce()));
                assertEquals(1, f.store.claims, "live lease resumes with the same token");
                assertEquals(f.original, f.runtime.currentOwnerToken());
            }
        }
    }

    @ParameterizedTest
    @CsvSource({"false,false", "false,true", "true,false", "true,true"})
    void staleOutsideEpisodePermanentlyDiscardsQueuedWorkAndTimers(
            boolean provider, boolean retention) {
        var queue = new ZLinkSerialExecutionQueue(Runnable::run, ZLinkExecutionLanePolicy.spot());
        var timer = ZLinkSerialExecutionQueue.spotTimer(Runnable::run, queue);
        try (var f = new Fixture(provider, retention)) {
            var store = f.store.backing;
            var before =
                    ZLinkLocationDescriptorFixture.descriptor(
                            f.original, "node", ZLinkPlacementObjectKind.USER_SPOT, 1);
            assertEquals(
                    ZLinkLocationWriteStatus.STORED,
                    await(store.updateMeshNode(before, ZLinkLocationWriteIntent.NEW_CLAIM))
                            .status());
            var called = new AtomicInteger();
            var disposed = new AtomicInteger();
            var execution =
                    f.runtime.execution(
                            () -> false,
                            () -> {
                                queue.discardApplications();
                                timer.discardApplications();
                                disposed.incrementAndGet();
                            });
            await(execution.ready());
            ZLinkSpotCloseCallAccess.executionPolicy(queue, execution);
            var entered = new CompletableFuture<Void>();
            var remote = new CompletableFuture<Void>();
            var staleWrite = new CompletableFuture<ZLinkLocationWriteStatus>();
            var active =
                    queue.enqueue(
                            () -> {
                                entered.complete(null);
                                return remote.thenCompose(
                                                ignored ->
                                                        store.updateMeshNode(
                                                                before,
                                                                ZLinkLocationWriteIntent.RENEW))
                                        .thenAccept(result -> staleWrite.complete(result.status()));
                            },
                            null);
            entered.join();
            java.util.function.Supplier<CompletionStage<Void>> callback =
                    () -> {
                        called.incrementAndGet();
                        return CompletableFuture.completedFuture(null);
                    };
            var pending = queue.enqueue(callback, null);
            var timerEntered = new CompletableFuture<Void>();
            var activeTimer =
                    timer.enqueue(
                            () -> {
                                timerEntered.complete(null);
                                return remote;
                            },
                            null);
            timerEntered.join();
            var tick = timer.enqueue(callback, null);
            assertFalse(pending.toCompletableFuture().isDone());
            assertFalse(tick.toCompletableFuture().isDone());
            var publication = new CompletableFuture<Void>();
            var descriptorCommitted = new CompletableFuture<ZLinkMeshNodeDescriptor>();
            f.runtime.setOwnerLeaseRecoveryListener(
                    admissionWasOpen -> {
                        assertFalse(admissionWasOpen);
                        var next =
                                ZLinkLocationDescriptorFixture.descriptor(
                                        f.runtime.descriptorPublicationOwnerToken(),
                                        "node",
                                        ZLinkPlacementObjectKind.USER_SPOT,
                                        1,
                                        before.descriptorRevision() + 1);
                        return publication
                                .thenCompose(
                                        ignored ->
                                                store.updateMeshNode(
                                                        next, ZLinkLocationWriteIntent.RENEW))
                                .thenAccept(
                                        result -> {
                                            assertEquals(
                                                    ZLinkLocationWriteStatus.STORED,
                                                    result.status());
                                            descriptorCommitted.complete(next);
                                        });
                    });
            f.store.onClaim =
                    () -> {
                        assertEquals(1, disposed.get());
                        assertEquals(1, f.disposed.get());
                        assertStale(pending);
                        assertStale(tick);
                        assertStale(queue.enqueue(callback, null));
                        assertNull(f.runtime.ownerLeaseSnapshot().outageCutoffNanos());
                    };
            f.at(RECLAIM_AFTER);
            var renewing = f.runtime.renewOwnerLeaseOnce();
            try {
                assertFalse(renewing.toCompletableFuture().isDone(), f.runtime::lastError);
                var owner = f.runtime.descriptorPublicationOwnerToken();
                assertEquals(f.original.ownerId(), owner.ownerId());
                assertNotEquals(f.original.leaseGeneration(), owner.leaseGeneration());
                assertFalse(active.toCompletableFuture().isDone());
                assertFalse(activeTimer.toCompletableFuture().isDone());
                assertFalse(f.runtime.isOwnerAdmissionOpen());
                assertThrows(ZLinkFrameworkException.class, f.runtime::ensureOwnerAdmissionOpen);
                assertFalse(descriptorCommitted.isDone());
                publication.complete(null);
                assertTrue(await(renewing));
                assertTrue(f.runtime.isOwnerAdmissionOpen());
                f.runtime.ensureOwnerAdmissionOpen();
                var after = await(descriptorCommitted);
                assertEquals(owner.leaseGeneration(), after.leaseGeneration());
                assertTrue(after.descriptorRevision() > before.descriptorRevision());
                assertFalse(active.toCompletableFuture().isDone());
            } finally {
                publication.complete(null);
                remote.complete(null);
            }
            assertEquals(ZLinkLocationWriteStatus.IGNORED_STALE, await(staleWrite));
            assertStale(queue.enqueue(callback, null));
            assertStale(timer.enqueue(callback, null));
            await(execution.ready());
            assertStale(queue.enqueue(callback, null));
            assertEquals(0, called.get());
            assertEquals(1, disposed.get());
            var fresh = f.runtime.execution(() -> true, () -> {});
            await(fresh.ready());
            fresh.check(true);
            var freshQueue =
                    new ZLinkSerialExecutionQueue(Runnable::run, ZLinkExecutionLanePolicy.spot());
            try {
                ZLinkSpotCloseCallAccess.executionPolicy(freshQueue, fresh);
                await(freshQueue.enqueue(callback, null));
                assertEquals(1, called.get());
            } finally {
                freshQueue.close();
            }
            await(active);
            await(activeTimer);
        } finally {
            queue.close();
            timer.close();
        }
    }

    @RepeatedTest(100)
    void fiveThousandRetainedExecutionsHaveConstantChecksAndSingleCollection() {
        for (boolean provider : new boolean[] {false, true}) {
            try (var f = new Fixture(provider, true)) {
                for (int i = 1; i < 5000; i++)
                    await(
                            f.runtime
                                    .execution(
                                            () -> {
                                                f.collected.incrementAndGet();
                                                return true;
                                            },
                                            f.disposed::incrementAndGet)
                                    .ready());
                long reads = f.clockReads.get();
                for (int i = 0; i < 5000; i++) f.execution.check(true);
                assertEquals(reads, f.clockReads.get(), "ordinary execution uses no clock");
                assertEquals(0, f.collected.get());
                f.outage();
                assertEquals(5000, f.collected.get(), "one visit per collected execution");
                f.at(Duration.ofSeconds(25).toNanos());
                reads = f.clockReads.get();
                for (int i = 0; i < 5000; i++) f.execution.check(true);
                assertEquals(
                        reads + 10000,
                        f.clockReads.get(),
                        "two local deadline reads per retained check");
                assertEquals(
                        5000, f.collected.get(), "execution checks do not enumerate other owners");
                assertFalse(await(f.runtime.renewOwnerLeaseOnce()));
                assertEquals(
                        5000, f.collected.get(), "reclaim exclusion does not recollect owners");
                assertEquals(1, f.store.claims);
                assertEquals(
                        0, f.store.reads, "reclaim before local expiration submits no exact read");
            }
        }
    }

    private static void assertStale(CompletionStage<?> stage) {
        Throwable failure = assertThrows(CompletionException.class, () -> await(stage)).getCause();
        while (failure instanceof CompletionException && failure.getCause() != null)
            failure = failure.getCause();
        assertTrue(
                systems.zlink.framework.runtime.messaging.ZLinkFrameworkErrorOrigin.isFramework(
                        failure));
        assertEquals(
                ZLinkFrameworkErrorKind.NOT_FOUND,
                assertInstanceOf(ZLinkFrameworkException.class, failure).kind());
    }

    @RepeatedTest(100)
    void cutoffAndDefinitiveLossEndPendingAndResumedWorkWithStaleRoute() {
        for (boolean provider : new boolean[] {false, true}) {
            for (boolean cutoff : new boolean[] {false, true}) {
                try (var f = new Fixture(provider, true)) {
                    var queue =
                            new ZLinkSerialExecutionQueue(
                                    Runnable::run, ZLinkExecutionLanePolicy.spot());
                    var timer = ZLinkSerialExecutionQueue.spotTimer(Runnable::run, queue);
                    var execution =
                            f.runtime.execution(
                                    () -> true,
                                    () -> {
                                        queue.discardApplications();
                                        timer.discardApplications();
                                    });
                    await(execution.ready());
                    ZLinkSpotCloseCallAccess.executionPolicy(queue, execution);
                    var entered = new CompletableFuture<Void>();
                    var remote = new CompletableFuture<Void>();
                    var active =
                            queue.enqueue(
                                    () -> {
                                        entered.complete(null);
                                        return remote;
                                    },
                                    null);
                    entered.join();
                    var called = new AtomicInteger();
                    var pending =
                            queue.enqueue(
                                    () -> {
                                        called.incrementAndGet();
                                        return CompletableFuture.completedFuture(null);
                                    },
                                    null);
                    f.outage();
                    if (cutoff) {
                        f.at(f.cutoff());
                        for (var task : java.util.List.copyOf(f.scheduler.tasks))
                            if (!task.isCancelled()
                                    && task.getDelay(TimeUnit.NANOSECONDS)
                                            > Duration.ofSeconds(5).toNanos()) task.run();
                    } else execution.discard();
                    assertStale(pending);
                    assertStale(
                            queue.enqueue(
                                    () -> {
                                        called.incrementAndGet();
                                        return CompletableFuture.completedFuture(null);
                                    },
                                    null));
                    assertStale(
                            timer.enqueue(
                                    () -> {
                                        called.incrementAndGet();
                                        return CompletableFuture.completedFuture(null);
                                    },
                                    null));
                    assertEquals(0, called.get());
                    remote.complete(null);
                    await(active);
                    queue.close();
                    timer.close();
                }
                try (var f = new Fixture(provider, true)) {
                    var queue =
                            new ZLinkSerialExecutionQueue(
                                    Runnable::run, ZLinkExecutionLanePolicy.spotReturningGate());
                    var execution =
                            f.runtime.execution(() -> true, () -> queue.discardApplications());
                    await(execution.ready());
                    ZLinkSpotCloseCallAccess.executionPolicy(queue, execution);
                    var remote = new CompletableFuture<Void>();
                    var entered = new CompletableFuture<Void>();
                    var resumed = new AtomicInteger();
                    var pending =
                            queue.enqueue(
                                    () -> {
                                        entered.complete(null);
                                        return ZLinkSerialExecutionQueue.manageCurrentInTurn(remote)
                                                .thenRun(resumed::incrementAndGet);
                                    },
                                    null);
                    entered.join();
                    f.outage();
                    if (cutoff) {
                        f.at(f.cutoff());
                        for (var task : java.util.List.copyOf(f.scheduler.tasks))
                            if (!task.isCancelled()
                                    && task.getDelay(TimeUnit.NANOSECONDS)
                                            > Duration.ofSeconds(5).toNanos()) task.run();
                    } else execution.discard();
                    remote.complete(null);
                    assertStale(pending);
                    assertEquals(0, resumed.get());
                    queue.close();
                }
            }
        }
    }

    @RepeatedTest(100)
    void expiredTokenCannotBeRenewedByEitherRepository() {
        for (boolean provider : new boolean[] {false, true}) {
            try (var f = new Fixture(provider, true)) {
                f.at(TTL.toNanos() - 1);
                assertInstanceOf(
                        ZLinkOwnerLeaseRenewed.class,
                        await(f.store.backing.renewOwnerLease(f.original, TTL)));
                f.at(TTL.multipliedBy(2).toNanos() - 1);
                assertInstanceOf(
                        ZLinkOwnerLeaseRenewStale.class,
                        await(f.store.backing.renewOwnerLease(f.original, TTL)));
            }
        }
    }
}
