package systems.zlink.framework.runtime.host;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertInstanceOf;
import static org.junit.jupiter.api.Assertions.assertThrows;
import static org.junit.jupiter.api.Assertions.assertTrue;

import org.junit.jupiter.api.Test;
import org.junit.jupiter.params.ParameterizedTest;
import org.junit.jupiter.params.provider.CsvSource;
import org.junit.jupiter.params.provider.ValueSource;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.locationprovider.ZLinkLocationStore;
import systems.zlink.framework.locationprovider.ZLinkStoreCancellation;
import systems.zlink.framework.locationprovider.ZLinkStoreKey;
import systems.zlink.framework.locationprovider.ZLinkStorePut;
import systems.zlink.framework.locationprovider.ZLinkStoreReadResult;
import systems.zlink.framework.locationprovider.ZLinkStoreScanRequest;
import systems.zlink.framework.locationprovider.ZLinkStoreScanResult;
import systems.zlink.framework.locationprovider.ZLinkStoreWriteRequest;
import systems.zlink.framework.locationprovider.ZLinkStoreWriteResult;
import systems.zlink.framework.locations.ZLinkActivationConcurrency;
import systems.zlink.framework.locations.ZLinkCapacityUsage;
import systems.zlink.framework.locations.ZLinkMeshNodeObjectRole;
import systems.zlink.framework.locations.ZLinkPageRequest;
import systems.zlink.framework.locations.ZLinkPlacementCapacity;
import systems.zlink.framework.runtime.binding.ZLinkJavaBackendAdapterFactory;
import systems.zlink.framework.runtime.configuration.DefaultZLinkFrameworkOptions;
import systems.zlink.framework.runtime.internal.locations.ZLinkLocationWriteIntent;
import systems.zlink.framework.runtime.internal.locations.ZLinkLocationWriteStatus;
import systems.zlink.framework.runtime.internal.locations.ZLinkMeshNodeDescriptor;
import systems.zlink.framework.runtime.internal.locations.ZLinkOwnerLeaseClaimed;
import systems.zlink.framework.runtime.internal.locations.ZLinkProviderLocationRepository;
import systems.zlink.framework.runtime.locations.ZLinkInMemoryProviderLocationStore;

import java.nio.charset.StandardCharsets;
import java.time.Duration;
import java.time.Instant;
import java.util.List;
import java.util.Map;
import java.util.Optional;
import java.util.UUID;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.ExecutionException;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.TimeoutException;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.concurrent.atomic.AtomicReference;

/**
 * Location runtime §5 and §10: a host stays Preparing until its descriptor publication is
 * confirmed. The renew interval is longer than each test, so every renewal is the one the test
 * runs; no assertion depends on elapsed time.
 */
final class ZLinkStartupDescriptorPublicationTest {
    private static final long WAIT_SECONDS = 30;

    @Test
    void unavailableStoreKeepsHostPreparingUntilARenewalPublishesServing() throws Exception {
        UnavailableStore store = new UnavailableStore();
        try (ZLinkFrameworkRuntime runtime =
                start(options(store, "unavailable-store", Duration.ofMillis(300)))) {
            ZLinkFrameworkRuntimeTestAccess.startupCompletion(runtime)
                    .toCompletableFuture()
                    .get(WAIT_SECONDS, TimeUnit.SECONDS);

            assertEquals(ZLinkFrameworkRuntimeState.PREPARING, runtime.state());
            assertFalse(runtime.isReady());
            assertTrue(descriptors(store.inner).isEmpty());

            store.available.set(true);
            assertTrue(renew(runtime));

            assertEquals(ZLinkFrameworkRuntimeState.SERVING, runtime.state());
            assertEquals(
                    ZLinkFrameworkRuntimeState.SERVING,
                    descriptors(store.inner).getFirst().state());
        }
    }

