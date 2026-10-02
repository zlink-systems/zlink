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
    void autoConnectPeerDiscoveryExcludesExpiredMeshOwnerLeases() {
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
                                                                    new ZLinkOwnerLeaseMissing());
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
