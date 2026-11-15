package systems.zlink.framework.runtime.locations;

import static org.junit.jupiter.api.Assertions.*;

import org.junit.jupiter.api.Test;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.errors.ZLinkFrameworkErrorKind;
import systems.zlink.framework.errors.ZLinkFrameworkException;
import systems.zlink.framework.locationprovider.ZLinkLocationStore;
import systems.zlink.framework.locationprovider.ZLinkStoreCancellation;
import systems.zlink.framework.locationprovider.ZLinkStoreKey;
import systems.zlink.framework.locationprovider.ZLinkStoreReadFound;
import systems.zlink.framework.locationprovider.ZLinkStoreReadResult;
import systems.zlink.framework.locationprovider.ZLinkStoreScanRequest;
import systems.zlink.framework.locationprovider.ZLinkStoreScanResult;
import systems.zlink.framework.locationprovider.ZLinkStoreValue;
import systems.zlink.framework.locationprovider.ZLinkStoreWriteRequest;
import systems.zlink.framework.locationprovider.ZLinkStoreWriteResult;
import systems.zlink.framework.locations.*;
import systems.zlink.framework.runtime.host.ZLinkFrameworkRuntimeState;
import systems.zlink.framework.runtime.internal.locations.*;
import systems.zlink.framework.runtime.internal.locations.ZLinkLocationRepository;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceMessageFollowWireCodec;
import systems.zlink.framework.spots.ZLinkSpotKind;

import java.lang.reflect.Proxy;
import java.time.Duration;
import java.time.Instant;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.Optional;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionException;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.concurrent.atomic.AtomicReference;
import java.util.function.BiFunction;

final class ZLinkStoreLocationResolversTest {
    private static final Instant NOW = Instant.parse("2026-07-27T00:00:00Z");
    private static final RoutingId NODE = RoutingId.from("node-a");

    @Test
    void serviceSummaryCountsExactLiveOwnersAndKeepsErrorCountZero() {
        var live = meshNodeDescriptor("mesh", RoutingId.from("live"), "live-owner");
        var stale = meshNodeDescriptor("mesh", RoutingId.from("stale"), "stale-owner");
        var missing = meshNodeDescriptor("mesh", RoutingId.from("missing"), "missing-owner");
        var expired = meshNodeDescriptor("mesh", RoutingId.from("expired"), "expired-owner");
        var store =
                repository(
                        (method, arguments) ->
                                switch (method) {
                                    case "listMeshNodes" ->
                                            CompletableFuture.completedFuture(
                                                    new ZLinkLocationPage<>(
                                                            List.of(live, stale, missing, expired),
                                                            null));
                                    case "readOwnerLease" ->
                                            CompletableFuture.completedFuture(
                                                    "missing-owner".equals(arguments[0])
                                                            ? new ZLinkOwnerLeaseMissing()
                                                            : new ZLinkOwnerLeaseFound(
                                                                    new ZLinkLocationOwnerToken(
                                                                            (String) arguments[0],
                                                                            "stale-owner"
                                                                                            .equals(
                                                                                                    arguments[
                                                                                                            0])
                                                                                    ? 2
                                                                                    : 1),
                                                                    "expired-owner"
                                                                                    .equals(
                                                                                            arguments[
                                                                                                    0])
                                                                            ? NOW
                                                                            : NOW.plusSeconds(30),
                                                                    NOW));
                                    default -> throw new UnsupportedOperationException(method);
                                });
        var stores = ZLinkRegisteredLocationStores.fromUnified(store);
        try (var runtime =
                new ZLinkLocationRuntime(stores, Duration.ofSeconds(30), Duration.ofSeconds(10))) {
            var query =
                    new ZLinkLocationRuntimeQueryService(
                            stores, runtime, new ZLinkLocationOptions());
            var summary =
                    query.listServiceSummaries(new ZLinkLocationServiceSummaryFilter("mesh"), null)
                            .toCompletableFuture()
                            .join()
                            .items()
                            .get(0);
            assertEquals(4, summary.totalCount());
            assertEquals(1, summary.readyCount());
            assertEquals(3, summary.stoppedCount());
            assertEquals(0, summary.errorCount());
        }
    }

    @Test
    void serviceSummaryRejectsOwnerLeaseFoundForAnotherOwner() {
        var live = meshNodeDescriptor("mesh", RoutingId.from("live"), "live-owner");
        var wrong = meshNodeDescriptor("mesh", RoutingId.from("wrong"), "requested-owner");
        var store =
                repository(
                        (method, arguments) ->
                                switch (method) {
                                    case "listMeshNodes" ->
                                            CompletableFuture.completedFuture(
                                                    new ZLinkLocationPage<>(
                                                            List.of(live, wrong), null));
                                    case "readOwnerLease" ->
                                            CompletableFuture.completedFuture(
                                                    new ZLinkOwnerLeaseFound(
                                                            new ZLinkLocationOwnerToken(
                                                                    "live-owner"
                                                                                    .equals(
                                                                                            arguments[
                                                                                                    0])
                                                                            ? "live-owner"
                                                                            : "actual-owner",
                                                                    1),
                                                            NOW.plusSeconds(30),
                                                            NOW));
                                    default -> throw new UnsupportedOperationException(method);
                                });
        var stores = ZLinkRegisteredLocationStores.fromUnified(store);
        try (var runtime =
                new ZLinkLocationRuntime(stores, Duration.ofSeconds(30), Duration.ofSeconds(10))) {
            var query =
                    new ZLinkLocationRuntimeQueryService(
                            stores, runtime, new ZLinkLocationOptions());
            var summary =
                    query.listServiceSummaries(new ZLinkLocationServiceSummaryFilter("mesh"), null)
                            .toCompletableFuture()
                            .join()
                            .items()
                            .get(0);
            assertEquals(2, summary.totalCount());
            assertEquals(1, summary.readyCount());
            assertEquals(1, summary.stoppedCount());
            assertEquals(0, summary.errorCount());
        }
    }

    @Test
    void serviceSummaryLeaseFailureFailsTheWholeQuery() {
        assertServiceSummaryLeaseError(
                false,
                ZLinkFrameworkErrorKind.UNAVAILABLE,
                new IllegalStateException("lease store unavailable"));
    }

    @Test
    void serviceSummaryLeaseWithoutExpirationFailsWithInternalFailure() {
        assertServiceSummaryLeaseError(true, ZLinkFrameworkErrorKind.INTERNAL_FAILURE, null);
    }

    @Test
    void serviceSummaryProviderArgumentFailureIsUnavailable() {
        assertServiceSummaryLeaseError(
                false,
                ZLinkFrameworkErrorKind.UNAVAILABLE,
                new IllegalArgumentException("provider rejected lease read"));
    }

    @Test
    void serviceSummaryPublicProviderNotFoundFailureIsUnavailable() {
        assertPublicProviderServiceSummary(
                new ZLinkFrameworkException(
                        ZLinkFrameworkErrorKind.NOT_FOUND, "provider read failed"),
                false,
                false,
                false);
        assertPublicProviderServiceSummary(
                new ZLinkFrameworkException(
                        ZLinkFrameworkErrorKind.NOT_FOUND, "provider read failed"),
                false,
                true,
                false);
    }

