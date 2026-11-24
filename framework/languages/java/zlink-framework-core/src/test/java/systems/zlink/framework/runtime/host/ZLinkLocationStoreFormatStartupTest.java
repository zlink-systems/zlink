package systems.zlink.framework.runtime.host;

import static org.junit.jupiter.api.Assertions.*;

import org.junit.jupiter.api.Test;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.errors.ZLinkConfigurationException;
import systems.zlink.framework.locationprovider.*;
import systems.zlink.framework.locations.*;
import systems.zlink.framework.runtime.binding.ZLinkJavaBackendAdapterFactory;
import systems.zlink.framework.runtime.configuration.DefaultZLinkFrameworkOptions;
import systems.zlink.framework.runtime.internal.locations.ZLinkLocationWriteIntent;
import systems.zlink.framework.runtime.internal.locations.ZLinkLocationWriteStatus;
import systems.zlink.framework.runtime.internal.locations.ZLinkMeshNodeDescriptor;
import systems.zlink.framework.runtime.internal.locations.ZLinkOwnerLeaseClaimed;
import systems.zlink.framework.runtime.internal.locations.ZLinkProviderLocationRepository;
import systems.zlink.framework.runtime.locations.ZLinkInMemoryProviderLocationStore;
import systems.zlink.framework.testsupport.ZLinkLocationContentionFixture;

import java.net.URLEncoder;
import java.nio.charset.StandardCharsets;
import java.time.Duration;
import java.time.Instant;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;
import java.util.Map;
import java.util.Optional;
import java.util.UUID;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionException;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicInteger;

final class ZLinkLocationStoreFormatStartupTest {
    private static final ZLinkStoreKey FORMAT = new ZLinkStoreKey("zlink:v12:format");
    private static final ZLinkStoreKey FLOOR = new ZLinkStoreKey("zlink:v12:generation-floor");
    private static final String LEGACY_PREFIX = "zlink:v11:capacity:";

    @Test
    void threeHostsConvertTheSameStoreBeforeOtherWrites() throws Exception {
        var scenario = ZLinkLocationContentionFixture.scenario("concurrentStartupConversion");
        var expected = scenario.required("expected");
        var records = scenario.required("records");
        String mesh = scenario.required("mesh").asText();
        int startupCount = scenario.required("startupCount").asInt();
        ProbeStore store = new ProbeStore();
        put(
                store.inner,
                new ZLinkStoreKey("zlink:v11:object-counter"),
                scenario.required("legacyNextObjectGeneration").asText());
        put(
                store.inner,
                new ZLinkStoreKey("zlink:v11:authority-owner-counter"),
                scenario.required("legacyNextAuthorityOwnerGeneration").asText());
        for (var record : records) {
            var descriptor = record.required("descriptorRoutingIdHex");
            if (!descriptor.isNull()) {
                seedDescriptor(
                        store.inner,
                        mesh,
                        RoutingId.fromHex(descriptor.asText()),
                        "v11-owner-" + descriptor.asText());
            }
            String rid =
                    URLEncoder.encode(
                            record.required("legacyRid").asText(), StandardCharsets.UTF_8);
            put(
                    store.inner,
                    legacy(mesh, rid),
                    legacyUsage(
                            "{}",
                            "{}",
                            record.required("activeActorSlots").asInt(),
                            record.required("pendingActorSlots").asInt()));
        }
        byte[] binary = new byte[RoutingId.MAX_LENGTH];
        Arrays.fill(binary, (byte) 0xff);
        RoutingId binaryRid = RoutingId.from(binary);
        put(store.inner, legacy(mesh, "hex%3A" + binaryRid.toHex()), legacyUsage("{}", "{}", 5, 0));
        store.concurrentStartups = startupCount;
        var hosts = new ArrayList<ZLinkFrameworkRuntime>();
        try {
            for (int i = 0; i < startupCount; i++) hosts.add(start(store, mesh));
            for (var host : hosts) startup(host);
        } finally {
            hosts.forEach(ZLinkFrameworkRuntime::close);
        }

        assertEquals(expected.required("format").asText(), value(store.inner, FORMAT));
        assertEquals(expected.required("generationFloor").asText(), value(store.inner, FLOOR));
        for (var record : records) {
            String hex = record.required("expectedRoutingIdHex").asText();
            assertEquals(
                    record.required("activeActorSlots").asText(),
                    value(store.inner, capacity(mesh, hex, "actors", "active")));
            var pending = record.required("pendingActorSlots");
            assertEquals(
                    pending.asInt() == 0 ? null : pending.asText(),
                    value(store.inner, capacity(mesh, hex, "actors", "pending")));
            assertNull(value(store.inner, capacity(mesh, hex, "spots", "active")));
        }
        assertNull(value(store.inner, capacity(mesh, "31", "actors", "active")));
        assertEquals(
                "5", value(store.inner, capacity(mesh, binaryRid.toHex(), "actors", "active")));
        var remaining =
                (ZLinkStoreScanPageResult)
                        store.inner
                                .scan(
                                        new ZLinkStoreScanRequest(LEGACY_PREFIX, null, 1000),
                                        () -> false)
                                .toCompletableFuture()
                                .join();
        assertEquals(
                expected.required("remainingLegacyRecords").asInt(),
                remaining.value().items().size());
        assertEquals(startupCount, store.initialScans.get());
        assertEquals(records.size() + 1, store.moved.get());
        assertEquals(
                expected.required("writesBeforeConversion").asInt(), store.prematureWrites.get());
    }

