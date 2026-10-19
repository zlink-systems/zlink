package systems.zlink.framework.runtime.locations;

import static org.junit.jupiter.api.Assertions.*;

import org.junit.jupiter.api.Test;
import org.junit.jupiter.params.ParameterizedTest;
import org.junit.jupiter.params.provider.ValueSource;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.actors.ZLinkActor;
import systems.zlink.framework.locationprovider.*;
import systems.zlink.framework.locations.*;
import systems.zlink.framework.runtime.configuration.DefaultZLinkFrameworkOptions;
import systems.zlink.framework.runtime.host.ZLinkFrameworkRuntime;
import systems.zlink.framework.runtime.host.ZLinkFrameworkRuntimeTestAccess;
import systems.zlink.framework.runtime.internal.locations.*;
import systems.zlink.framework.runtime.internal.metrics.ZLinkRuntimeMetricProbe;
import systems.zlink.framework.spots.ZLinkSpot;
import systems.zlink.framework.spots.ZLinkSpotContext;

import java.lang.reflect.InvocationTargetException;
import java.lang.reflect.Proxy;
import java.time.Duration;
import java.util.List;
import java.util.Map;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicInteger;

final class ZLinkCapacityMetricPollingTest {
    private static final String MESH = "capacity-poll";
    private static final Map<String, String> TAGS =
            Map.of("mesh_name", MESH, "capacity_scope", "spot");
    private static final String RESERVED = "zlink.object.capacity.reserved";
    private static final String ACTIVE = "zlink.object.capacity.active";

    @Test
    void listsAndCollectionOutsidePollingDoNotReadCapacity() throws Exception {
        try (var fixture = new Fixture(false, false)) {
            int reads = fixture.store.reads.get();
            for (int i = 0; i < 3; i++) fixture.descriptor();
            for (int i = 0; i < 3; i++) fixture.metrics.observable(RESERVED, TAGS);
            assertEquals(reads, fixture.store.reads.get());
        }
    }

    @ParameterizedTest
    @ValueSource(booleans = {false, true})
    void firstFailedReadDoesNotPublishAnUnconfirmedZero(boolean synchronous) throws Exception {
        try (var fixture = new Fixture(true, synchronous)) {
            assertFalse(fixture.metrics.hasObservable(RESERVED, TAGS));
            assertFalse(fixture.metrics.hasObservable(ACTIVE, TAGS));
            assertFalse(fixture.metrics.hasObservable("zlink.object.capacity.limit", TAGS));
            assertReadError(fixture.metrics, 1);
            fixture.store.fail = false;
            fixture.tick();
            assertTrue(fixture.metrics.hasObservable(RESERVED, TAGS));
            assertEquals(0, fixture.metrics.observable(RESERVED, TAGS));
        }
    }

    @Test
    void eachMeshNodeLoopReadsOnlyItsOwnLocalRecord() throws Exception {
        try (var fixture = new Fixture(false, false, true)) {
            assertEquals(2, fixture.loops.size());
            for (var loop : fixture.loops) {
                int reads = fixture.store.reads.get();
                fixture.tick(loop);
                assertEquals(reads + 1, fixture.store.reads.get());
            }
        }
    }

    @Test
    void remoteReserveAppearsAtNextPollAndProviderCommitAppearsImmediately() throws Exception {
        try (var fixture = new Fixture(false, false)) {
            var descriptor = fixture.descriptor();
            var otherHost = new ZLinkProviderLocationRepository(fixture.store.delegate);
            var reservation = reserve(otherHost, descriptor, "remote");
            assertEquals(0, fixture.metrics.observable(RESERVED, TAGS));
            int reads = fixture.store.reads.get();
            fixture.tick();
            assertEquals(reads + 1, fixture.store.reads.get());
            assertEquals(1, fixture.metrics.observable(RESERVED, TAGS));
            assertEquals(0, fixture.metrics.observable(ACTIVE, TAGS));
            assertEquals(
                    ZLinkObjectCommitResult.COMMITTED,
                    fixture.repository
                            .commit(reservation, new byte[] {1}, null, () -> false)
                            .toCompletableFuture()
                            .get(10, TimeUnit.SECONDS));
            assertEquals(0, fixture.metrics.observable(RESERVED, TAGS));
            assertEquals(1, fixture.metrics.observable(ACTIVE, TAGS));
        }
    }

    @ParameterizedTest
    @ValueSource(booleans = {false, true})
    void failedPollingReadKeepsLastValueAndCountsOneStoreError(boolean synchronous)
            throws Exception {
        try (var fixture = new Fixture(false, synchronous)) {
            reserve(fixture.repository, fixture.descriptor(), "local");
            assertEquals(1, fixture.metrics.observable(RESERVED, TAGS));
            fixture.store.fail = true;
            int reads = fixture.store.reads.get();
            fixture.tick();
            assertEquals(reads + 1, fixture.store.reads.get());
            assertEquals(1, fixture.metrics.observable(RESERVED, TAGS));
            assertReadError(fixture.metrics, 1);
        }
    }

    private static void assertReadError(ZLinkRuntimeMetricProbe metrics, long expected) {
        assertEquals(expected, metrics.count("zlink.location.store.errors"));
        assertEquals("read", metrics.only("zlink.location.store.errors").tags().get("operation"));
    }