    @Test
    void serviceSummaryPublicProviderInvalidOperationFailureIsUnavailable() {
        assertPublicProviderServiceSummary(
                new ZLinkFrameworkException(
                        ZLinkFrameworkErrorKind.INVALID_OPERATION, "provider read failed"),
                false,
                false,
                false);
        assertPublicProviderServiceSummary(
                new ZLinkFrameworkException(
                        ZLinkFrameworkErrorKind.INVALID_OPERATION, "provider read failed"),
                false,
                true,
                false);
    }

    @Test
    void serviceSummaryPublicProviderLeaseWithoutExpirationIsInternalFailure() {
        assertPublicProviderServiceSummary(null, true, false, false);
    }

    @Test
    void serviceSummaryPublicProviderValidLeaseIsReady() {
        assertPublicProviderServiceSummary(null, false, false, false);
    }

    @Test
    void serviceSummaryPublicProviderWrongOwnerLeaseIsStopped() {
        assertPublicProviderServiceSummary(null, false, false, true);
    }

    private static void assertPublicProviderServiceSummary(
            RuntimeException providerFailure,
            boolean corrupt,
            boolean synchronous,
            boolean wrongOwner) {
        String ownerId = "owner-a";
        var backing =
                new ZLinkInMemoryProviderLocationStore(
                        java.time.Clock.fixed(NOW, java.time.ZoneOffset.UTC));
        var seeded = new ZLinkProviderLocationRepository(backing);
        var claimed =
                assertInstanceOf(
                        ZLinkOwnerLeaseClaimed.class,
                        seeded.claimOwnerLease(ownerId, Duration.ofSeconds(30))
                                .toCompletableFuture()
                                .join());
        assertEquals(1, claimed.token().leaseGeneration());
        seeded.updateMeshNode(
                        meshNodeDescriptor("mesh", NODE, ownerId),
                        ZLinkLocationWriteIntent.NEW_CLAIM)
                .toCompletableFuture()
                .join();
        ZLinkLocationStore provider =
                new ZLinkLocationStore() {
                    @Override
                    public java.util.concurrent.CompletionStage<ZLinkStoreReadResult> read(
                            ZLinkStoreKey key, ZLinkStoreCancellation cancellation) {
                        if (providerFailure != null) {
                            if (synchronous) throw providerFailure;
                            return CompletableFuture.failedFuture(providerFailure);
                        }
                        return backing.read(key, cancellation)
                                .thenApply(
                                        result -> {
                                            if ((!corrupt && !wrongOwner)
                                                    || !(result
                                                            instanceof ZLinkStoreReadFound found))
                                                return result;
                                            var value = found.value();
                                            byte[] leaseBytes = value.bytes();
                                            if (wrongOwner) {
                                                String mutated =
                                                        new String(
                                                                        leaseBytes,
                                                                        java.nio.charset
                                                                                .StandardCharsets
                                                                                .UTF_8)
                                                                .replace(
                                                                        "\"" + ownerId + "\"",
                                                                        "\"other-owner\"");
                                                byte[] changed =
                                                        mutated.getBytes(
                                                                java.nio.charset.StandardCharsets
                                                                        .UTF_8);
                                                assertFalse(
                                                        java.util.Arrays.equals(
                                                                leaseBytes, changed));
                                                assertTrue(mutated.contains("\"other-owner\""));
                                                leaseBytes = changed;
                                            }
                                            return new ZLinkStoreReadFound(
                                                    new ZLinkStoreValue(
                                                            leaseBytes,
                                                            value.version(),
                                                            corrupt ? null : value.expiresAt(),
                                                            value.storeNow()));
                                        });
                    }

                    @Override
                    public java.util.concurrent.CompletionStage<ZLinkStoreWriteResult> write(
                            ZLinkStoreWriteRequest request, ZLinkStoreCancellation cancellation) {
                        return backing.write(request, cancellation);
                    }

                    @Override
                    public java.util.concurrent.CompletionStage<ZLinkStoreScanResult> scan(
                            ZLinkStoreScanRequest request, ZLinkStoreCancellation cancellation) {
                        return backing.scan(request, cancellation);
                    }
                };
        var stores =
                ZLinkRegisteredLocationStores.fromUnified(
                        new ZLinkProviderLocationRepository(provider));
        try (var runtime =
                new ZLinkLocationRuntime(stores, Duration.ofSeconds(30), Duration.ofSeconds(10))) {
            var query =
                    new ZLinkLocationRuntimeQueryService(
                            stores, runtime, new ZLinkLocationOptions());
            var operation =
                    query.listServiceSummaries(new ZLinkLocationServiceSummaryFilter("mesh"), null);
            if (providerFailure == null && !corrupt) {
                var summary = operation.toCompletableFuture().join().items().get(0);
                assertEquals(1, summary.totalCount());
                assertEquals(wrongOwner ? 0 : 1, summary.readyCount());
                assertEquals(wrongOwner ? 1 : 0, summary.stoppedCount());
                assertEquals(0, summary.errorCount());
            } else {
                var failure =
                        assertThrows(
                                CompletionException.class,
                                () -> operation.toCompletableFuture().join());
                var actual = assertInstanceOf(ZLinkFrameworkException.class, failure.getCause());
                assertEquals(
                        corrupt
                                ? ZLinkFrameworkErrorKind.INTERNAL_FAILURE
                                : ZLinkFrameworkErrorKind.UNAVAILABLE,
                        actual.kind());
                if (providerFailure != null) assertSame(providerFailure, actual.getCause());
            }
        }
    }

    @Test
    void serviceSummaryPageValidationRemainsSynchronous() {
        var store =
                repository(
                        (method, arguments) -> {
                            throw new AssertionError("no provider read expected");
                        });
        var stores = ZLinkRegisteredLocationStores.fromUnified(store);
        try (var runtime =
                new ZLinkLocationRuntime(stores, Duration.ofSeconds(30), Duration.ofSeconds(10))) {
            var query =
                    new ZLinkLocationRuntimeQueryService(
                            stores, runtime, new ZLinkLocationOptions());
            assertThrows(
                    IllegalArgumentException.class,
                    () ->
                            query.listServiceSummaries(
                                    new ZLinkLocationServiceSummaryFilter("mesh"),
                                    new ZLinkPageRequest(1001, null)));
        }
    }

