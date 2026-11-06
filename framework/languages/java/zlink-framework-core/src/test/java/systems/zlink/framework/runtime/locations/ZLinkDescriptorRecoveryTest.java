package systems.zlink.framework.runtime.locations;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertInstanceOf;
import static org.junit.jupiter.api.Assertions.assertThrows;
import static org.junit.jupiter.api.Assertions.assertTrue;

import static systems.zlink.framework.runtime.internal.locations.ZLinkDescriptorWriteIntent.FixedRidNewClaim.FIXED_RID_NEW_CLAIM;

import org.junit.jupiter.params.ParameterizedTest;
import org.junit.jupiter.params.provider.Arguments;
import org.junit.jupiter.params.provider.MethodSource;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.errors.ZLinkFrameworkErrorKind;
import systems.zlink.framework.errors.ZLinkFrameworkException;
import systems.zlink.framework.locationprovider.ZLinkLocationStore;
import systems.zlink.framework.locationprovider.ZLinkStoreCancellation;
import systems.zlink.framework.locationprovider.ZLinkStoreKey;
import systems.zlink.framework.locationprovider.ZLinkStoreMissingCondition;
import systems.zlink.framework.locationprovider.ZLinkStoreReadResult;
import systems.zlink.framework.locationprovider.ZLinkStoreScanRequest;
import systems.zlink.framework.locationprovider.ZLinkStoreScanResult;
import systems.zlink.framework.locationprovider.ZLinkStoreWriteRequest;
import systems.zlink.framework.locationprovider.ZLinkStoreWriteResult;
import systems.zlink.framework.locations.ZLinkPageRequest;
import systems.zlink.framework.runtime.binding.ZLinkJavaBackendAdapterFactory;
import systems.zlink.framework.runtime.configuration.DefaultZLinkFrameworkOptions;
import systems.zlink.framework.runtime.host.ZLinkFrameworkRuntime;
import systems.zlink.framework.runtime.host.ZLinkFrameworkRuntimeState;
import systems.zlink.framework.runtime.host.ZLinkFrameworkRuntimeTestAccess;
import systems.zlink.framework.runtime.internal.handlers.ZLinkHandlerActivator;
import systems.zlink.framework.runtime.internal.locations.ZLinkDescriptorWriteIntent;
import systems.zlink.framework.runtime.internal.locations.ZLinkLocationOwnerToken;
import systems.zlink.framework.runtime.internal.locations.ZLinkLocationRepository;
import systems.zlink.framework.runtime.internal.locations.ZLinkLocationWriteIntent;
import systems.zlink.framework.runtime.internal.locations.ZLinkLocationWriteStatus;
import systems.zlink.framework.runtime.internal.locations.ZLinkMeshNodeDescriptor;
import systems.zlink.framework.runtime.internal.locations.ZLinkMeshNodeDescriptorKey;
import systems.zlink.framework.runtime.internal.locations.ZLinkOwnerLeaseClaimed;
import systems.zlink.framework.runtime.internal.locations.ZLinkOwnerLeaseFound;
import systems.zlink.framework.runtime.internal.locations.ZLinkProviderLocationRepository;

import java.lang.reflect.InvocationTargetException;
import java.lang.reflect.Proxy;
import java.time.Clock;
import java.time.Duration;
import java.time.Instant;
import java.time.ZoneId;
import java.time.ZoneOffset;
import java.util.List;
import java.util.UUID;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.CopyOnWriteArrayList;
import java.util.concurrent.ExecutionException;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.concurrent.atomic.AtomicLong;
import java.util.concurrent.atomic.AtomicReference;
import java.util.stream.IntStream;
import java.util.stream.Stream;

final class ZLinkDescriptorRecoveryTest {
    private static final Duration MARGIN = Duration.ofNanos(10);
    private static final long RELEASE_WAIT = MARGIN.multipliedBy(3).toNanos();