    private static ZLinkObjectReservation reserve(
            ZLinkLocationRepository repository, ZLinkMeshNodeDescriptor descriptor, String name)
            throws Exception {
        return assertInstanceOf(
                        ZLinkObjectReserved.class,
                        repository
                                .reserve(
                                        new ZLinkObjectReservationRequest(
                                                ZLinkPlacementObjectKind.USER_SPOT,
                                                ZLinkAuthorityKeyCodec.spot(name),
                                                "room",
                                                "inline-v1:test",
                                                new byte[32],
                                                4,
                                                new ZLinkMeshNodeDescriptorKey(
                                                        descriptor.meshName(), descriptor.rid()),
                                                descriptor.lifecycleGeneration(),
                                                new ZLinkLocationOwnerToken(
                                                        descriptor.ownerId(),
                                                        descriptor.leaseGeneration()),
                                                new byte[] {1},
                                                ZLinkPlacementCapacityBundle.spot(
                                                        ZLinkPlacementObjectKind.USER_SPOT,
                                                        "room",
                                                        1)),
                                        () -> false)
                                .toCompletableFuture()
                                .get(10, TimeUnit.SECONDS))
                .reservation();
    }

    private static final class Fixture implements AutoCloseable {
        final CountingStore store;
        final ZLinkRuntimeMetricProbe metrics = ZLinkRuntimeMetricProbe.install();
        final ZLinkFrameworkRuntime runtime;
        final ZLinkLocationRepository repository;
        final ZLinkAutoConnectLoop loop;
        final List<ZLinkAutoConnectLoop> loops;

        Fixture(boolean fail, boolean synchronous) throws Exception {
            this(fail, synchronous, false);
        }

        @SuppressWarnings("unchecked")
        Fixture(boolean fail, boolean synchronous, boolean secondNode) throws Exception {
            store = new CountingStore(fail, synchronous);
            var options = new DefaultZLinkFrameworkOptions();
            options.addLocationStore(store.provider());
            options.addRelocationStore(
                    new systems.zlink.framework.runtime.InMemoryRelocationStore());
            options.configureLocations().setPollingInterval(Duration.ofHours(1));
            options.addRouteMesh(MESH)
                    .listen("tcp://127.0.0.1:0")
                    .setRoutingId(RoutingId.from("capacity-poll-node"))
                    .objects()
                    .server()
                    .addSpotFactory("room", Room.class, f -> f.recreateOnRelocation());
            if (secondNode)
                options.addRouteMesh("capacity-other")
                        .listen("tcp://127.0.0.1:0")
                        .setRoutingId(RoutingId.from("capacity-other-node"));
            runtime = ZLinkFrameworkRuntimeTestAccess.start(options);
            ZLinkFrameworkRuntimeTestAccess.startupCompletion(runtime)
                    .toCompletableFuture()
                    .get(10, TimeUnit.SECONDS);
            var location = (ZLinkLocationRuntime) field(runtime, "locationRuntime");
            repository = location.locationStore();
            var host = field(runtime, "locationAutoConnectHost");
            loops = (List<ZLinkAutoConnectLoop>) field(host, "loops");
            loop = loops.getFirst();
        }

        ZLinkMeshNodeDescriptor descriptor() throws Exception {
            return repository
                    .listMeshNodes(MESH, ZLinkPageRequest.firstPage())
                    .toCompletableFuture()
                    .get(10, TimeUnit.SECONDS)
                    .items()
                    .getFirst();
        }

        void tick() throws Exception {
            tick(loop);
        }

        void tick(ZLinkAutoConnectLoop target) throws Exception {
            var method = ZLinkAutoConnectLoop.class.getDeclaredMethod("tick");
            method.setAccessible(true);
            ((CompletionStage<?>) method.invoke(target))
                    .toCompletableFuture()
                    .get(10, TimeUnit.SECONDS);
        }

        public void close() throws Exception {
            try {
                runtime.close();
            } finally {
                metrics.close();
            }
        }
    }

    private static Object field(Object owner, String name) throws Exception {
        var field = owner.getClass().getDeclaredField(name);
        field.setAccessible(true);
        return field.get(owner);
    }

    private static final class CountingStore {
        final ZLinkInMemoryProviderLocationStore delegate =
                new ZLinkInMemoryProviderLocationStore();
        final AtomicInteger reads = new AtomicInteger();
        volatile boolean fail;
        final boolean synchronous;

        CountingStore(boolean fail, boolean synchronous) {
            this.fail = fail;
            this.synchronous = synchronous;
        }

        ZLinkLocationStore provider() {
            return (ZLinkLocationStore)
                    Proxy.newProxyInstance(
                            ZLinkLocationStore.class.getClassLoader(),
                            new Class<?>[] {ZLinkLocationStore.class},
                            (proxy, method, args) -> {
                                if (method.getName().equals("read")
                                        && ((ZLinkStoreKey) args[0])
                                                .value()
                                                .startsWith("zlink:v11:capacity:")) {
                                    reads.incrementAndGet();
                                    if (fail) {
                                        var failure =
                                                new IllegalStateException("capacity-read-failure");
                                        if (synchronous) throw failure;
                                        return CompletableFuture.failedFuture(failure);
                                    }
                                }
                                try {
                                    return method.invoke(delegate, args);
                                } catch (InvocationTargetException failure) {
                                    throw failure.getCause();
                                }
                            });
        }
    }

    public record Room(ZLinkSpotContext context) implements ZLinkSpot<ZLinkActor> {
        public CompletionStage<Void> onJoinedActor(ZLinkActor actor) {
            return CompletableFuture.completedFuture(null);
        }

        public CompletionStage<Void> onLeaveActor(ZLinkActor actor) {
            return CompletableFuture.completedFuture(null);
        }

        public void configure() {}
    }
}
