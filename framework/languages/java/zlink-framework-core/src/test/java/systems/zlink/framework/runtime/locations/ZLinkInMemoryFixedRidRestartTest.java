package systems.zlink.framework.runtime.locations;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertInstanceOf;
import static org.junit.jupiter.api.Assertions.assertTrue;

import org.junit.jupiter.api.RepeatedTest;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.locationprovider.ZLinkLocationStore;
import systems.zlink.framework.locations.ZLinkPageRequest;
import systems.zlink.framework.runtime.binding.ZLinkJavaBackendAdapterFactory;
import systems.zlink.framework.runtime.configuration.DefaultZLinkFrameworkOptions;
import systems.zlink.framework.runtime.host.ZLinkFrameworkRuntime;
import systems.zlink.framework.runtime.host.ZLinkFrameworkRuntimeState;
import systems.zlink.framework.runtime.host.ZLinkFrameworkRuntimeTestAccess;
import systems.zlink.framework.runtime.internal.handlers.ZLinkHandlerActivator;
import systems.zlink.framework.runtime.internal.locations.ZLinkLocationRepository;
import systems.zlink.framework.runtime.internal.locations.ZLinkLocationWriteResult;
import systems.zlink.framework.runtime.internal.locations.ZLinkLocationWriteStatus;
import systems.zlink.framework.runtime.internal.locations.ZLinkMeshNodeDescriptor;

import java.lang.reflect.InvocationTargetException;
import java.lang.reflect.Proxy;
import java.time.Clock;
import java.time.Duration;
import java.time.Instant;
import java.time.ZoneId;
import java.time.ZoneOffset;
import java.util.UUID;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.concurrent.atomic.AtomicLong;
import java.util.concurrent.atomic.AtomicReference;

final class ZLinkInMemoryFixedRidRestartTest {
    private static final long WAIT_SECONDS = 30;
    private static final Duration MARGIN = Duration.ofNanos(10);

    @RepeatedTest(100)
    void normalFixedRidRestartWaitsFromMissingObservation() throws Exception {
        var monotonicNanos = new AtomicLong();
        var store =
                new ZLinkInMemoryLocationStore(
                        Clock.fixed(Instant.parse("2026-10-16T00:00:00Z"), ZoneOffset.UTC),
                        monotonicNanos::get,
                        MARGIN);
        var successfulDescriptorMutations = new AtomicInteger();
        var repository = countingRepository(store, successfulDescriptorMutations);
        String routingId = "in-memory-fixed-rid-restart";

        try (var first = start(options(store, routingId), repository)) {
            ZLinkFrameworkRuntimeTestAccess.startupCompletion(first)
                    .toCompletableFuture()
                    .get(WAIT_SECONDS, TimeUnit.SECONDS);

            assertEquals(
                    ZLinkFrameworkRuntimeState.PREPARING,
                    ZLinkFrameworkRuntimeTestAccess.state(first));
            assertEquals(0, successfulDescriptorMutations.get());
            assertTrue(descriptors(store).isEmpty());

            monotonicNanos.set(MARGIN.multipliedBy(3).toNanos());
            assertTrue(renew(first));
            assertEquals(
                    ZLinkFrameworkRuntimeState.SERVING,
                    ZLinkFrameworkRuntimeTestAccess.state(first));
            assertTrue(successfulDescriptorMutations.get() > 0);
        }

        assertTrue(descriptors(store).isEmpty());
        int mutationsBeforeRestart = successfulDescriptorMutations.get();
        long restartObservedAt = monotonicNanos.get();
        try (var restarted = start(options(store, routingId), repository)) {
            ZLinkFrameworkRuntimeTestAccess.startupCompletion(restarted)
                    .toCompletableFuture()
                    .get(WAIT_SECONDS, TimeUnit.SECONDS);

            assertEquals(
                    ZLinkFrameworkRuntimeState.PREPARING,
                    ZLinkFrameworkRuntimeTestAccess.state(restarted));
            assertEquals(mutationsBeforeRestart, successfulDescriptorMutations.get());
            assertTrue(descriptors(store).isEmpty());

            monotonicNanos.set(restartObservedAt + MARGIN.multipliedBy(3).toNanos() - 1);
            assertTrue(renew(restarted));
            assertEquals(
                    ZLinkFrameworkRuntimeState.PREPARING,
                    ZLinkFrameworkRuntimeTestAccess.state(restarted));
            assertEquals(mutationsBeforeRestart, successfulDescriptorMutations.get());
            assertTrue(descriptors(store).isEmpty());

            monotonicNanos.incrementAndGet();
            assertTrue(renew(restarted));
            assertEquals(
                    ZLinkFrameworkRuntimeState.SERVING,
                    ZLinkFrameworkRuntimeTestAccess.state(restarted));
            assertTrue(restarted.isReady());
            assertTrue(successfulDescriptorMutations.get() > mutationsBeforeRestart);
            assertEquals(1, descriptors(store).size());
        }
    }