    @ParameterizedTest(name = "{displayName}, provider={0}, repetition={1}")
    @MethodSource("backends")
    void recoveryCannotReplaceAnotherExpiredOwnerBeforeFixedRidReleaseWait(
            boolean provider, int repetition) throws Exception {
        var backend = new Backend(provider);
        try (var first = backend.start(backend.publisherRepository)) {
            backend.serve(first);
            var original = backend.descriptor();
            backend.clock.advance(Duration.ofHours(1));
            try (var second = backend.start(backend.repository)) {
                backend.serve(second);
                var competing = backend.descriptor();
                assertFalse(original.ownerId().equals(competing.ownerId()));
                backend.clock.advance(Duration.ofHours(1));
                backend.intents.clear();

                assertFalse(await(ZLinkFrameworkRuntimeTestAccess.renewOwnerLeaseOnce(first)));
                assertEquals(competing, backend.descriptor());
                assertEquals(List.of(ZLinkLocationWriteIntent.RENEW), backend.intents);
                var newOwner =
                        assertInstanceOf(
                                        ZLinkOwnerLeaseFound.class,
                                        await(
                                                backend.repository.readOwnerLease(
                                                        original.ownerId())))
                                .token();
                assertTrue(newOwner.leaseGeneration() > original.leaseGeneration());
                var replacement = withOwner(original, newOwner);
                long observedAt = backend.monotonicNanos.get();

                assertReleaseWait(backend.repository, replacement);
                assertEquals(competing, backend.descriptor());
                backend.monotonicNanos.set(observedAt + RELEASE_WAIT - 1);
                assertReleaseWait(backend.repository, replacement);
                assertEquals(competing, backend.descriptor());
                backend.monotonicNanos.incrementAndGet();
                assertEquals(
                        ZLinkLocationWriteStatus.STORED,
                        await(backend.repository.updateMeshNode(replacement, FIXED_RID_NEW_CLAIM))
                                .status());
                assertEquals(newOwner.ownerId(), backend.descriptor().ownerId());
                // Install a live token for shutdown after the expired-owner assertions.
                assertFalse(await(ZLinkFrameworkRuntimeTestAccess.renewOwnerLeaseOnce(second)));
            }
        }
    }

    @ParameterizedTest(name = "{displayName}, provider={0}, repetition={1}")
    @MethodSource("backends")
    void recoveryCannotReplaceAnotherLiveOwner(boolean provider, int repetition) throws Exception {
        var backend = new Backend(provider);
        try (var first = backend.start(backend.publisherRepository)) {
            backend.serve(first);
            backend.clock.advance(Duration.ofHours(1));
            try (var second = backend.start(backend.repository)) {
                backend.serve(second);
                var competing = backend.descriptor();
                backend.intents.clear();

                assertFalse(await(ZLinkFrameworkRuntimeTestAccess.renewOwnerLeaseOnce(first)));
                assertEquals(competing, backend.descriptor());
                assertEquals(List.of(ZLinkLocationWriteIntent.RENEW), backend.intents);
            }
        }
    }

    @ParameterizedTest(name = "{displayName}, provider={0}, repetition={1}")
    @MethodSource("backends")
    void missingRecoveryCannotOverwriteAnOwnerCreatedBeforeItsConditionalWrite(
            boolean provider, int repetition) throws Exception {
        var backend = new Backend(provider);
        try (var first = backend.start(backend.publisherRepository)) {
            backend.serve(first);
            var original = backend.descriptor();
            assertEquals(
                    ZLinkLocationWriteStatus.STORED,
                    await(
                            backend.repository.removeMeshNode(
                                    new ZLinkMeshNodeDescriptorKey(
                                            original.meshName(), original.rid()),
                                    new ZLinkLocationOwnerToken(
                                            original.ownerId(), original.leaseGeneration()))));
            assertTrue(
                    await(backend.repository.listMeshNodes("game", ZLinkPageRequest.firstPage()))
                            .items()
                            .isEmpty());
            backend.clock.advance(Duration.ofHours(1));
            backend.intents.clear();
            backend.pause.set(true);
            var recovery = ZLinkFrameworkRuntimeTestAccess.renewOwnerLeaseOnce(first);
            try {
                await(backend.paused);
                try (var second = backend.start(backend.repository)) {
                    backend.serve(second);
                    var competing = backend.descriptor();
                    var recoveringOwner =
                            assertInstanceOf(
                                    ZLinkOwnerLeaseFound.class,
                                    await(backend.repository.readOwnerLease(original.ownerId())));
                    assertTrue(
                            recoveringOwner.leaseExpiresAt().isAfter(recoveringOwner.storeNow()));
                    assertTrue(
                            recoveringOwner.token().leaseGeneration() > original.leaseGeneration());
                    backend.resume.complete(null);

                    assertFalse(await(recovery));
                    assertEquals(competing, backend.descriptor());
                    assertEquals(List.of(ZLinkLocationWriteIntent.RENEW), backend.intents);
                }
            } finally {
                backend.resume.complete(null);
            }
        }
    }