    private static void assertServiceSummaryLeaseError(
            boolean corrupt, ZLinkFrameworkErrorKind expected, RuntimeException storeFailure) {
        var live = meshNodeDescriptor("mesh", RoutingId.from("live"), "live-owner");
        var failed = meshNodeDescriptor("mesh", RoutingId.from("failed"), "failed-owner");
        var store =
                repository(
                        (method, arguments) ->
                                switch (method) {
                                    case "listMeshNodes" ->
                                            CompletableFuture.completedFuture(
                                                    new ZLinkLocationPage<>(
                                                            List.of(live, failed), null));
                                    case "readOwnerLease" -> {
                                        if ("live-owner".equals(arguments[0]))
                                            yield CompletableFuture.completedFuture(
                                                    new ZLinkOwnerLeaseFound(
                                                            new ZLinkLocationOwnerToken(
                                                                    "live-owner", 1),
                                                            NOW.plusSeconds(30),
                                                            NOW));
                                        if (corrupt)
                                            yield CompletableFuture.completedFuture(
                                                    new ZLinkOwnerLeaseFound(
                                                            new ZLinkLocationOwnerToken(
                                                                    "failed-owner", 1),
                                                            null,
                                                            NOW));
                                        yield CompletableFuture.failedFuture(storeFailure);
                                    }
                                    default -> throw new UnsupportedOperationException(method);
                                });
        var stores = ZLinkRegisteredLocationStores.fromUnified(store);
        try (var runtime =
                new ZLinkLocationRuntime(stores, Duration.ofSeconds(30), Duration.ofSeconds(10))) {
            var query =
                    new ZLinkLocationRuntimeQueryService(
                            stores, runtime, new ZLinkLocationOptions());
            var failure =
                    assertThrows(
                            CompletionException.class,
                            () ->
                                    query.listServiceSummaries(
                                                    new ZLinkLocationServiceSummaryFilter("mesh"),
                                                    null)
                                            .toCompletableFuture()
                                            .join());
            assertEquals(
                    expected,
                    assertInstanceOf(ZLinkFrameworkException.class, failure.getCause()).kind());
        }
    }

    @Test
    void objectLocationPagesFollowSharedFixture() throws Exception {
        var root = java.nio.file.Path.of(System.getProperty("user.dir")).toAbsolutePath();
        while (root != null
                && !java.nio.file.Files.isRegularFile(
                        root.resolve(
                                "framework/runtime/conformance/object-location-pages-v1.json")))
            root = root.getParent();
        assertNotNull(root);
        var fixture =
                new com.fasterxml.jackson.databind.ObjectMapper()
                        .readTree(
                                java.nio.file.Files.readString(
                                        root.resolve(
                                                "framework/runtime/conformance/object-location-pages-v1.json")));
        for (var scenario : fixture.get("cases")) {
            var ids = new java.util.HashMap<String, String>();
            var entries = new java.util.ArrayList<ZLinkAuthorityEntry>();
            for (var row : fixture.get("datasets").get(scenario.get("dataset").asText())) {
                var profile = fixture.get("profiles").get(row.get("profile").asText());
                var id = row.get("id").asText();
                var globalId =
                        profile.get("idPrefix").asText() + row.get("globalIdSuffix").asText();
                ids.put(id, globalId);
                var kind =
                        ZLinkPlacementObjectKind.valueOf(
                                row.get("kind").asText().toUpperCase(java.util.Locale.ROOT));
                var original = readySpotSnapshot();
                var snapshot =
                        new ZLinkAuthoritySnapshot(
                                "v1",
                                original.payload(),
                                1,
                                1,
                                original.ownerId(),
                                1,
                                new ZLinkPlacementAllocation(
                                        ZLinkPlacementAllocationState.ACTIVE,
                                        kind,
                                        profile.get("stableType").asText(),
                                        new ZLinkMeshNodeDescriptorKey(
                                                profile.get("meshName").asText(),
                                                RoutingId.from(profile.get("nodeRid").asText())),
                                        1,
                                        original.allocation().capacityBundle()),
                                original.pendingCreation(),
                                original.storeNow());
                entries.add(
                        new ZLinkAuthorityEntry(
                                kind == ZLinkPlacementObjectKind.ACTOR
                                        ? ZLinkAuthorityKeyCodec.actor(globalId)
                                        : ZLinkAuthorityKeyCodec.spot(globalId),
                                snapshot));
            }
            var scans = new AtomicInteger();
            var store =
                    repository(
                            (method, arguments) -> {
                                if (method.equals("readOwnerLease"))
                                    return CompletableFuture.completedFuture(
                                            new ZLinkOwnerLeaseMissing());
                                if (!method.equals("list"))
                                    throw new UnsupportedOperationException(method);
                                scans.incrementAndGet();
                                var prefix = (String) arguments[0];
                                assertTrue(
                                        prefix.equals(ZLinkAuthorityKeyCodec.actorPrefix())
                                                || prefix.equals(
                                                        ZLinkAuthorityKeyCodec.spotPrefix()));
                                @SuppressWarnings("unchecked")
                                var cursor = (Optional<ZLinkAuthorityScanCursor>) arguments[1];
                                int offset =
                                        cursor.map(value -> Integer.parseInt(value.encoded()))
                                                .orElse(0);
                                var rows =
                                        entries.stream()
                                                .filter(entry -> entry.key().startsWith(prefix))
                                                .toList();
                                int end =
                                        Math.min(
                                                offset
                                                        + Math.min(
                                                                (Integer) arguments[2],
                                                                scenario.get("storePageSize")
                                                                        .asInt()),
                                                rows.size());
                                return CompletableFuture.supplyAsync(
                                        () ->
                                                new ZLinkAuthorityPage(
                                                        rows.subList(offset, end),
                                                        end < rows.size()
                                                                ? Optional.of(
                                                                        new ZLinkAuthorityScanCursor(
                                                                                Integer.toString(
                                                                                        end)))
                                                                : Optional.empty()));
                            });
            var stores = ZLinkRegisteredLocationStores.fromUnified(store);
            try (var runtime =
                    new ZLinkLocationRuntime(
                            stores, Duration.ofSeconds(30), Duration.ofSeconds(10))) {
                var query =
                        new ZLinkLocationRuntimeQueryService(
                                stores, runtime, new ZLinkLocationOptions());
                var jsonMapper = new com.fasterxml.jackson.databind.ObjectMapper();
                var encodingProbe =
                        new ZLinkLocationObjectEntry(
                                "actor-é\"\\\b\t\n\f\r\u0001",
                                1,
                                "mesh-é\"\\\b\t\n\f\r\u0001",
                                RoutingId.from("node-1"),
                                ZLinkLocationObjectState.READY,
                                "type-é\"\\\b\t\n\f\r\u0001");
                assertEquals(
                        jsonMapper.writeValueAsBytes(
                                        Map.of(
                                                "globalId", encodingProbe.globalId(),
                                                "objectGeneration",
                                                        encodingProbe.objectGeneration(),
                                                "meshName", encodingProbe.meshName(),
                                                "nodeRid", encodingProbe.nodeRid().toString(),
                                                "state", encodingProbe.state().name(),
                                                "stableType", encodingProbe.stableType()))
                                .length,
                        ZLinkLocationRuntimeQueryService.encodedObjectBytes(encodingProbe));
                var filter =
                        ZLinkLocationObjectFilter.of(
                                ZLinkPlacementObjectKind.valueOf(
                                        scenario.get("objectKind")
                                                .asText()
                                                .toUpperCase(java.util.Locale.ROOT)));
                var all = new java.util.ArrayList<String>();
                List<ZLinkLocationObjectEntry> previousBoundaryItems = null;
                String previousBoundaryCursor = null;
                String token = null;
                for (var expected : scenario.get("expectedPages")) {
                    var page =
                            query.listObjectLocations(
                                            filter,
                                            new ZLinkPageRequest(
                                                    scenario.get("pageSize").asInt(), token))
                                    .toCompletableFuture()
                                    .join();
                    var encodedEntries =
                            page.items().stream()
                                    .map(
                                            entry ->
                                                    Map.of(
                                                            "globalId", entry.globalId(),
                                                            "objectGeneration",
                                                                    entry.objectGeneration(),
                                                            "meshName", entry.meshName(),
                                                            "nodeRid", entry.nodeRid().toString(),
                                                            "state", entry.state().name(),
                                                            "stableType", entry.stableType()))
                                    .toList();
                    var encodedPage = new LinkedHashMap<String, Object>();
                    encodedPage.put("items", encodedEntries);
                    encodedPage.put("continuationToken", page.continuationToken());
                    int actualPageBytes = jsonMapper.writeValueAsBytes(encodedPage).length;
                    int calculatedItemsBytes =
                            page.items().stream()
                                    .mapToInt(ZLinkLocationRuntimeQueryService::encodedObjectBytes)
                                    .sum();
                    assertEquals(
                            actualPageBytes,
                            ZLinkLocationRuntimeQueryService.encodedObjectPageBytes(
                                    calculatedItemsBytes,
                                    page.items().size(),
                                    page.continuationToken()));
                    assertTrue(actualPageBytes <= 4 * 1024 * 1024);
                    if (previousBoundaryItems != null
                            && scenario.get("pageSize").asInt() == 1000
                            && !page.items().isEmpty()
                            && previousBoundaryItems.size() < 1000) {
                        var candidateEntries =
                                java.util.stream.Stream.concat(
                                                previousBoundaryItems.stream(),
                                                java.util.stream.Stream.of(page.items().get(0)))
                                        .map(
                                                entry ->
                                                        Map.of(
                                                                "globalId", entry.globalId(),
                                                                "objectGeneration",
                                                                        entry.objectGeneration(),
                                                                "meshName", entry.meshName(),
                                                                "nodeRid",
                                                                        entry.nodeRid().toString(),
                                                                "state", entry.state().name(),
                                                                "stableType", entry.stableType()))
                                        .toList();
                        var candidatePage = new LinkedHashMap<String, Object>();
                        candidatePage.put("items", candidateEntries);
                        candidatePage.put("continuationToken", previousBoundaryCursor);
                        assertTrue(
                                jsonMapper.writeValueAsBytes(candidatePage).length
                                        > 4 * 1024 * 1024);
                    }
                    previousBoundaryItems = page.items();
                    previousBoundaryCursor = page.continuationToken();
                    var expectedIds = new java.util.ArrayList<String>();
                    expected.forEach(id -> expectedIds.add(ids.get(id.asText())));
                    assertEquals(
                            expectedIds.size(), page.items().size(), scenario.get("name").asText());
                    for (int index = 0; index < expectedIds.size(); index++)
                        assertEquals(
                                expectedIds.get(index),
                                page.items().get(index).globalId(),
                                scenario.get("name").asText() + " entry " + index);
                    all.addAll(
                            page.items().stream().map(ZLinkLocationObjectEntry::globalId).toList());
                    token = page.continuationToken();
                }
                assertNull(token, scenario.get("name").asText());
                assertEquals(
                        scenario.get("expectedScanCalls").asInt(),
                        scans.get(),
                        scenario.get("name").asText());
                var expectedAll = new java.util.ArrayList<String>();
                scenario.get("expectedAll").forEach(id -> expectedAll.add(ids.get(id.asText())));
                assertEquals(expectedAll, all);
                assertEquals(all.size(), new java.util.HashSet<>(all).size());
            }
        }
    }