    @ParameterizedTest
    @CsvSource({
        "PREPARING, true, false", "PREPARING, false, false", "PREPARING, false, true",
        "SERVING, true, false", "SERVING, false, false", "SERVING, false, true"
    })
    void unconfirmedPublicationIsRetriedOnRenewalThenServes(
            ZLinkFrameworkRuntimeState failedState,
            boolean applied,
            boolean confirmingReadAvailable)
            throws Exception {
        LostDescriptorResponseStore store =
                new LostDescriptorResponseStore(failedState, applied, confirmingReadAvailable);
        var options = options(store, "lost-publication", Duration.ofSeconds(10));
        // One claim succeeds while the other is unconfirmed; retry must confirm both claims.
        options.addRouteMesh("other")
                .listen("inproc://other-" + UUID.randomUUID())
                .setRoutingId(RoutingId.from("other"));
        try (ZLinkFrameworkRuntime runtime = start(options)) {
            ZLinkFrameworkRuntimeTestAccess.startupCompletion(runtime)
                    .toCompletableFuture()
                    .get(WAIT_SECONDS, TimeUnit.SECONDS);

            assertEquals(ZLinkFrameworkRuntimeState.PREPARING, runtime.state());
            assertFalse(runtime.isReady());
            if (applied || failedState == ZLinkFrameworkRuntimeState.SERVING) {
                assertEquals(
                        applied ? failedState : ZLinkFrameworkRuntimeState.PREPARING,
                        descriptors(store.inner).getFirst().state());
            } else {
                assertTrue(descriptors(store.inner).isEmpty());
            }
            assertFalse(renew(runtime));
            assertEquals(ZLinkFrameworkRuntimeState.PREPARING, runtime.state());
            assertFalse(runtime.isReady());

            store.losing.set(false);
            assertTrue(renew(runtime));

            assertEquals(ZLinkFrameworkRuntimeState.SERVING, runtime.state());
            for (String mesh : List.of("game", "other")) {
                var rows =
                        new ZLinkProviderLocationRepository(store.inner)
                                .listMeshNodes(mesh, ZLinkPageRequest.firstPage())
                                .toCompletableFuture()
                                .get(WAIT_SECONDS, TimeUnit.SECONDS)
                                .items();
                assertEquals(1, rows.size(), mesh);
                assertEquals(ZLinkFrameworkRuntimeState.SERVING, rows.getFirst().state(), mesh);
            }
            assertTrue(runtime.isReady());
        }
    }

    @ParameterizedTest
    @ValueSource(booleans = {true, false})
    void providerReadFailureAfterClaimKeepsHostPreparing(boolean synchronous) throws Exception {
        var store = new ReadFailureAfterClaimStore(synchronous);
        try (var runtime = start(options(store, "read-failure", Duration.ofSeconds(10)))) {
            try {
                ZLinkFrameworkRuntimeTestAccess.startupCompletion(runtime)
                        .toCompletableFuture()
                        .get(WAIT_SECONDS, TimeUnit.SECONDS);
                assertEquals(ZLinkFrameworkRuntimeState.PREPARING, runtime.state());
                assertFalse(runtime.isReady());
                store.failing.set(false);
                assertTrue(renew(runtime));
                assertEquals(ZLinkFrameworkRuntimeState.SERVING, runtime.state());
                assertTrue(runtime.isReady());
            } finally {
                store.failing.set(false);
            }
        }
    }

    @Test
    void lostWriteResponseIsConfirmedByReadingTheRowWithoutWaitingForRenewal() throws Exception {
        var store = new LostDescriptorResponseStore(null, true, true);
        try (var runtime = start(options(store, "confirmed-write", Duration.ofSeconds(10)))) {
            ZLinkFrameworkRuntimeTestAccess.startupCompletion(runtime)
                    .toCompletableFuture()
                    .get(WAIT_SECONDS, TimeUnit.SECONDS);
            assertEquals(ZLinkFrameworkRuntimeState.SERVING, runtime.state());
            assertEquals(
                    ZLinkFrameworkRuntimeState.SERVING,
                    descriptors(store.inner).getFirst().state());
            assertTrue(runtime.isReady());
        }
    }