    @ParameterizedTest(name = "{displayName}, provider={0}, repetition={1}")
    @MethodSource("backends")
    void ordinaryNewClaimReplacesAnExpiredOwnerWithoutFixedRidWait(boolean provider, int repetition)
            throws Exception {
        var backend = new Backend(provider);
        try (var first = backend.start(backend.publisherRepository)) {
            backend.serve(first);
            var original = backend.descriptor();
            backend.clock.advance(Duration.ofHours(1));
            var owner =
                    assertInstanceOf(
                                    ZLinkOwnerLeaseClaimed.class,
                                    await(
                                            backend.repository.claimOwnerLease(
                                                    "new-owner", Duration.ofHours(1))))
                            .token();
            assertEquals(
                    ZLinkLocationWriteStatus.STORED,
                    await(
                                    backend.repository.updateMeshNode(
                                            withOwner(original, owner),
                                            ZLinkLocationWriteIntent.NEW_CLAIM))
                            .status());
            assertEquals(owner.ownerId(), backend.descriptor().ownerId());
            // Claim a live cleanup token after the original lease has expired.
            assertFalse(await(ZLinkFrameworkRuntimeTestAccess.renewOwnerLeaseOnce(first)));
        }
    }

    private static Stream<Arguments> backends() {
        return IntStream.rangeClosed(1, 100)
                .boxed()
                .flatMap(
                        repetition ->
                                Stream.of(
                                        Arguments.of(false, repetition),
                                        Arguments.of(true, repetition)));
    }

    private static void assertReleaseWait(
            ZLinkLocationRepository repository, ZLinkMeshNodeDescriptor descriptor) {
        var failure =
                assertThrows(
                        Exception.class,
                        () -> await(repository.updateMeshNode(descriptor, FIXED_RID_NEW_CLAIM)));
        assertEquals(
                ZLinkFrameworkErrorKind.UNAVAILABLE,
                assertInstanceOf(
                                ZLinkFrameworkException.class,
                                failure instanceof ExecutionException
                                        ? failure.getCause()
                                        : failure)
                        .kind());
    }

    private static ZLinkMeshNodeDescriptor withOwner(
            ZLinkMeshNodeDescriptor original, ZLinkLocationOwnerToken owner) {
        return new ZLinkMeshNodeDescriptor(
                original.meshName(),
                original.rid(),
                original.lifecycleGeneration(),
                original.descriptorRevision() + 1,
                original.endpoint(),
                original.channelWeights(),
                original.applicationVersion(),
                original.objectCapabilities(),
                original.objectRole(),
                original.entrySpotId(),
                original.placementWeight(),
                original.capacity(),
                original.activationConcurrency(),
                original.maintenanceWave(),
                original.state(),
                original.securityIdentity(),
                owner.ownerId(),
                owner.leaseGeneration(),
                original.updatedAt());
    }

    private static <T> T await(CompletionStage<T> stage) throws Exception {
        return stage.toCompletableFuture().get(30, TimeUnit.SECONDS);
    }

    private static final class Backend {
        final MutableClock clock = new MutableClock();
        final AtomicLong monotonicNanos = new AtomicLong();
        final AtomicBoolean pause = new AtomicBoolean();
        final CompletableFuture<Void> paused = new CompletableFuture<>();
        final CompletableFuture<Void> resume = new CompletableFuture<>();
        final List<ZLinkDescriptorWriteIntent> intents = new CopyOnWriteArrayList<>();
        final ZLinkLocationStore store;
        final ZLinkLocationRepository repository;
        final ZLinkLocationRepository publisherRepository;