    @Test
    void objectQueryScansPagesRatherThanIndividualAuthorities() {
        var entries =
                java.util.stream.IntStream.range(0, 1000)
                        .mapToObj(
                                index ->
                                        new ZLinkAuthorityEntry(
                                                ZLinkAuthorityKeyCodec.spot("room-" + index),
                                                readySpotSnapshot()))
                        .toList();
        for (int pageSize : new int[] {100, 256, 1000}) {
            var scans = new AtomicInteger();
            var limits = new java.util.ArrayList<Integer>();
            var store =
                    repository(
                            (method, arguments) ->
                                    switch (method) {
                                        case "list" -> {
                                            assertEquals(
                                                    ZLinkAuthorityKeyCodec.spotPrefix(),
                                                    arguments[0]);
                                            @SuppressWarnings("unchecked")
                                            var cursor =
                                                    (Optional<ZLinkAuthorityScanCursor>)
                                                            arguments[1];
                                            int offset =
                                                    cursor.map(
                                                                    value ->
                                                                            Integer.parseInt(
                                                                                    value
                                                                                            .encoded()))
                                                            .orElse(0);
                                            int limit = (Integer) arguments[2];
                                            scans.incrementAndGet();
                                            limits.add(limit);
                                            int end = Math.min(offset + limit, entries.size());
                                            yield CompletableFuture.completedFuture(
                                                    new ZLinkAuthorityPage(
                                                            entries.subList(offset, end),
                                                            end < entries.size()
                                                                    ? Optional.of(
                                                                            new ZLinkAuthorityScanCursor(
                                                                                    Integer
                                                                                            .toString(
                                                                                                    end)))
                                                                    : Optional.empty()));
                                        }
                                        case "readOwnerLease" ->
                                                CompletableFuture.completedFuture(
                                                        new ZLinkOwnerLeaseMissing());
                                        default -> throw new UnsupportedOperationException(method);
                                    });
            var stores = ZLinkRegisteredLocationStores.fromUnified(store);
            try (var runtime =
                    new ZLinkLocationRuntime(
                            stores, Duration.ofSeconds(30), Duration.ofSeconds(10))) {
                var query =
                        new ZLinkLocationRuntimeQueryService(
                                stores, runtime, new ZLinkLocationOptions());
                var ids = new java.util.ArrayList<String>();
                String token = null;
                do {
                    var page =
                            query.listObjectLocations(
                                            ZLinkLocationObjectFilter.of(
                                                    ZLinkPlacementObjectKind.USER_SPOT),
                                            new ZLinkPageRequest(pageSize, token))
                                    .toCompletableFuture()
                                    .join();
                    assertEquals(
                            Math.min(pageSize, entries.size() - ids.size()), page.items().size());
                    ids.addAll(
                            page.items().stream().map(ZLinkLocationObjectEntry::globalId).toList());
                    token = page.continuationToken();
                    assertEquals((ids.size() + pageSize - 1) / pageSize, scans.get());
                } while (token != null);
                assertEquals(
                        java.util.stream.IntStream.range(0, 1000)
                                .mapToObj(index -> "room-" + index)
                                .toList(),
                        ids);
                assertTrue(limits.stream().allMatch(limit -> limit == pageSize));
            }
        }
    }

