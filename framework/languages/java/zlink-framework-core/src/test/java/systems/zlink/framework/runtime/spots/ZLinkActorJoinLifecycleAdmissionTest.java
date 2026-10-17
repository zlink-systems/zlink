package systems.zlink.framework.runtime.spots;

import static org.junit.jupiter.api.Assertions.*;

import org.junit.jupiter.api.Test;
import org.junit.jupiter.params.ParameterizedTest;
import org.junit.jupiter.params.provider.ValueSource;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.framework.actors.*;
import systems.zlink.framework.locations.ZLinkPlacementObjectKind;
import systems.zlink.framework.messaging.ZLinkMessage;
import systems.zlink.framework.runtime.actors.ZLinkActorRuntime;
import systems.zlink.framework.runtime.configuration.DefaultZLinkFrameworkOptions;
import systems.zlink.framework.runtime.host.ZLinkFrameworkRuntime;
import systems.zlink.framework.runtime.host.ZLinkFrameworkRuntimeTestAccess;
import systems.zlink.framework.runtime.internal.backend.ZLinkInternalSpotNode;
import systems.zlink.framework.runtime.internal.dispatch.ZLinkApplicationJobQueue;
import systems.zlink.framework.runtime.internal.locations.*;
import systems.zlink.framework.runtime.internal.relocation.ZLinkActorJoinRelocationPort;
import systems.zlink.framework.runtime.locations.ZLinkInMemoryLocationStore;
import systems.zlink.framework.runtime.messaging.ZLinkJsonMessageSerializer;
import systems.zlink.framework.runtime.protocol.ServiceWirePilotCodec;
import systems.zlink.framework.spots.*;

import java.lang.reflect.Proxy;
import java.nio.charset.StandardCharsets;
import java.time.Duration;
import java.time.Instant;
import java.util.ArrayList;
import java.util.List;
import java.util.Map;
import java.util.UUID;
import java.util.concurrent.*;
import java.util.function.Function;

final class ZLinkActorJoinLifecycleAdmissionTest {
    private static final RoutingId NODE = RoutingId.from("admission-node");
    private static final Duration REQUEST_TIMEOUT = Duration.ofSeconds(1);
    static final long COMPLETION_SECONDS = 5;

    @ParameterizedTest
    @ValueSource(booleans = {false, true})
    void localAndCanonicalCallbacksShareTheWholeLifecycleTurn(boolean canonicalFirst)
            throws Exception {
        try (var fixture = new Fixture<>(Room.class)) {
            var entered = new CompletableFuture<Void>();
            var release = new CompletableFuture<ZLinkSpotActorJoinResult>();
            fixture.room.callback =
                    id -> {
                        entered.complete(null);
                        return fixture.room.calls.size() == 1
                                ? release
                                : CompletableFuture.completedFuture(
                                        ZLinkSpotActorJoinResult.accept());
                    };
            try {
                var first = canonicalFirst ? fixture.canonical(1) : fixture.local();
                entered.get(COMPLETION_SECONDS, TimeUnit.SECONDS);
                var second = canonicalFirst ? fixture.local() : fixture.canonical(2);
                fixture.drainInfrastructure();
                assertEquals(1, fixture.room.calls.size(), "second callback entered a held turn");
                assertFalse(first.toCompletableFuture().isDone());
                assertFalse(second.toCompletableFuture().isDone());
                release.complete(ZLinkSpotActorJoinResult.accept());
                assertTrue(
                        first.toCompletableFuture()
                                .get(COMPLETION_SECONDS, TimeUnit.SECONDS)
                                .accepted());
                assertTrue(
                        second.toCompletableFuture()
                                .get(COMPLETION_SECONDS, TimeUnit.SECONDS)
                                .accepted());
                assertEquals(
                        canonicalFirst
                                ? List.of("remote-1", "local")
                                : List.of("local", "remote-2"),
                        fixture.room.calls);
            } finally {
                release.complete(ZLinkSpotActorJoinResult.accept());
            }
        }
    }