        Backend(boolean provider) {
            if (provider) {
                var inner = new ZLinkInMemoryProviderLocationStore(clock);
                store =
                        new ZLinkLocationStore() {
                            @Override
                            public CompletionStage<ZLinkStoreReadResult> read(
                                    ZLinkStoreKey key, ZLinkStoreCancellation cancellation) {
                                return inner.read(key, cancellation);
                            }

                            @Override
                            public CompletionStage<ZLinkStoreWriteResult> write(
                                    ZLinkStoreWriteRequest request,
                                    ZLinkStoreCancellation cancellation) {
                                if (request.conditions().stream()
                                                .anyMatch(
                                                        condition ->
                                                                condition
                                                                                instanceof
                                                                                ZLinkStoreMissingCondition
                                                                                        missing
                                                                        && missing.key()
                                                                                .value()
                                                                                .startsWith(
                                                                                        "mesh-node\0"))
                                        && pause.compareAndSet(true, false)) {
                                    paused.complete(null);
                                    return resume.thenCompose(
                                            ignored -> inner.write(request, cancellation));
                                }
                                return inner.write(request, cancellation);
                            }

                            @Override
                            public CompletionStage<ZLinkStoreScanResult> scan(
                                    ZLinkStoreScanRequest request,
                                    ZLinkStoreCancellation cancellation) {
                                return inner.scan(request, cancellation);
                            }
                        };
                repository =
                        new ZLinkProviderLocationRepository(store, monotonicNanos::get, MARGIN);
            } else {
                var memory = new ZLinkInMemoryLocationStore(clock, monotonicNanos::get, MARGIN);
                store = memory;
                repository = memory;
            }
            publisherRepository =
                    (ZLinkLocationRepository)
                            Proxy.newProxyInstance(
                                    ZLinkLocationRepository.class.getClassLoader(),
                                    new Class<?>[] {ZLinkLocationRepository.class},
                                    (proxy, method, arguments) -> {
                                        if (method.getName().equals("updateMeshNode")) {
                                            intents.add((ZLinkDescriptorWriteIntent) arguments[1]);
                                            // The in-memory read and mutation share one lane turn.
                                            // Pause before
                                            // that turn; the provider can pause at the actual
                                            // Missing CAS.
                                            if (!provider && pause.compareAndSet(true, false)) {
                                                paused.complete(null);
                                                return resume.thenCompose(
                                                        ignored ->
                                                                repository.updateMeshNode(
                                                                        (ZLinkMeshNodeDescriptor)
                                                                                arguments[0],
                                                                        (ZLinkDescriptorWriteIntent)
                                                                                arguments[1]));
                                            }
                                        }
                                        try {
                                            return method.invoke(repository, arguments);
                                        } catch (InvocationTargetException failure) {
                                            throw failure.getCause();
                                        }
                                    });
        }

        ZLinkFrameworkRuntime start(ZLinkLocationRepository target) throws Exception {
            var options = new DefaultZLinkFrameworkOptions();
            options.addLocationStore(store);
            options.configureLocations().setOwnerLeaseTtl(Duration.ofHours(1));
            options.configureLocations().setOwnerLeaseRenewInterval(Duration.ofMinutes(10));
            options.configureLocations().setOwnerLeaseRenewTimeout(Duration.ofSeconds(2));
            options.configureLocations().setOwnerLeaseFencingMargin(MARGIN);
            options.configureLocations().setPollingInterval(Duration.ofHours(1));
            options.addRouteMesh("game")
                    .listen("inproc://recovery-" + UUID.randomUUID())
                    .setRoutingId(RoutingId.from("recovery"));
            var runtime =
                    ZLinkFrameworkRuntimeTestAccess.startWithConstruction(
                            options,
                            new ZLinkJavaBackendAdapterFactory(),
                            ZLinkHandlerActivator.reflection(),
                            null,
                            new ZLinkFrameworkRuntimeTestAccess.Construction()
                                    .locationStores(
                                            ZLinkRegisteredLocationStores.fromUnified(target)));
            await(ZLinkFrameworkRuntimeTestAccess.startupCompletion(runtime));
            assertEquals(
                    ZLinkFrameworkRuntimeState.PREPARING,
                    ZLinkFrameworkRuntimeTestAccess.state(runtime));
            return runtime;
        }

        void serve(ZLinkFrameworkRuntime runtime) throws Exception {
            monotonicNanos.addAndGet(RELEASE_WAIT);
            assertTrue(await(ZLinkFrameworkRuntimeTestAccess.renewOwnerLeaseOnce(runtime)));
            assertEquals(
                    ZLinkFrameworkRuntimeState.SERVING,
                    ZLinkFrameworkRuntimeTestAccess.state(runtime));
        }

        ZLinkMeshNodeDescriptor descriptor() throws Exception {
            var rows =
                    await(repository.listMeshNodes("game", ZLinkPageRequest.firstPage())).items();
            assertEquals(1, rows.size());
            return rows.getFirst();
        }
    }

    private static final class MutableClock extends Clock {
        private final AtomicReference<Instant> now =
                new AtomicReference<>(Instant.parse("2026-11-06T00:00:00Z"));

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