    @Test
    void resumesAfterOneRecordMovedAndAnInterruptedStartup() throws Exception {
        ProbeStore store = new ProbeStore();
        put(store.inner, legacy("a"), legacyUsage("{}", "{}", 2, 0));
        put(store.inner, legacy("b"), legacyUsage("{}", "{}", 3, 0));
        store.interruptAfterFirstRecord = true;
        try (var host = start(store)) {
            assertInstanceOf(ZLinkConfigurationException.class, startupFailure(host));
        }
        assertNull(value(store.inner, FORMAT));
        assertEquals("1", value(store.inner, FLOOR));
        assertEquals(1, store.moved.get());
        assertEquals(0, store.prematureWrites.get());

        store.interruptAfterFirstRecord = false;
        store.loseNextResponse = true;
        try (var host = start(store)) {
            startup(host);
        }
        assertEquals("converted", value(store.inner, FORMAT));
        assertEquals("1", value(store.inner, FLOOR));
        assertEquals("2", value(store.inner, capacity("61", "actors", "active")));
        assertEquals("3", value(store.inner, capacity("62", "actors", "active")));
        assertEquals(2, store.moved.get());
        assertEquals(0, store.prematureWrites.get());
    }

    @Test
    void rejectsALegacyRidThatMatchesTwoDescriptors() {
        ProbeStore store = new ProbeStore();
        seedDescriptor(store.inner, "mesh", RoutingId.from(1L), "v11-owner-1");
        seedDescriptor(store.inner, "mesh", RoutingId.from("1"), "v11-owner-2");
        put(store.inner, legacy("1"), legacyUsage("{}", "{}", 1, 0));

        try (var host = start(store)) {
            assertInstanceOf(ZLinkConfigurationException.class, startupFailure(host));
        }
        assertNotNull(value(store.inner, legacy("1")));
        assertNull(value(store.inner, FORMAT));
        assertNull(value(store.inner, capacity("00000001", "actors", "active")));
        assertNull(value(store.inner, capacity("31", "actors", "active")));
        assertEquals(0, store.prematureWrites.get());
    }