    @Test
    void objectQueryKeepsByteLimitedContinuationAtUnreturnedEntry() {
        var original = readySpotSnapshot();
        var stableType = "\u0001".repeat(255);
        var descriptor =
                new ZLinkMeshNodeDescriptorKey(
                        "\u0001".repeat(160), RoutingId.from("\u0001".repeat(255)));
        var allocation = original.allocation();
        var snapshot =
                new ZLinkAuthoritySnapshot(
                        original.storeVersion(),
                        original.payload(),
                        original.objectGeneration(),
                        original.authorityOwnerGeneration(),
                        original.ownerId(),
                        original.ownerLeaseGeneration(),
                        new ZLinkPlacementAllocation(
                                allocation.state(),
                                allocation.objectKind(),
                                stableType,
                                descriptor,
                                allocation.descriptorLifecycleGeneration(),
                                ZLinkPlacementCapacityBundle.spot(
                                        ZLinkPlacementObjectKind.USER_SPOT, stableType, 1)),
                        original.pendingCreation(),
                        original.storeNow());
        var ids =
                java.util.stream.IntStream.range(0, 1000)
                        .mapToObj(index -> "\u0001".repeat(250) + String.format("%04d", index))
                        .toList();
        var entries =
                ids.stream()
                        .map(
                                id ->
                                        new ZLinkAuthorityEntry(
                                                ZLinkAuthorityKeyCodec.spot(id), snapshot))
                        .toList();
        var store =
                repository(
                        (method, arguments) ->
                                switch (method) {
                                    case "list" -> {
                                        @SuppressWarnings("unchecked")
                                        var cursor =
                                                (Optional<ZLinkAuthorityScanCursor>) arguments[1];
                                        int offset =
                                                cursor.map(
                                                                value ->
                                                                        Integer.parseInt(
                                                                                value.encoded()))
                                                        .orElse(0);
                                        int end =
                                                Math.min(
                                                        offset
                                                                + Math.min(
                                                                        (Integer) arguments[2],
                                                                        128),
                                                        entries.size());
                                        yield CompletableFuture.supplyAsync(
                                                () ->
                                                        new ZLinkAuthorityPage(
                                                                entries.subList(offset, end),
                                                                end < entries.size()
                                                                        ? Optional.of(
                                                                                new ZLinkAuthorityScanCursor(
                                                                                        Integer
                                                                                                .toString(
                                                                                                        end)))
                                                                        : Optional.empty()));
                                    }
                                    case "readOwnerLease" ->
                                            CompletableFuture.completedFuture(
                                                    new ZLinkOwnerLeaseMissing());
                                    default -> throw new UnsupportedOperationException(method);
                                });
        var stores = ZLinkRegisteredLocationStores.fromUnified(store);
        try (var runtime =
                new ZLinkLocationRuntime(stores, Duration.ofSeconds(30), Duration.ofSeconds(10))) {
            var query =
                    new ZLinkLocationRuntimeQueryService(
                            stores, runtime, new ZLinkLocationOptions());
            var filter = ZLinkLocationObjectFilter.of(ZLinkPlacementObjectKind.USER_SPOT);
            var first =
                    query.listObjectLocations(filter, new ZLinkPageRequest(1000, null))
                            .toCompletableFuture()
                            .join();
            assertEquals(909, first.items().size());
            assertEquals(
                    ids.subList(0, first.items().size()),
                    first.items().stream().map(ZLinkLocationObjectEntry::globalId).toList());
            assertNotNull(first.continuationToken());
            var second =
                    query.listObjectLocations(
                                    filter, new ZLinkPageRequest(1000, first.continuationToken()))
                            .toCompletableFuture()
                            .join();
            assertEquals(
                    ids.subList(first.items().size(), ids.size()),
                    second.items().stream().map(ZLinkLocationObjectEntry::globalId).toList());
            assertNull(second.continuationToken());
        }
    }

    @Test
    void objectQueryContinuesPastEmptyFilteredPage() {
        var scans = new AtomicInteger();
        ZLinkLocationRepository store =
                repository(
                        (method, arguments) ->
                                switch (method) {
                                    case "list" -> {
                                        int index = scans.getAndIncrement();
                                        assertEquals(
                                                index == 0
                                                        ? Optional.empty()
                                                        : Optional.of(
                                                                new ZLinkAuthorityScanCursor(
                                                                        "next")),
                                                arguments[1]);
                                        yield CompletableFuture.completedFuture(
                                                index == 0
                                                        ? new ZLinkAuthorityPage(
                                                                List.of(),
                                                                Optional.of(
                                                                        new ZLinkAuthorityScanCursor(
                                                                                "next")))
                                                        : new ZLinkAuthorityPage(
                                                                List.of(
                                                                        new ZLinkAuthorityEntry(
                                                                                ZLinkAuthorityKeyCodec
                                                                                        .spot(
                                                                                                "room"),
                                                                                readySpotSnapshot())),
                                                                Optional.empty()));
                                    }
                                    case "readOwnerLease" ->
                                            CompletableFuture.completedFuture(
                                                    new ZLinkOwnerLeaseMissing());
                                    default -> throw new UnsupportedOperationException(method);
                                });
        var stores = ZLinkRegisteredLocationStores.fromUnified(store);
        try (var runtime =
                new ZLinkLocationRuntime(stores, Duration.ofSeconds(30), Duration.ofSeconds(10))) {
            var query =
                    new ZLinkLocationRuntimeQueryService(
                            stores, runtime, new ZLinkLocationOptions());
            var page =
                    query.listObjectLocations(
                                    ZLinkLocationObjectFilter.of(
                                            ZLinkPlacementObjectKind.USER_SPOT),
                                    ZLinkPageRequest.firstPage())
                            .toCompletableFuture()
                            .join();
            assertEquals(1, page.items().size());
            assertNull(page.continuationToken());
            assertEquals(2, scans.get());
        }
    }

    @Test
    void firstPageUsesTheQueryDefaultSize() {
        assertEquals(100, ZLinkPageRequest.firstPage().pageSize());
    }

    @Test
    void unspecifiedQueryPageSizeReturnsOneHundredObjects() {
        var reads = new AtomicInteger();
        ZLinkLocationRepository store =
                repository(
                        (method, arguments) ->
                                switch (method) {
                                    case "list" -> {
                                        int index = reads.getAndIncrement();
                                        yield CompletableFuture.completedFuture(
                                                new ZLinkAuthorityPage(
                                                        List.of(
                                                                new ZLinkAuthorityEntry(
                                                                        ZLinkAuthorityKeyCodec.spot(
                                                                                "room-" + index),
                                                                        readySpotSnapshot())),
                                                        index < 100
                                                                ? Optional.of(
                                                                        new ZLinkAuthorityScanCursor(
                                                                                "cursor-" + index))
                                                                : Optional.empty()));
                                    }
                                    case "readOwnerLease" ->
                                            CompletableFuture.completedFuture(
                                                    new ZLinkOwnerLeaseMissing());
                                    default -> throw new UnsupportedOperationException(method);
                                });
        var stores = ZLinkRegisteredLocationStores.fromUnified(store);
        try (var runtime =
                new ZLinkLocationRuntime(stores, Duration.ofSeconds(30), Duration.ofSeconds(10))) {
            var query =
                    new ZLinkLocationRuntimeQueryService(
                            stores, runtime, new ZLinkLocationOptions());
            var page =
                    query.listObjectLocations(
                                    ZLinkLocationObjectFilter.of(
                                            ZLinkPlacementObjectKind.USER_SPOT),
                                    new ZLinkPageRequest(0, null))
                            .toCompletableFuture()
                            .join();
            assertEquals(100, page.items().size());
            assertNotNull(page.continuationToken());
            assertEquals(100, reads.get());
        }
    }

