package systems.zlink.framework.runtime.host;

import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertTrue;

import java.time.Duration;
import java.util.UUID;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.function.BooleanSupplier;
import org.junit.jupiter.api.Test;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.locationprovider.ZLinkLocationStore;
import systems.zlink.framework.locationprovider.ZLinkStoreCancellation;
import systems.zlink.framework.locationprovider.ZLinkStoreKey;
import systems.zlink.framework.locationprovider.ZLinkStoreReadResult;
import systems.zlink.framework.locationprovider.ZLinkStoreScanRequest;
import systems.zlink.framework.locationprovider.ZLinkStoreScanResult;
import systems.zlink.framework.locationprovider.ZLinkStoreWriteRequest;
import systems.zlink.framework.locationprovider.ZLinkStoreWriteResult;
import systems.zlink.framework.locations.ZLinkPageRequest;
import systems.zlink.framework.runtime.binding.ZLinkJavaBackendAdapterFactory;
import systems.zlink.framework.runtime.configuration.DefaultZLinkFrameworkOptions;
import systems.zlink.framework.runtime.internal.locations
    .ZLinkProviderLocationRepository;
import systems.zlink.framework.runtime.locations.ZLinkInMemoryProviderLocationStore;

final class ZLinkLocationRuntimeHostStartupTest {
    @Test
    void degradedHostCompletesStartupBeforeOwnerLeaseRecovery()
        throws Exception {
        RecoveringLocationStore store = new RecoveringLocationStore();
        DefaultZLinkFrameworkOptions options = new DefaultZLinkFrameworkOptions();
        options.addLocationStore(store);
        options.configureLocations().setOwnerLeaseTtl(Duration.ofSeconds(1));
        options.configureLocations().setOwnerLeaseRenewInterval(
            Duration.ofMillis(20));
        options.configureLocations().setOwnerLeaseRenewTimeout(
            Duration.ofMillis(10));
        options.configureLocations().setOwnerLeaseFencingMargin(
            Duration.ofMillis(100));
        options.addRouteMesh("game")
            .listen("inproc://degraded-host-" + UUID.randomUUID())
            .setRoutingId(RoutingId.from("degraded-host"));

        try (ZLinkFrameworkRuntime runtime = ZLinkFrameworkRuntimeTestAccess.start(
            options,
            new ZLinkJavaBackendAdapterFactory())) {
            assertTrue(store.unavailableObserved.await(1, TimeUnit.SECONDS));
            ZLinkFrameworkRuntimeTestAccess.startupCompletion(runtime)
                .toCompletableFuture()
                .get(1, TimeUnit.SECONDS);
            await(() -> store.failedRequests.get() >= 3, Duration.ofSeconds(1));

            assertFalse(runtime.isReady());
            assertTrue(store.descriptors().isEmpty());

            store.available.set(true);

            await(() -> runtime.isReady() && !store.descriptors().isEmpty(),
                Duration.ofSeconds(2));
        }
    }

    private static void await(BooleanSupplier condition, Duration timeout)
        throws InterruptedException {
        long deadline = System.nanoTime() + timeout.toNanos();
        while (!condition.getAsBoolean() && System.nanoTime() < deadline) {
            Thread.sleep(1);
        }
        assertTrue(condition.getAsBoolean());
    }

    private static final class RecoveringLocationStore
        implements ZLinkLocationStore {
        private final ZLinkInMemoryProviderLocationStore inner =
            new ZLinkInMemoryProviderLocationStore();
        private final ZLinkProviderLocationRepository repository =
            new ZLinkProviderLocationRepository(inner);
        private final AtomicBoolean available = new AtomicBoolean();
        private final AtomicInteger failedRequests = new AtomicInteger();
        private final CountDownLatch unavailableObserved = new CountDownLatch(1);

        @Override
        public CompletionStage<ZLinkStoreReadResult> read(
            ZLinkStoreKey key,
            ZLinkStoreCancellation cancellation) {
            if (!available.get()) {
                return unavailable();
            }
            return inner.read(key, cancellation);
        }

        @Override
        public CompletionStage<ZLinkStoreWriteResult> write(
            ZLinkStoreWriteRequest request,
            ZLinkStoreCancellation cancellation) {
            if (!available.get()) {
                return unavailable();
            }
            return inner.write(request, cancellation);
        }

        @Override
        public CompletionStage<ZLinkStoreScanResult> scan(
            ZLinkStoreScanRequest request,
            ZLinkStoreCancellation cancellation) {
            if (!available.get()) {
                return unavailable();
            }
            return inner.scan(request, cancellation);
        }

        java.util.List<systems.zlink.framework.runtime.internal.locations
            .ZLinkMeshNodeDescriptor> descriptors() {
            return repository.listMeshNodes("game", ZLinkPageRequest.firstPage())
                .toCompletableFuture().join().items();
        }

        private <T> CompletionStage<T> unavailable() {
            failedRequests.incrementAndGet();
            unavailableObserved.countDown();
            return CompletableFuture.failedFuture(
                new IllegalStateException("location store transport unavailable"));
        }
    }
}