    @Test
    void invalidDescriptorRequestRemainsAStartupError() throws Exception {
        var store =
                new LostDescriptorResponseStore(ZLinkFrameworkRuntimeState.PREPARING, false, false);
        store.failure = new IllegalArgumentException("invalid descriptor request");
        try (var runtime = start(options(store, "invalid-publication", Duration.ofSeconds(10)))) {
            var failure =
                    assertThrows(
                            ExecutionException.class,
                            () ->
                                    ZLinkFrameworkRuntimeTestAccess.startupCompletion(runtime)
                                            .toCompletableFuture()
                                            .get(WAIT_SECONDS, TimeUnit.SECONDS));
            assertInstanceOf(IllegalArgumentException.class, failure.getCause());
            assertEquals(ZLinkFrameworkRuntimeState.ERROR, runtime.state());
            store.losing.set(false);
        }
    }

    @Test
    void anotherOwnersDescriptorFailsStartupAfterTheLeaseArrives() throws Exception {
        UnavailableStore store = new UnavailableStore();
        holdDescriptorForAnotherOwner(store.inner, "taken-late");
        try (ZLinkFrameworkRuntime runtime =
                start(options(store, "taken-late", Duration.ofSeconds(10)))) {
            ZLinkFrameworkRuntimeTestAccess.startupCompletion(runtime)
                    .toCompletableFuture()
                    .get(WAIT_SECONDS, TimeUnit.SECONDS);
            assertEquals(ZLinkFrameworkRuntimeState.PREPARING, runtime.state());

            store.available.set(true);
            renew(runtime);

            assertEquals(ZLinkFrameworkRuntimeState.ERROR, runtime.state());
            assertEquals("other-owner", descriptors(store.inner).getFirst().ownerId());
        }
    }

    @Test
    void anotherOwnersDescriptorFailsStartupWithTheLease() throws Exception {
        UnavailableStore store = new UnavailableStore();
        store.available.set(true);
        holdDescriptorForAnotherOwner(store.inner, "taken-now");
        try (ZLinkFrameworkRuntime runtime =
                start(options(store, "taken-now", Duration.ofSeconds(10)))) {
            ExecutionException failure =
                    assertThrows(
                            ExecutionException.class,
                            () ->
                                    ZLinkFrameworkRuntimeTestAccess.startupCompletion(runtime)
                                            .toCompletableFuture()
                                            .get(WAIT_SECONDS, TimeUnit.SECONDS));
            assertTrue(failure.getCause().getMessage().contains("REJECTED_CONFLICT"));
            assertEquals(ZLinkFrameworkRuntimeState.ERROR, runtime.state());
        }
    }

    @Test
    void anotherOwnersConflictWinsOverAnUnconfirmedMeshPublication() throws Exception {
        var store =
                new LostDescriptorResponseStore(ZLinkFrameworkRuntimeState.PREPARING, false, false);
        holdDescriptorForAnotherOwner(store.inner, "other", "mixed-other");
        var options = options(store, "mixed-game", Duration.ofSeconds(10));
        options.addRouteMesh("other")
                .listen("inproc://mixed-other-" + UUID.randomUUID())
                .setRoutingId(RoutingId.from("mixed-other"));
        try (var runtime = start(options)) {
            try {
                var failure =
                        assertThrows(
                                ExecutionException.class,
                                () ->
                                        ZLinkFrameworkRuntimeTestAccess.startupCompletion(runtime)
                                                .toCompletableFuture()
                                                .get(WAIT_SECONDS, TimeUnit.SECONDS));
                assertTrue(failure.getCause().getMessage().contains("REJECTED_CONFLICT"));
                assertEquals(ZLinkFrameworkRuntimeState.ERROR, runtime.state());
            } finally {
                store.losing.set(false);
            }
        }
    }

    private static boolean renew(ZLinkFrameworkRuntime runtime) throws Exception {
        return ZLinkFrameworkRuntimeTestAccess.renewOwnerLeaseOnce(runtime)
                .toCompletableFuture()
                .get(WAIT_SECONDS, TimeUnit.SECONDS);
    }

    private static ZLinkFrameworkRuntime start(DefaultZLinkFrameworkOptions options) {
        return ZLinkFrameworkRuntimeTestAccess.start(options, new ZLinkJavaBackendAdapterFactory());
    }