    @Test
    void positiveReadyAuthorityIsCachedButMissingAuthorityIsNot() {
        AtomicInteger reads = new AtomicInteger();
        AtomicReference<Object> current = new AtomicReference<>(readySpotSnapshot());
        ZLinkLocationRepository store =
                (ZLinkLocationRepository)
                        Proxy.newProxyInstance(
                                getClass().getClassLoader(),
                                new Class<?>[] {ZLinkLocationRepository.class},
                                (proxy, method, arguments) ->
                                        switch (method.getName()) {
                                            case "read" -> {
                                                reads.incrementAndGet();
                                                yield CompletableFuture.completedFuture(
                                                        current.get());
                                            }
                                            case "readOwnerLease" ->
                                                    CompletableFuture.completedFuture(
                                                            new ZLinkOwnerLeaseFound(
                                                                    new ZLinkLocationOwnerToken(
                                                                            "owner-a",
                                                                            ((ZLinkAuthoritySnapshot)
                                                                                            current
                                                                                                    .get())
                                                                                    .ownerLeaseGeneration()),
                                                                    NOW.plusSeconds(30),
                                                                    NOW));
                                            default ->
                                                    throw new UnsupportedOperationException(
                                                            method.getName());
                                        });
        ZLinkLocationOptions cachedOptions = new ZLinkLocationOptions();
        cachedOptions.setRouteCacheMaxAge(Duration.ofSeconds(10));
        var cached =
                new ZLinkStoreLocationResolvers(
                        ZLinkRegisteredLocationStores.fromUnified(store), cachedOptions);

        assertEquals("room-a", cached.resolveSpot("room-a").toCompletableFuture().join().spotId());
        assertEquals("room-a", cached.resolveSpot("room-a").toCompletableFuture().join().spotId());
        assertEquals(1, reads.get());

        ZLinkLocationOptions uncachedOptions = new ZLinkLocationOptions();
        uncachedOptions.setRouteCacheMaxAge(Duration.ZERO);
        current.set(new ZLinkAuthorityMissing(NOW));
        var misses =
                new ZLinkStoreLocationResolvers(
                        ZLinkRegisteredLocationStores.fromUnified(store), uncachedOptions);
        assertNull(misses.resolveSpot("room-a").toCompletableFuture().join());
        assertNull(misses.resolveSpot("room-a").toCompletableFuture().join());
        assertEquals(3, reads.get());
    }

    @Test
    void expiredExactOwnerLeaseRejectsAnOtherwiseReadyRoute() {
        ZLinkLocationRepository store =
                (ZLinkLocationRepository)
                        Proxy.newProxyInstance(
                                getClass().getClassLoader(),
                                new Class<?>[] {ZLinkLocationRepository.class},
                                (proxy, method, arguments) ->
                                        switch (method.getName()) {
                                            case "read" ->
                                                    CompletableFuture.completedFuture(
                                                            readySpotSnapshot());
                                            case "readOwnerLease" ->
                                                    CompletableFuture.completedFuture(
                                                            new ZLinkOwnerLeaseMissing());
                                            default ->
                                                    throw new UnsupportedOperationException(
                                                            method.getName());
                                        });
        var resolvers =
                new ZLinkStoreLocationResolvers(
                        ZLinkRegisteredLocationStores.fromUnified(store),
                        new ZLinkLocationOptions());

        CompletionException failure =
                assertThrows(
                        CompletionException.class,
                        () -> resolvers.resolveSpot("room-a").toCompletableFuture().join());
        assertEquals(
                ZLinkFrameworkErrorKind.UNAVAILABLE,
                assertInstanceOf(ZLinkFrameworkException.class, failure.getCause()).kind());
    }

    @Test
    void exactLookupProjectsExpiredReadyOwnerAsUnavailable() {
        ZLinkLocationRepository store =
                repository(
                        (method, arguments) ->
                                switch (method) {
                                    case "read" ->
                                            CompletableFuture.completedFuture(readySpotSnapshot());
                                    case "readOwnerLease" ->
                                            CompletableFuture.completedFuture(
                                                    new ZLinkOwnerLeaseMissing());
                                    default -> throw new UnsupportedOperationException(method);
                                });
        ZLinkRegisteredLocationStores stores = ZLinkRegisteredLocationStores.fromUnified(store);
        ZLinkLocationOptions options = new ZLinkLocationOptions();
        try (ZLinkLocationRuntime runtime =
                new ZLinkLocationRuntime(stores, Duration.ofSeconds(30), Duration.ofSeconds(10))) {
            var query = new ZLinkLocationRuntimeQueryService(stores, runtime, options);

            ZLinkLocationObjectEntry entry =
                    query.findSpotLocation("room-a").toCompletableFuture().join().orElseThrow();

            assertEquals(ZLinkLocationObjectState.UNAVAILABLE, entry.state());
        }
    }

    @Test
    void exactLookupMapsStoreFailureToUnavailableFrameworkError() {
        ZLinkLocationRepository store =
                repository(
                        (method, arguments) -> {
                            if (method.equals("read")) {
                                return CompletableFuture.failedFuture(
                                        new IllegalStateException("store unavailable"));
                            }
                            throw new UnsupportedOperationException(method);
                        });
        ZLinkRegisteredLocationStores stores = ZLinkRegisteredLocationStores.fromUnified(store);
        try (ZLinkLocationRuntime runtime =
                new ZLinkLocationRuntime(stores, Duration.ofSeconds(30), Duration.ofSeconds(10))) {
            var query =
                    new ZLinkLocationRuntimeQueryService(
                            stores, runtime, new ZLinkLocationOptions());

            CompletionException failure =
                    assertThrows(
                            CompletionException.class,
                            () -> query.findSpotLocation("room-a").toCompletableFuture().join());

            assertEquals(
                    ZLinkFrameworkErrorKind.UNAVAILABLE,
                    assertInstanceOf(ZLinkFrameworkException.class, failure.getCause()).kind());
        }
    }

    @Test
    void messageFollowInvalidatesOnlyTheExactCachedRouteFence() {
        AtomicInteger reads = new AtomicInteger();
        AtomicReference<Object> current = new AtomicReference<>(readySpotSnapshot());
        ZLinkLocationRepository store =
                (ZLinkLocationRepository)
                        Proxy.newProxyInstance(
                                getClass().getClassLoader(),
                                new Class<?>[] {ZLinkLocationRepository.class},
                                (proxy, method, arguments) ->
                                        switch (method.getName()) {
                                            case "read" -> {
                                                reads.incrementAndGet();
                                                yield CompletableFuture.completedFuture(
                                                        current.get());
                                            }
                                            case "readOwnerLease" ->
                                                    CompletableFuture.completedFuture(
                                                            new ZLinkOwnerLeaseFound(
                                                                    new ZLinkLocationOwnerToken(
                                                                            "owner-a", 7),
                                                                    NOW.plusSeconds(30),
                                                                    NOW));
                                            default ->
                                                    throw new UnsupportedOperationException(
                                                            method.getName());
                                        });
        ZLinkLocationOptions options = new ZLinkLocationOptions();
        options.setRouteCacheMaxAge(Duration.ofSeconds(10));
        var resolvers =
                new ZLinkStoreLocationResolvers(
                        ZLinkRegisteredLocationStores.fromUnified(store), options);

        resolvers.resolveSpot("room-a").toCompletableFuture().join();
        var oldRoute =
                new ZLinkServiceMessageFollowWireCodec.SpotRoute("room-a", 5, NODE, 11, 13, 7);
        var wrongLease =
                new ZLinkServiceMessageFollowWireCodec.SpotRoute("room-a", 5, NODE, 11, 13, 8);
        assertFalse(resolvers.invalidateRouteIfMatches(wrongLease));
        assertEquals(1, reads.get());

        assertTrue(resolvers.invalidateRouteIfMatches(oldRoute));
        current.set(readySpotSnapshot(9, 21, 7, 17, "v2"));
        resolvers.resolveSpot("room-a").toCompletableFuture().join();
        assertEquals(2, reads.get());

        // A delayed notice from the previous owner must not erase v2.
        assertFalse(resolvers.invalidateRouteIfMatches(oldRoute));
        assertEquals(
                9, resolvers.resolveSpot("room-a").toCompletableFuture().join().spotGeneration());
        assertEquals(2, reads.get());
    }