    @RepeatedTest(100)
    void servingFixedRidLeaseGenerationRecoveryStoresOnItsFirstRenew() throws Exception {
        var monotonicNanos = new AtomicLong();
        var clock = new MutableClock(Instant.parse("2026-11-06T00:00:00Z"));
        var store = new ZLinkInMemoryLocationStore(clock, monotonicNanos::get, MARGIN);
        var successfulDescriptorMutations = new AtomicInteger();
        var repository = countingRepository(store, successfulDescriptorMutations);

        try (var runtime = start(options(store, "in-memory-lease-recovery"), repository)) {
            ZLinkFrameworkRuntimeTestAccess.startupCompletion(runtime)
                    .toCompletableFuture()
                    .get(WAIT_SECONDS, TimeUnit.SECONDS);
            assertEquals(
                    ZLinkFrameworkRuntimeState.PREPARING,
                    ZLinkFrameworkRuntimeTestAccess.state(runtime));
            monotonicNanos.set(MARGIN.multipliedBy(3).toNanos());
            assertTrue(renew(runtime));
            assertEquals(
                    ZLinkFrameworkRuntimeState.SERVING,
                    ZLinkFrameworkRuntimeTestAccess.state(runtime));
            var previous =
                    assertInstanceOf(ZLinkMeshNodeDescriptor.class, descriptors(store).getFirst());
            int mutationsBeforeRecovery = successfulDescriptorMutations.get();

            clock.advance(Duration.ofHours(1));
            renew(runtime);

            var recovered =
                    assertInstanceOf(ZLinkMeshNodeDescriptor.class, descriptors(store).getFirst());
            assertEquals(
                    ZLinkFrameworkRuntimeState.SERVING,
                    ZLinkFrameworkRuntimeTestAccess.state(runtime));
            assertTrue(runtime.isReady());
            assertEquals(previous.ownerId(), recovered.ownerId());
            assertEquals(previous.lifecycleGeneration(), recovered.lifecycleGeneration());
            assertTrue(recovered.leaseGeneration() > previous.leaseGeneration());
            assertEquals(mutationsBeforeRecovery + 1, successfulDescriptorMutations.get());
        }
    }

    @RepeatedTest(100)
    void preparingFixedRidLeaseGenerationReplacementKeepsTheNewClaimWait() throws Exception {
        var monotonicNanos = new AtomicLong();
        var clock = new MutableClock(Instant.parse("2026-11-06T00:00:00Z"));
        var store = new ZLinkInMemoryLocationStore(clock, monotonicNanos::get, MARGIN);
        var successfulDescriptorMutations = new AtomicInteger();
        var repository = countingRepository(store, successfulDescriptorMutations);

        try (var runtime = start(options(store, "in-memory-preparing-recovery"), repository)) {
            ZLinkFrameworkRuntimeTestAccess.startupCompletion(runtime)
                    .toCompletableFuture()
                    .get(WAIT_SECONDS, TimeUnit.SECONDS);
            assertEquals(
                    ZLinkFrameworkRuntimeState.PREPARING,
                    ZLinkFrameworkRuntimeTestAccess.state(runtime));
            assertTrue(descriptors(store).isEmpty());
            monotonicNanos.set(MARGIN.multipliedBy(3).toNanos() - 1);

            clock.advance(Duration.ofHours(1));
            assertTrue(renew(runtime));
            assertEquals(
                    ZLinkFrameworkRuntimeState.PREPARING,
                    ZLinkFrameworkRuntimeTestAccess.state(runtime));
            assertTrue(descriptors(store).isEmpty());
            assertEquals(0, successfulDescriptorMutations.get());
            long replacementObservedAt = monotonicNanos.get();

            monotonicNanos.set(replacementObservedAt + MARGIN.multipliedBy(3).toNanos() - 1);
            assertTrue(renew(runtime));
            assertEquals(
                    ZLinkFrameworkRuntimeState.PREPARING,
                    ZLinkFrameworkRuntimeTestAccess.state(runtime));
            assertTrue(descriptors(store).isEmpty());
            assertEquals(0, successfulDescriptorMutations.get());

            monotonicNanos.incrementAndGet();
            assertTrue(renew(runtime));
            assertEquals(
                    ZLinkFrameworkRuntimeState.SERVING,
                    ZLinkFrameworkRuntimeTestAccess.state(runtime));
            assertTrue(successfulDescriptorMutations.get() > 0);
            assertFalse(descriptors(store).isEmpty());
        }
    }