    private static DefaultZLinkFrameworkOptions options(
            ZLinkLocationStore store, String routingId, Duration renewTimeout) {
        DefaultZLinkFrameworkOptions options = new DefaultZLinkFrameworkOptions();
        options.addLocationStore(store);
        options.configureLocations().setOwnerLeaseTtl(Duration.ofHours(1));
        options.configureLocations().setOwnerLeaseRenewInterval(Duration.ofMinutes(10));
        options.configureLocations().setOwnerLeaseRenewTimeout(renewTimeout);
        options.configureLocations().setOwnerLeaseFencingMargin(Duration.ofSeconds(1));
        options.addRouteMesh("game")
                .listen("inproc://" + routingId + "-" + UUID.randomUUID())
                .setRoutingId(RoutingId.from(routingId));
        return options;
    }

    private static List<ZLinkMeshNodeDescriptor> descriptors(ZLinkLocationStore store)
            throws Exception {
        return new ZLinkProviderLocationRepository(store)
                .listMeshNodes("game", ZLinkPageRequest.firstPage())
                .toCompletableFuture()
                .get(WAIT_SECONDS, TimeUnit.SECONDS)
                .items();
    }

    private static void holdDescriptorForAnotherOwner(ZLinkLocationStore store, String routingId)
            throws Exception {
        holdDescriptorForAnotherOwner(store, "game", routingId);
    }

    private static void holdDescriptorForAnotherOwner(
            ZLinkLocationStore store, String meshName, String routingId) throws Exception {
        var repository = new ZLinkProviderLocationRepository(store);
        var owner =
                assertInstanceOf(
                                ZLinkOwnerLeaseClaimed.class,
                                repository
                                        .claimOwnerLease("other-owner", Duration.ofHours(1))
                                        .toCompletableFuture()
                                        .get(WAIT_SECONDS, TimeUnit.SECONDS))
                        .token();
        var descriptor =
                new ZLinkMeshNodeDescriptor(
                        meshName,
                        RoutingId.from(routingId),
                        1,
                        1,
                        "tcp://127.0.0.1:7000",
                        Map.of(),
                        1,
                        List.of(),
                        ZLinkMeshNodeObjectRole.NONE,
                        Optional.empty(),
                        0,
                        new ZLinkPlacementCapacity(
                                new ZLinkCapacityUsage(0, 0, 100),
                                new ZLinkCapacityUsage(0, 0, 100),
                                List.of()),
                        new ZLinkActivationConcurrency(0, 64),
                        Optional.empty(),
                        ZLinkFrameworkRuntimeState.SERVING,
                        "other-security",
                        owner.ownerId(),
                        owner.leaseGeneration(),
                        Instant.parse("2026-10-16T00:00:00Z"));
        assertEquals(
                ZLinkLocationWriteStatus.STORED,
                repository
                        .updateMeshNode(descriptor, ZLinkLocationWriteIntent.NEW_CLAIM)
                        .toCompletableFuture()
                        .get(WAIT_SECONDS, TimeUnit.SECONDS)
                        .status());
    }

    /**
     * Fails selected MeshNode writes before or after applying them, and independently controls
     * whether their confirmation reads are available, while {@link #losing} is set.
     */
    private static final class LostDescriptorResponseStore implements ZLinkLocationStore {
        private final ZLinkInMemoryProviderLocationStore inner =
                new ZLinkInMemoryProviderLocationStore();
        private final AtomicBoolean losing = new AtomicBoolean(true);
        private final AtomicReference<ZLinkStoreKey> lostRow = new AtomicReference<>();
        private final ZLinkFrameworkRuntimeState failedState;
        private final boolean applied;
        private final boolean confirmingReadAvailable;
        private RuntimeException failure =
                new IllegalStateException("location store response lost");

        LostDescriptorResponseStore(
                ZLinkFrameworkRuntimeState failedState,
                boolean applied,
                boolean confirmingReadAvailable) {
            this.failedState = failedState;
            this.applied = applied;
            this.confirmingReadAvailable = confirmingReadAvailable;
        }

        @Override
        public CompletionStage<ZLinkStoreReadResult> read(
                ZLinkStoreKey key, ZLinkStoreCancellation cancellation) {
            if (losing.get() && !confirmingReadAvailable && key.equals(lostRow.get())) {
                return CompletableFuture.failedFuture(
                        new IllegalStateException("location store read unavailable"));
            }
            return inner.read(key, cancellation);
        }