    @Test
    void actorABARoundTripRejectsDelayedNoticeFromPreviousTenure() {
        RoutingId nodeB = RoutingId.from("node-b");
        AtomicInteger reads = new AtomicInteger();
        AtomicReference<Object> current =
                new AtomicReference<>(readyActorSnapshot("v1", NODE, 11, 13, "owner-a", 7));
        ZLinkLocationRepository store =
                (ZLinkLocationRepository)
                        Proxy.newProxyInstance(
                                getClass().getClassLoader(),
                                new Class<?>[] {ZLinkLocationRepository.class},
                                (proxy, method, arguments) ->
                                        switch (method.getName()) {
                                            case "read" -> {
                                                reads.incrementAndGet();
                                                yield CompletableFuture.completedFuture(
                                                        current.get());
                                            }
                                            case "readOwnerLease" -> {
                                                ZLinkAuthoritySnapshot snapshot =
                                                        (ZLinkAuthoritySnapshot) current.get();
                                                yield CompletableFuture.completedFuture(
                                                        new ZLinkOwnerLeaseFound(
                                                                new ZLinkLocationOwnerToken(
                                                                        snapshot.ownerId(),
                                                                        snapshot
                                                                                .ownerLeaseGeneration()),
                                                                NOW.plusSeconds(30),
                                                                NOW));
                                            }
                                            default ->
                                                    throw new UnsupportedOperationException(
                                                            method.getName());
                                        });
        ZLinkLocationOptions options = new ZLinkLocationOptions();
        options.setRouteCacheMaxAge(Duration.ofSeconds(10));
        var resolvers =
                new ZLinkStoreLocationResolvers(
                        ZLinkRegisteredLocationStores.fromUnified(store), options);

        var firstA = resolvers.resolveActor("actor-a").toCompletableFuture().join();
        var firstNotice =
                actorNotice(
                        actorRoute(firstA),
                        new ZLinkServiceMessageFollowWireCodec.ActorRoute(
                                "actor-a", 5, nodeB, 19, 15, 9));
        assertEquals(1, reads.get());

        current.set(readyActorSnapshot("v2", nodeB, 19, 15, "owner-b", 9));
        assertTrue(resolvers.invalidateRouteIfMatches(firstNotice));
        var atB = resolvers.resolveActor("actor-a").toCompletableFuture().join();
        assertEquals(nodeB, atB.nodeRid());
        assertEquals(2, reads.get());

        var returnNotice =
                actorNotice(
                        actorRoute(atB),
                        new ZLinkServiceMessageFollowWireCodec.ActorRoute(
                                "actor-a", 5, NODE, 11, 17, 7));
        current.set(readyActorSnapshot("v3", NODE, 11, 17, "owner-a", 7));
        assertTrue(resolvers.invalidateRouteIfMatches(returnNotice));
        var secondA = resolvers.resolveActor("actor-a").toCompletableFuture().join();
        assertEquals(NODE, secondA.nodeRid());
        assertEquals(17, secondA.authorityOwnerGeneration());
        assertEquals(3, reads.get());

        // The first A tenure has the same node and lease as the current A
        // tenure. Its older authority generation must still make the delayed
        // notice ineligible to mutate the current cached route.
        assertFalse(resolvers.invalidateRouteIfMatches(firstNotice));
        assertEquals(
                17,
                resolvers
                        .resolveActor("actor-a")
                        .toCompletableFuture()
                        .join()
                        .authorityOwnerGeneration());
        assertEquals(3, reads.get());
    }

    @Test
    void foreignActorPayloadUsesCanonicalOuterAuthorityRoute() {
        RoutingId foreignNode = RoutingId.from("dotnet-owner");
        ZLinkAuthoritySnapshot snapshot =
                new ZLinkAuthoritySnapshot(
                        "foreign-v1",
                        new byte[] {0x44, 0x4f, 0x54, 0x4e, 0x45, 0x54},
                        29,
                        31,
                        "foreign-owner",
                        37,
                        new ZLinkPlacementAllocation(
                                ZLinkPlacementAllocationState.ACTIVE,
                                ZLinkPlacementObjectKind.ACTOR,
                                "PlayerActor",
                                new ZLinkMeshNodeDescriptorKey("foreign-mesh", foreignNode),
                                41,
                                ZLinkPlacementCapacityBundle.actor(1)),
                        NOW);
        ZLinkLocationRepository store =
                repository(
                        (method, arguments) ->
                                switch (method) {
                                    case "read" -> CompletableFuture.completedFuture(snapshot);
                                    case "readOwnerLease" ->
                                            CompletableFuture.completedFuture(
                                                    new ZLinkOwnerLeaseFound(
                                                            new ZLinkLocationOwnerToken(
                                                                    "foreign-owner", 37),
                                                            NOW.plusSeconds(30),
                                                            NOW));
                                    default -> throw new UnsupportedOperationException(method);
                                });
        var resolvers =
                new ZLinkStoreLocationResolvers(
                        ZLinkRegisteredLocationStores.fromUnified(store),
                        new ZLinkLocationOptions());

        var route = resolvers.resolveActor("foreign-actor").toCompletableFuture().join();

        assertEquals("foreign-actor", route.actorRef().actorId());
        assertEquals(29, route.actorRef().objectGeneration());
        assertEquals("foreign-mesh", route.meshName());
        assertEquals(foreignNode, route.nodeRid());
        assertEquals(41, route.targetNodeGeneration());
        assertEquals(ZLinkSpotKind.ENTRY, route.locationKind());
        assertEquals("", route.spotId());
        assertEquals(31, route.authorityOwnerGeneration());
        assertEquals(37, route.ownerLeaseGeneration());
    }