    @Test
    void canonicalCallbackPropagatesRejectionAndFailure() throws Exception {
        try (var fixture = new Fixture<>(Room.class)) {
            fixture.room.callback =
                    id -> CompletableFuture.completedFuture(ZLinkSpotActorJoinResult.reject());
            assertFalse(
                    fixture.canonical(1)
                            .toCompletableFuture()
                            .get(COMPLETION_SECONDS, TimeUnit.SECONDS)
                            .accepted());
            var failure = new IllegalStateException("callback failure");
            fixture.room.callback = id -> CompletableFuture.failedFuture(failure);
            var result = fixture.canonical(2).toCompletableFuture();
            assertSame(
                    failure,
                    assertThrows(
                                    ExecutionException.class,
                                    () -> result.get(COMPLETION_SECONDS, TimeUnit.SECONDS))
                            .getCause());
        }
    }

    @Test
    void canonicalCapacityWaitDoesNotBlockInfrastructureAndPublishesInOrder() throws Exception {
        try (var fixture = new Fixture<>(Room.class)) {
            var permits = fixture.exhaustCapacity();
            var release = new CompletableFuture<ZLinkSpotActorJoinResult>();
            var entered = new CompletableFuture<Void>();
            fixture.room.callback =
                    id -> {
                        entered.complete(null);
                        return fixture.room.calls.size() == 1
                                ? release
                                : CompletableFuture.completedFuture(
                                        ZLinkSpotActorJoinResult.accept());
                    };
            try {
                // Run both submissions on the existing infrastructure queue. Its sentinel must
                // complete while all capacity is still held by this fixture.
                var submitted = new ArrayList<CompletionStage<ZLinkSpotActorJoinResult>>();
                fixture.activation
                        .context
                        .enqueueInfrastructureDispatch(
                                () -> {
                                    submitted.add(fixture.canonical(1));
                                    submitted.add(fixture.canonical(2));
                                    return CompletableFuture.completedFuture(null);
                                })
                        .toCompletableFuture()
                        .get(COMPLETION_SECONDS, TimeUnit.SECONDS);
                fixture.drainInfrastructure();
                assertEquals(
                        0, fixture.room.calls.size(), "capacity must precede user callback entry");
                assertEquals(2, fixture.jobs.snapshot().capacityWaiters());
                permits.removeFirst().close();
                permits.removeFirst().close();
                fixture.drainInfrastructure();
                entered.get(COMPLETION_SECONDS, TimeUnit.SECONDS);
                assertEquals(1, fixture.room.calls.size());
                assertFalse(submitted.get(1).toCompletableFuture().isDone());
                release.complete(ZLinkSpotActorJoinResult.accept());
                for (var result : submitted)
                    assertTrue(
                            result.toCompletableFuture()
                                    .get(COMPLETION_SECONDS, TimeUnit.SECONDS)
                                    .accepted());
                assertEquals(List.of("remote-1", "remote-2"), fixture.room.calls);
            } finally {
                release.complete(ZLinkSpotActorJoinResult.accept());
                permits.forEach(ZLinkApplicationJobQueue.Permit::close);
            }
            assertEquals(0, fixture.jobs.snapshot().reservedSupplyPermits());
            assertEquals(0, fixture.jobs.snapshot().queuedApplicationJobs());
        }
    }