    @Test
    void omitsZeroCountersFromTheBatchAndRejectsAnOversizedBatchAtomically() throws Exception {
        ProbeStore fit = new ProbeStore();
        put(fit.inner, legacy("a"), legacyUsage(types(2046, 1), types(20, 0), 1, 0));
        try (var host = start(fit)) {
            startup(host);
        }
        assertEquals("converted", value(fit.inner, FORMAT));
        assertEquals(
                "1", value(fit.inner, capacity("61", "spot-type\0userSpot\0type2045", "active")));
        assertNull(value(fit.inner, capacity("61", "spot-type\0userSpot\0type0", "pending")));
        assertNull(value(fit.inner, legacy("a")));
        assertEquals(1, fit.moved.get());

        ProbeStore oversized = new ProbeStore();
        put(oversized.inner, legacy("a"), legacyUsage(types(2047, 1), "{}", 1, 0));
        try (var host = start(oversized)) {
            assertInstanceOf(ZLinkConfigurationException.class, startupFailure(host));
        }
        assertNull(value(oversized.inner, FORMAT));
        assertNotNull(value(oversized.inner, legacy("a")));
        assertNull(value(oversized.inner, capacity("61", "actors", "active")));
        assertEquals(0, oversized.moved.get());
        assertEquals(0, oversized.prematureWrites.get());
    }

    private static ZLinkFrameworkRuntime start(ZLinkLocationStore store) {
        return start(store, "mesh");
    }

    private static ZLinkFrameworkRuntime start(ZLinkLocationStore store, String mesh) {
        var options = new DefaultZLinkFrameworkOptions();
        options.addLocationStore(store);
        options.addRouteMesh(mesh).listen("inproc://format-test-" + UUID.randomUUID());
        return ZLinkFrameworkRuntimeTestAccess.start(options, new ZLinkJavaBackendAdapterFactory());
    }

    private static void startup(ZLinkFrameworkRuntime runtime) throws Exception {
        ZLinkFrameworkRuntimeTestAccess.startupCompletion(runtime)
                .toCompletableFuture()
                .get(10, TimeUnit.SECONDS);
    }

    private static Throwable startupFailure(ZLinkFrameworkRuntime runtime) {
        return assertThrows(
                        CompletionException.class,
                        () ->
                                ZLinkFrameworkRuntimeTestAccess.startupCompletion(runtime)
                                        .toCompletableFuture()
                                        .orTimeout(10, TimeUnit.SECONDS)
                                        .join())
                .getCause();
    }

    private static void seedDescriptor(
            ZLinkLocationStore store, String mesh, RoutingId rid, String ownerId) {
        var repository = new ZLinkProviderLocationRepository(store);
        var owner =
                assertInstanceOf(
                        ZLinkOwnerLeaseClaimed.class,
                        repository
                                .claimOwnerLease(ownerId, Duration.ofMinutes(1))
                                .toCompletableFuture()
                                .join());
        var node =
                new ZLinkMeshNodeDescriptor(
                        mesh,
                        rid,
                        1,
                        1,
                        "tcp://127.0.0.1:7100",
                        Map.of(),
                        1,
                        List.of(),
                        ZLinkMeshNodeObjectRole.SERVER,
                        Optional.empty(),
                        100,
                        new ZLinkPlacementCapacity(
                                new ZLinkCapacityUsage(0, 0, 0),
                                new ZLinkCapacityUsage(0, 0, 0),
                                List.of()),
                        new ZLinkActivationConcurrency(0, 8),
                        Optional.empty(),
                        ZLinkFrameworkRuntimeState.SERVING,
                        "seeded-v11",
                        ownerId,
                        owner.token().leaseGeneration(),
                        Instant.now());
        assertEquals(
                ZLinkLocationWriteStatus.STORED,
                repository
                        .updateMeshNode(node, ZLinkLocationWriteIntent.NEW_CLAIM)
                        .toCompletableFuture()
                        .join()
                        .status());
    }

    private static String types(int count, int value) {
        var fields = new ArrayList<String>();
        for (int i = 0; i < count; i++) fields.add("\"user_spot\\u0000type" + i + "\":" + value);
        return "{" + String.join(",", fields) + "}";
    }

    private static String legacyUsage(
            String activeTypes, String pendingTypes, int active, int pending) {
        return """
        {"active":{"actors":%d,"spots":0,"spotTypes":%s},
         "pending":{"actors":%d,"spots":0,"spotTypes":%s}}
        """
                .formatted(active, activeTypes, pending, pendingTypes);
    }

    private static ZLinkStoreKey legacy(String rid) {
        return legacy("mesh", rid);
    }

    private static ZLinkStoreKey legacy(String mesh, String rid) {
        return new ZLinkStoreKey(LEGACY_PREFIX + mesh + ":" + rid);
    }