    private static ZLinkFrameworkRuntime start(
            DefaultZLinkFrameworkOptions options, ZLinkLocationRepository repository) {
        return ZLinkFrameworkRuntimeTestAccess.startWithConstruction(
                options,
                new ZLinkJavaBackendAdapterFactory(),
                ZLinkHandlerActivator.reflection(),
                null,
                new ZLinkFrameworkRuntimeTestAccess.Construction()
                        .locationStores(ZLinkRegisteredLocationStores.fromUnified(repository)));
    }

    private static DefaultZLinkFrameworkOptions options(
            ZLinkLocationStore store, String routingId) {
        var options = new DefaultZLinkFrameworkOptions();
        options.addLocationStore(store);
        options.configureLocations().setOwnerLeaseTtl(Duration.ofHours(1));
        options.configureLocations().setOwnerLeaseRenewInterval(Duration.ofMinutes(10));
        options.configureLocations().setOwnerLeaseRenewTimeout(Duration.ofSeconds(2));
        options.configureLocations().setOwnerLeaseFencingMargin(MARGIN);
        options.configureLocations().setPollingInterval(Duration.ofHours(1));
        options.addRouteMesh("game")
                .listen("inproc://" + routingId + "-" + UUID.randomUUID())
                .setRoutingId(RoutingId.from(routingId));
        return options;
    }

    private static ZLinkLocationRepository countingRepository(
            ZLinkInMemoryLocationStore store, AtomicInteger successfulDescriptorMutations) {
        return (ZLinkLocationRepository)
                Proxy.newProxyInstance(
                        ZLinkLocationRepository.class.getClassLoader(),
                        new Class<?>[] {ZLinkLocationRepository.class},
                        (proxy, method, arguments) -> {
                            try {
                                Object result = method.invoke(store, arguments);
                                if (!method.getName().equals("updateMeshNode")
                                        || !(result instanceof CompletionStage<?> stage)) {
                                    return result;
                                }
                                return stage.thenApply(
                                        value -> {
                                            if (value instanceof ZLinkLocationWriteResult write
                                                    && write.status()
                                                            == ZLinkLocationWriteStatus.STORED) {
                                                successfulDescriptorMutations.incrementAndGet();
                                            }
                                            return value;
                                        });
                            } catch (InvocationTargetException failure) {
                                throw failure.getCause();
                            }
                        });
    }

    private static java.util.List<?> descriptors(ZLinkInMemoryLocationStore store)
            throws Exception {
        return store.listMeshNodes("game", ZLinkPageRequest.firstPage())
                .toCompletableFuture()
                .get(WAIT_SECONDS, TimeUnit.SECONDS)
                .items();
    }

    private static boolean renew(ZLinkFrameworkRuntime runtime) throws Exception {
        return ZLinkFrameworkRuntimeTestAccess.renewOwnerLeaseOnce(runtime)
                .toCompletableFuture()
                .get(WAIT_SECONDS, TimeUnit.SECONDS);
    }

    private static final class MutableClock extends Clock {
        private final AtomicReference<Instant> now;

        MutableClock(Instant now) {
            this.now = new AtomicReference<>(now);
        }

        void advance(Duration duration) {
            now.updateAndGet(value -> value.plus(duration));
        }

        @Override
        public ZoneId getZone() {
            return ZoneOffset.UTC;
        }

        @Override
        public Clock withZone(ZoneId zone) {
            return this;
        }

        @Override
        public Instant instant() {
            return now.get();
        }
    }
}