        @Override
        public CompletionStage<ZLinkStoreWriteResult> write(
                ZLinkStoreWriteRequest request, ZLinkStoreCancellation cancellation) {
            if (!losing.get()
                    || !(request.mutations().getFirst() instanceof ZLinkStorePut put)
                    || !new String(put.bytes(), StandardCharsets.UTF_8)
                            .contains("\"meshName\":\"game\"")
                    || (failedState != null
                            && !new String(put.bytes(), StandardCharsets.UTF_8)
                                    .contains(
                                            "\"state\":\""
                                                    + (failedState
                                                                    == ZLinkFrameworkRuntimeState
                                                                            .PREPARING
                                                            ? "preparing"
                                                            : "serving")
                                                    + "\""))) {
                return inner.write(request, cancellation);
            }
            lostRow.set(put.key());
            return (applied
                            ? inner.write(request, cancellation)
                            : CompletableFuture.<ZLinkStoreWriteResult>completedFuture(null))
                    .thenCompose(ignored -> CompletableFuture.failedFuture(failure));
        }

        @Override
        public CompletionStage<ZLinkStoreScanResult> scan(
                ZLinkStoreScanRequest request, ZLinkStoreCancellation cancellation) {
            return inner.scan(request, cancellation);
        }
    }

    /** Fails reads after a successful owner claim, before descriptor publication starts. */
    private static final class ReadFailureAfterClaimStore implements ZLinkLocationStore {
        private final ZLinkInMemoryProviderLocationStore inner =
                new ZLinkInMemoryProviderLocationStore();
        private final AtomicBoolean claimed = new AtomicBoolean();
        private final AtomicBoolean failing = new AtomicBoolean(true);
        private final boolean synchronous;

        ReadFailureAfterClaimStore(boolean synchronous) {
            this.synchronous = synchronous;
        }

        @Override
        public CompletionStage<ZLinkStoreReadResult> read(
                ZLinkStoreKey key, ZLinkStoreCancellation cancellation) {
            if (claimed.get() && failing.get()) {
                if (synchronous) {
                    throw new IllegalStateException("provider read unavailable");
                }
                return CompletableFuture.failedFuture(
                        new TimeoutException("provider read timed out"));
            }
            return inner.read(key, cancellation);
        }

        @Override
        public CompletionStage<ZLinkStoreWriteResult> write(
                ZLinkStoreWriteRequest request, ZLinkStoreCancellation cancellation) {
            return inner.write(request, cancellation)
                    .thenApply(
                            result -> {
                                claimed.set(true);
                                return result;
                            });
        }

        @Override
        public CompletionStage<ZLinkStoreScanResult> scan(
                ZLinkStoreScanRequest request, ZLinkStoreCancellation cancellation) {
            return inner.scan(request, cancellation);
        }
    }

    /** Fails every request with a provider error until {@link #available} is set. */
    private static final class UnavailableStore implements ZLinkLocationStore {
        private final ZLinkInMemoryProviderLocationStore inner =
                new ZLinkInMemoryProviderLocationStore();
        private final AtomicBoolean available = new AtomicBoolean();

        @Override
        public CompletionStage<ZLinkStoreReadResult> read(
                ZLinkStoreKey key, ZLinkStoreCancellation cancellation) {
            return available.get() ? inner.read(key, cancellation) : unavailable();
        }

        @Override
        public CompletionStage<ZLinkStoreWriteResult> write(
                ZLinkStoreWriteRequest request, ZLinkStoreCancellation cancellation) {
            return available.get() ? inner.write(request, cancellation) : unavailable();
        }

        @Override
        public CompletionStage<ZLinkStoreScanResult> scan(
                ZLinkStoreScanRequest request, ZLinkStoreCancellation cancellation) {
            return available.get() ? inner.scan(request, cancellation) : unavailable();
        }

        private static <T> CompletionStage<T> unavailable() {
            return CompletableFuture.failedFuture(
                    new IllegalStateException("location store transport unavailable"));
        }
    }
}