    private static ZLinkStoreKey capacity(String hex, String dimension, String state) {
        return capacity("mesh", hex, dimension, state);
    }

    private static ZLinkStoreKey capacity(String mesh, String hex, String dimension, String state) {
        return new ZLinkStoreKey(
                "zlink:v12:capacity\0" + mesh + "\0" + hex + "\0" + dimension + "\0" + state);
    }

    private static String value(ZLinkLocationStore store, ZLinkStoreKey key) {
        var read = store.read(key, () -> false).toCompletableFuture().join();
        return read instanceof ZLinkStoreReadFound found
                ? new String(found.value().bytes(), StandardCharsets.UTF_8)
                : null;
    }

    private static void put(ZLinkLocationStore store, ZLinkStoreKey key, String value) {
        assertInstanceOf(
                ZLinkStoreWriteApplied.class,
                store.write(
                                new ZLinkStoreWriteRequest(
                                        List.of(),
                                        List.of(
                                                new ZLinkStorePut(
                                                        key,
                                                        value.getBytes(StandardCharsets.UTF_8),
                                                        null))),
                                () -> false)
                        .toCompletableFuture()
                        .join());
    }

    private static final class ProbeStore implements ZLinkLocationStore {
        private final ZLinkLocationStore inner = new ZLinkInMemoryProviderLocationStore();
        private final AtomicInteger initialScans = new AtomicInteger();
        private final CompletableFuture<Void> allScanning = new CompletableFuture<>();
        private final AtomicInteger moved = new AtomicInteger();
        private final AtomicInteger prematureWrites = new AtomicInteger();
        private int concurrentStartups;
        private volatile boolean interruptAfterFirstRecord;
        private volatile boolean loseNextResponse;

        @Override
        public CompletionStage<ZLinkStoreReadResult> read(
                ZLinkStoreKey key, ZLinkStoreCancellation cancellation) {
            return inner.read(key, cancellation);
        }

        @Override
        public CompletionStage<ZLinkStoreWriteResult> write(
                ZLinkStoreWriteRequest request, ZLinkStoreCancellation cancellation) {
            boolean convertsRecord =
                    request.mutations().stream()
                            .anyMatch(
                                    mutation ->
                                            mutation instanceof ZLinkStoreDelete delete
                                                    && delete.key()
                                                            .value()
                                                            .startsWith(LEGACY_PREFIX));
            if (convertsRecord && interruptAfterFirstRecord && moved.get() > 0) {
                return CompletableFuture.failedFuture(
                        new ZLinkConfigurationException("injected startup interruption"));
            }
            boolean formatMetadata =
                    request.mutations().size() == 1
                            && request.mutations().getFirst() instanceof ZLinkStorePut put
                            && (put.key().equals(FLOOR) || put.key().equals(FORMAT));
            if (!convertsRecord && !formatMetadata && value(inner, FORMAT) == null) {
                prematureWrites.incrementAndGet();
            }
            return inner.write(request, cancellation)
                    .thenCompose(
                            result -> {
                                if (convertsRecord && result instanceof ZLinkStoreWriteApplied) {
                                    moved.incrementAndGet();
                                    if (loseNextResponse) {
                                        loseNextResponse = false;
                                        return CompletableFuture.failedFuture(
                                                new IllegalStateException(
                                                        "lost conversion response"));
                                    }
                                }
                                return CompletableFuture.completedFuture(result);
                            });
        }

        @Override
        public CompletionStage<ZLinkStoreScanResult> scan(
                ZLinkStoreScanRequest request, ZLinkStoreCancellation cancellation) {
            var result = inner.scan(request, cancellation);
            if (concurrentStartups > 0
                    && request.prefix().equals(LEGACY_PREFIX)
                    && !allScanning.isDone()) {
                if (initialScans.incrementAndGet() == concurrentStartups) {
                    assertNull(value(inner, FORMAT));
                    assertEquals(0, prematureWrites.get());
                    allScanning.complete(null);
                }
                return allScanning.thenCompose(ignored -> result);
            }
            return result;
        }
    }
}