    @Test
    void cancelledCapacityWaitAndHostDrainReturnEveryUnpublishedPermit() throws Exception {
        try (var fixture = new Fixture<>(Room.class)) {
            var permits = fixture.exhaustCapacity();
            try {
                var cancelled = fixture.local().toCompletableFuture();
                assertEquals(1, fixture.jobs.snapshot().capacityWaiters());
                assertTrue(cancelled.cancel(false));
                assertInstanceOf(
                        CancellationException.class,
                        assertThrows(CompletionException.class, cancelled::join).getCause());
                assertEquals(0, fixture.jobs.snapshot().capacityWaiters());
                var waiting = fixture.canonical(1).toCompletableFuture();
                assertEquals(1, fixture.jobs.snapshot().capacityWaiters());
                fixture.host
                        .beginDrain()
                        .toCompletableFuture()
                        .get(COMPLETION_SECONDS, TimeUnit.SECONDS);
                permits.forEach(ZLinkApplicationJobQueue.Permit::close);
                permits.clear();
                fixture.drainInfrastructure();
                var failure =
                        assertThrows(
                                ExecutionException.class,
                                () -> waiting.get(COMPLETION_SECONDS, TimeUnit.SECONDS));
                assertEquals(
                        systems.zlink.framework.errors.ZLinkFrameworkErrorKind.SHUTTING_DOWN,
                        assertInstanceOf(
                                        systems.zlink.framework.errors.ZLinkFrameworkException
                                                .class,
                                        failure.getCause())
                                .kind());
                assertTrue(fixture.room.calls.isEmpty());
                assertEquals(0, fixture.jobs.snapshot().capacityWaiters());
                assertEquals(0, fixture.jobs.snapshot().permitsInUse());
            } finally {
                permits.forEach(ZLinkApplicationJobQueue.Permit::close);
            }
        }
    }

    static final class Fixture<T extends ZLinkSpot<ZLinkActor>> implements AutoCloseable {
        final ZLinkFrameworkRuntime framework;
        final ZLinkSpotRuntime host;
        final SpotActivation activation;
        final T room;
        final ZLinkApplicationJobQueue jobs;

        Fixture(Class<T> roomType) throws Exception {
            var options = new DefaultZLinkFrameworkOptions();
            options.addLocationStore(new ZLinkInMemoryLocationStore());
            var node = options.addRouteMesh("join-admission");
            node.listen("inproc://join-admission-" + UUID.randomUUID()).setRoutingId(NODE);
            node.objects()
                    .server()
                    .addSpotFactory("room", roomType, factory -> factory.disableRelocation());
            framework = ZLinkFrameworkRuntimeTestAccess.start(options);
            framework
                    .spotManager()
                    .getOrCreate("room-a", "room")
                    .submit()
                    .toCompletableFuture()
                    .get(COMPLETION_SECONDS, TimeUnit.SECONDS);
            host = (ZLinkSpotRuntime) framework.spotManager();
            activation = host.spotLifecycle().spotActivationFor("room-a");
            room = roomType.cast(activation.spot());
            var jobsField = ZLinkSpotRuntime.class.getDeclaredField("applicationJobQueue");
            jobsField.setAccessible(true);
            jobs = (ZLinkApplicationJobQueue) jobsField.get(host);
            var backend =
                    (ZLinkInternalSpotNode)
                            Proxy.newProxyInstance(
                                    ZLinkInternalSpotNode.class.getClassLoader(),
                                    new Class<?>[] {ZLinkInternalSpotNode.class},
                                    (proxy, method, args) ->
                                            "routingId".equals(method.getName()) ? NODE : null);
            var actors =
                    new ZLinkActorRuntime(
                            backend,
                            Map.of("player", Factory.class),
                            REQUEST_TIMEOUT,
                            new ZLinkJsonMessageSerializer());
            var authority =
                    new ZLinkAuthoritySnapshot(
                            "v1",
                            new byte[0],
                            7L,
                            2L,
                            "owner",
                            3L,
                            new ZLinkPlacementAllocation(
                                    ZLinkPlacementAllocationState.ACTIVE,
                                    ZLinkPlacementObjectKind.ACTOR,
                                    "player",
                                    new ZLinkMeshNodeDescriptorKey("join-admission", NODE),
                                    9L,
                                    ZLinkPlacementCapacityBundle.actor(1)),
                            Instant.now());
            actors.setDirectJoinRelocationStores(
                    (ZLinkLocationRepository)
                            Proxy.newProxyInstance(
                                    ZLinkLocationRepository.class.getClassLoader(),
                                    new Class<?>[] {ZLinkLocationRepository.class},
                                    (proxy, method, args) ->
                                            "read".equals(method.getName())
                                                    ? CompletableFuture.completedFuture(authority)
                                                    : null));
            actors.setActorJoinRelocationPort(
                    new ZLinkActorJoinRelocationPort() {
                        @Override
                        public CompletionStage<Submission> relocate(Goal goal, Duration timeout) {
                            return CompletableFuture.failedFuture(
                                    new AssertionError("not relocating"));
                        }

                        @Override
                        public void admit(Admission admission) {
                            throw new AssertionError("not legacy admission");
                        }

                        @Override
                        public void admitCanonical(CanonicalAdmission admission) {}
                    });
            host.actorAdmissions().attach(actors, host::isDraining, null);
        }