    @Test
    void sameEndpointUsesRidAndOwnerLease() {
        ZLinkMeshNodeDescriptor live =
                meshNodeDescriptor("mesh", RoutingId.from("live"), "live-owner");
        ZLinkMeshNodeDescriptor expired =
                meshNodeDescriptor("mesh", RoutingId.from("expired"), "expired-owner");
        ZLinkLocationRepository store =
                (ZLinkLocationRepository)
                        Proxy.newProxyInstance(
                                getClass().getClassLoader(),
                                new Class<?>[] {ZLinkLocationRepository.class},
                                (proxy, method, arguments) ->
                                        switch (method.getName()) {
                                            case "listMeshNodes" ->
                                                    CompletableFuture.completedFuture(
                                                            new ZLinkLocationPage<>(
                                                                    List.of(live, expired), null));
                                            case "readOwnerLease" ->
                                                    "live-owner".equals(arguments[0])
                                                            ? CompletableFuture.completedFuture(
                                                                    new ZLinkOwnerLeaseFound(
                                                                            new ZLinkLocationOwnerToken(
                                                                                    "live-owner",
                                                                                    1),
                                                                            NOW.plusSeconds(30),
                                                                            NOW))
                                                            : CompletableFuture.completedFuture(
                                                                    new ZLinkOwnerLeaseFound(
                                                                            new ZLinkLocationOwnerToken(
                                                                                    "expired-owner",
                                                                                    1),
                                                                            NOW,
                                                                            NOW));
                                            default ->
                                                    throw new UnsupportedOperationException(
                                                            method.getName());
                                        });
        var resolvers =
                new ZLinkStoreLocationResolvers(
                        ZLinkRegisteredLocationStores.fromUnified(store),
                        new ZLinkLocationOptions());

        var peers =
                resolvers
                        .listPeers(
                                ZLinkAutoConnectType.ROUTE_MESH, "mesh", ZLinkLocationRole.ROUTER)
                        .toCompletableFuture()
                        .join();

        assertEquals(
                List.of(RoutingId.from("live")),
                peers.stream().map(ZLinkAutoConnectPeer::nodeRid).toList());
        var local =
                new ZLinkAutoConnectPlanner.Local(
                        ZLinkAutoConnectType.ROUTE_MESH,
                        "mesh",
                        ZLinkLocationRole.ROUTER,
                        RoutingId.from("aaa-local"),
                        live.endpoint());
        assertEquals(1, ZLinkAutoConnectPlanner.computeDesired(local, peers).size());
        var self =
                new ZLinkAutoConnectPlanner.Local(
                        ZLinkAutoConnectType.ROUTE_MESH,
                        "mesh",
                        ZLinkLocationRole.ROUTER,
                        live.rid(),
                        live.endpoint());
        assertEquals("self", ZLinkAutoConnectPlanner.decideAll(self, peers).get(0).skipReason());
    }

    private static ZLinkMeshNodeDescriptor meshNodeDescriptor(
            String meshName, RoutingId rid, String ownerId) {
        return new ZLinkMeshNodeDescriptor(
                meshName,
                rid,
                1,
                1,
                "tcp://127.0.0.1:7000",
                Map.of("orders", 100),
                1,
                List.of(),
                ZLinkMeshNodeObjectRole.NONE,
                Optional.empty(),
                100,
                new ZLinkPlacementCapacity(
                        new ZLinkCapacityUsage(0, 0, 0),
                        new ZLinkCapacityUsage(0, 0, 0),
                        List.of()),
                new ZLinkActivationConcurrency(0, 1),
                Optional.empty(),
                ZLinkFrameworkRuntimeState.SERVING,
                "security",
                ownerId,
                1,
                NOW);
    }

    private static ZLinkLocationRepository repository(
            BiFunction<String, Object[], Object> invocation) {
        return (ZLinkLocationRepository)
                Proxy.newProxyInstance(
                        ZLinkStoreLocationResolversTest.class.getClassLoader(),
                        new Class<?>[] {ZLinkLocationRepository.class},
                        (proxy, method, arguments) ->
                                invocation.apply(
                                        method.getName(),
                                        arguments == null ? new Object[0] : arguments));
    }

    private static ZLinkAuthoritySnapshot readySpotSnapshot() {
        return readySpotSnapshot(5, 13, 7, 11, "v1");
    }

    private static ZLinkAuthoritySnapshot readySpotSnapshot(
            long objectGeneration,
            long authorityOwnerGeneration,
            long ownerLeaseGeneration,
            long nodeGeneration,
            String storeVersion) {
        byte[] payload =
                new ZLinkServiceAuthorityPayloadCodec()
                        .encodeUser(
                                ZLinkServiceAuthorityPayloadCodec.State.READY,
                                "RoomSpot",
                                "room-a",
                                "owner-a",
                                ownerLeaseGeneration,
                                "game",
                                NODE,
                                nodeGeneration);
        return new ZLinkAuthoritySnapshot(
                storeVersion,
                payload,
                objectGeneration,
                authorityOwnerGeneration,
                "owner-a",
                ownerLeaseGeneration,
                new ZLinkPlacementAllocation(
                        ZLinkPlacementAllocationState.ACTIVE,
                        ZLinkPlacementObjectKind.USER_SPOT,
                        "RoomSpot",
                        new ZLinkMeshNodeDescriptorKey("game", NODE),
                        nodeGeneration,
                        ZLinkPlacementCapacityBundle.spot(
                                ZLinkPlacementObjectKind.USER_SPOT, "RoomSpot", 1)),
                Optional.empty(),
                NOW);
    }

    private static ZLinkAuthoritySnapshot readyActorSnapshot(
            String storeVersion,
            RoutingId nodeRid,
            long nodeGeneration,
            long authorityOwnerGeneration,
            String ownerId,
            long ownerLeaseGeneration) {
        byte[] payload =
                new ZLinkActorAuthorityPayloadCodec()
                        .encode(
                                ZLinkActorAuthorityPayloadCodec.State.READY,
                                "PlayerActor",
                                "actor-a",
                                "room-a",
                                5,
                                2,
                                ownerId,
                                ownerLeaseGeneration,
                                "game",
                                nodeRid,
                                nodeGeneration);
        return new ZLinkAuthoritySnapshot(
                storeVersion,
                payload,
                5,
                authorityOwnerGeneration,
                ownerId,
                ownerLeaseGeneration,
                new ZLinkPlacementAllocation(
                        ZLinkPlacementAllocationState.ACTIVE,
                        ZLinkPlacementObjectKind.ACTOR,
                        "PlayerActor",
                        new ZLinkMeshNodeDescriptorKey("game", nodeRid),
                        nodeGeneration,
                        ZLinkPlacementCapacityBundle.actor(1)),
                NOW);
    }

    private static ZLinkServiceMessageFollowWireCodec.ActorRoute actorRoute(
            ZLinkStoreLocationResolvers.ActorRoute route) {
        return new ZLinkServiceMessageFollowWireCodec.ActorRoute(
                route.actorRef().actorId(),
                route.actorRef().objectGeneration(),
                route.nodeRid(),
                route.targetNodeGeneration(),
                route.authorityOwnerGeneration(),
                route.ownerLeaseGeneration());
    }

    private static ZLinkServiceMessageFollowWireCodec.Notice actorNotice(
            ZLinkServiceMessageFollowWireCodec.ActorRoute source,
            ZLinkServiceMessageFollowWireCodec.ActorRoute target) {
        return new ZLinkServiceMessageFollowWireCodec.Notice(source, target, 1, 1, 1, 31, 37, 0);
    }
}