        CompletionStage<ZLinkSpotActorJoinResult> local() {
            return activation.admitActorJoin(() -> room.onActorJoin("local", ZLinkMessage.empty()));
        }

        CompletionStage<ZLinkSpotActorJoinResult> canonical(long sequence) {
            var rid = NODE.toString().getBytes(StandardCharsets.UTF_8);
            var actor = new ServiceWirePilotCodec.Fence("remote-" + sequence, 7L, rid, 9L, 2L, 3L);
            var target = new ServiceWirePilotCodec.Fence("room-a", 1L, rid, 9L, 2L, 3L);
            var join = new ServiceWirePilotCodec.ActorJoin28(sequence, actor, false, target, null);
            var payload = Message.from(new byte[0]);
            return activation
                    .admitCanonicalActorJoin(
                            ZLinkSpotRuntime.canonicalActorJoinRequest(join, NODE, REQUEST_TIMEOUT),
                            NODE,
                            payload,
                            join,
                            "application/octet-stream")
                    .whenComplete((value, failure) -> payload.close());
        }

        void drainInfrastructure() throws Exception {
            activation
                    .context
                    .enqueueInfrastructureDispatch(() -> CompletableFuture.completedFuture(null))
                    .toCompletableFuture()
                    .get(COMPLETION_SECONDS, TimeUnit.SECONDS);
        }

        List<ZLinkApplicationJobQueue.Permit> exhaustCapacity() {
            var permits = new ArrayList<ZLinkApplicationJobQueue.Permit>();
            for (; ; ) {
                var claim =
                        jobs.acquire(ZLinkApplicationJobQueue.Origin.REMOTE).toCompletableFuture();
                if (!claim.isDone()) {
                    claim.cancel(false);
                    return permits;
                }
                permits.add(claim.join());
            }
        }

        @Override
        public void close() {
            framework.close();
        }
    }

    public static final class Room implements ZLinkSpot<ZLinkActor> {
        final ZLinkSpotContext context;
        final List<String> calls = new CopyOnWriteArrayList<>();
        volatile Function<String, CompletionStage<ZLinkSpotActorJoinResult>> callback =
                id -> CompletableFuture.completedFuture(ZLinkSpotActorJoinResult.accept());

        public Room(ZLinkSpotContext context) {
            this.context = context;
        }

        @Override
        public ZLinkSpotContext context() {
            return context;
        }

        @Override
        public CompletionStage<ZLinkSpotActorJoinResult> onActorJoin(
                String actorId, ZLinkMessage request) {
            calls.add(actorId);
            return callback.apply(actorId);
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

    public static final class Factory implements ZLinkActorFactory {
        @Override
        public CompletionStage<ZLinkActor> create(ZLinkActorContext context) {
            return CompletableFuture.failedFuture(new AssertionError("not instantiated"));
        }
    }
}
