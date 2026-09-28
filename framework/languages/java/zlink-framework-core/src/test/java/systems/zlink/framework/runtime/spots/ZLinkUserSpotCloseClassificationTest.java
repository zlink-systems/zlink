package systems.zlink.framework.runtime.spots;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertInstanceOf;
import static org.junit.jupiter.api.Assertions.assertThrows;
import static org.junit.jupiter.api.Assertions.fail;

import org.junit.jupiter.api.Test;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.errors.ZLinkFrameworkErrorKind;
import systems.zlink.framework.errors.ZLinkFrameworkException;
import systems.zlink.framework.locationprovider.ZLinkLocationStore;
import systems.zlink.framework.locationprovider.ZLinkStoreCancellation;
import systems.zlink.framework.locationprovider.ZLinkStoreKey;
import systems.zlink.framework.locationprovider.ZLinkStoreReadResult;
import systems.zlink.framework.locationprovider.ZLinkStoreScanRequest;
import systems.zlink.framework.locationprovider.ZLinkStoreScanResult;
import systems.zlink.framework.locationprovider.ZLinkStoreWriteRequest;
import systems.zlink.framework.locationprovider.ZLinkStoreWriteResult;
import systems.zlink.framework.runtime.binding.ZLinkJavaBackendAdapterFactory;
import systems.zlink.framework.runtime.configuration.DefaultZLinkFrameworkOptions;
import systems.zlink.framework.runtime.host.ZLinkFrameworkRuntime;
import systems.zlink.framework.runtime.host.ZLinkFrameworkRuntimeTestAccess;
import systems.zlink.framework.runtime.internal.backend.ZLinkUserSpotOperationException;
import systems.zlink.framework.runtime.internal.locations.ZLinkAuthorityExpectFound;
import systems.zlink.framework.runtime.internal.locations.ZLinkAuthorityPut;
import systems.zlink.framework.runtime.internal.locations.ZLinkAuthoritySnapshot;
import systems.zlink.framework.runtime.internal.locations.ZLinkLocationRepository;
import systems.zlink.framework.runtime.internal.locations.ZLinkProviderLocationRepository;
import systems.zlink.framework.runtime.locations.ZLinkAuthorityKeyCodec;
import systems.zlink.framework.runtime.locations.ZLinkInMemoryLocationStore;
import systems.zlink.framework.runtime.locations.ZLinkServiceAuthorityPayloadCodec;
import systems.zlink.framework.spots.SpotRef;
import systems.zlink.framework.spots.ZLinkSpot;
import systems.zlink.framework.spots.ZLinkSpotContext;

import java.util.UUID;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.ExecutionException;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicBoolean;

/**
 * Spec 06-spot-address-messaging §7.1 and §9: the command 48 target alone classifies a rejected
 * Close. A different ObjectGeneration is SpotGenerationStale (33, InvalidOperation); a changed
 * owner fence or a User Spot that is not Ready is SpotMoving (34, Unavailable). The manager Close
 * on the owner node reaches the same target handler through the local command 48 path.
 */
final class ZLinkUserSpotCloseClassificationTest {
    private static final int SPOT_GENERATION_STALE = 33;
    private static final int SPOT_MOVING = 34;

    @Test
    void otherObjectGenerationIsGenerationStale() throws Exception {
        try (Harness harness = new Harness()) {
            SpotRef ref = harness.create();
            SpotRef other =
                    new SpotRef(
                            ref.spotId(),
                            ref.objectGeneration() + 1,
                            ref.meshName(),
                            ref.nodeRid());
            harness.assertClose(
                    other, ZLinkFrameworkErrorKind.INVALID_OPERATION, SPOT_GENERATION_STALE);
            assertEquals("READY", harness.state());
        }
    }

    @Test
    void userSpotThatIsNotReadyIsMoving() throws Exception {
        try (Harness harness = new Harness()) {
            SpotRef ref = harness.create();
            harness.rewrite(ZLinkServiceAuthorityPayloadCodec.State.CREATING);
            harness.assertClose(ref, ZLinkFrameworkErrorKind.UNAVAILABLE, SPOT_MOVING);
            assertEquals("CREATING", harness.state());
        }
    }

    @Test
    void authorityThatIsNoLongerAUserSpotIsMoving() throws Exception {
        try (Harness harness = new Harness()) {
            SpotRef ref = harness.create();
            harness.rewriteAsInstanceAuthority();
            harness.assertClose(ref, ZLinkFrameworkErrorKind.UNAVAILABLE, SPOT_MOVING);
        }
    }

    @Test
    void storeVersionChangedAfterTheSourceReadIsMoving() throws Exception {
        try (Harness harness = new Harness()) {
            SpotRef ref = harness.create();
            harness.store.rewriteAfterNextRead.set(true);
            harness.assertClose(ref, ZLinkFrameworkErrorKind.UNAVAILABLE, SPOT_MOVING);
            assertEquals("READY", harness.state());
        }
    }

    @Test
    void changedTargetNodeGenerationIsMoving() throws Exception {
        try (Harness harness = new Harness()) {
            SpotRef ref = harness.create();
            harness.rewriteNodeGeneration(1);
            ExecutionException failure =
                    assertThrows(
                            ExecutionException.class,
                            () ->
                                    harness.runtime
                                            .spotManager()
                                            .close(ref)
                                            .toCompletableFuture()
                                            .get(10, TimeUnit.SECONDS));
            ZLinkFrameworkException framework =
                    assertInstanceOf(ZLinkFrameworkException.class, failure.getCause());
            assertEquals(ZLinkFrameworkErrorKind.UNAVAILABLE, framework.kind());
            assertEquals(
                    true,
                    framework.getMessage().contains("failureCode=" + SPOT_MOVING),
                    framework.getMessage());
            assertEquals("READY", harness.state());
        }
    }

    private static final class Harness implements AutoCloseable {
        final String spotId = "classify-" + UUID.randomUUID();
        final RewritingStore store;
        final ZLinkLocationRepository repository;
        final ZLinkFrameworkRuntime runtime;

        Harness() {
            ZLinkInMemoryLocationStore inner = new ZLinkInMemoryLocationStore();
            store = new RewritingStore(inner, spotId);
            repository = new ZLinkProviderLocationRepository(store);
            DefaultZLinkFrameworkOptions options = new DefaultZLinkFrameworkOptions();
            options.addLocationStore(store);
            var node = options.addRouteMesh("classify-mesh");
            node.listen("inproc://spot-close-classify-" + UUID.randomUUID())
                    .setRoutingId(RoutingId.from("spot-close-classify-" + UUID.randomUUID()));
            node.objects()
                    .server()
                    .addSpotFactory(
                            "classify-spot",
                            ClassifySpot.class,
                            factory -> factory.disableRelocation());
            runtime =
                    ZLinkFrameworkRuntimeTestAccess.start(
                            options, new ZLinkJavaBackendAdapterFactory());
        }

        SpotRef create() throws Exception {
            return runtime.spotManager()
                    .getOrCreate(spotId, "classify-spot")
                    .submit()
                    .toCompletableFuture()
                    .get(10, TimeUnit.SECONDS)
                    .spot();
        }

        void rewrite(ZLinkServiceAuthorityPayloadCodec.State state) throws Exception {
            rewrite(state, 0);
        }

        void rewriteNodeGeneration(long delta) throws Exception {
            rewrite(ZLinkServiceAuthorityPayloadCodec.State.READY, delta);
        }

        void rewriteAsInstanceAuthority() throws Exception {
            rewrite(ZLinkServiceAuthorityPayloadCodec.State.READY, 0, true);
        }

        private void rewrite(ZLinkServiceAuthorityPayloadCodec.State state, long nodeDelta)
                throws Exception {
            rewrite(state, nodeDelta, false);
        }

        private void rewrite(
                ZLinkServiceAuthorityPayloadCodec.State state, long nodeDelta, boolean instance)
                throws Exception {
            String key = ZLinkAuthorityKeyCodec.spot(spotId);
            ZLinkAuthoritySnapshot snapshot =
                    assertInstanceOf(
                            ZLinkAuthoritySnapshot.class,
                            repository.read(key, () -> false).toCompletableFuture().get());
            var codec = new ZLinkServiceAuthorityPayloadCodec();
            var current = codec.decode(snapshot.payload()).orElseThrow();
            byte[] payload =
                    instance
                            ? codec.encodeInstance(
                                    state,
                                    current.stableType(),
                                    current.spotId(),
                                    current.ownerId(),
                                    current.ownerLeaseGeneration(),
                                    current.meshName(),
                                    current.nodeRid(),
                                    current.nodeGeneration() + nodeDelta)
                            : codec.encodeUser(
                                    state,
                                    current.stableType(),
                                    current.spotId(),
                                    current.ownerId(),
                                    current.ownerLeaseGeneration(),
                                    current.meshName(),
                                    current.nodeRid(),
                                    current.nodeGeneration() + nodeDelta);
            repository
                    .compareExchange(
                            key,
                            new ZLinkAuthorityExpectFound(snapshot.storeVersion()),
                            new ZLinkAuthorityPut(payload),
                            () -> false)
                    .toCompletableFuture()
                    .get();
        }

        String state() throws Exception {
            ZLinkAuthoritySnapshot snapshot =
                    assertInstanceOf(
                            ZLinkAuthoritySnapshot.class,
                            repository
                                    .read(ZLinkAuthorityKeyCodec.spot(spotId), () -> false)
                                    .toCompletableFuture()
                                    .get());
            return new ZLinkServiceAuthorityPayloadCodec()
                    .decode(snapshot.payload())
                    .orElseThrow()
                    .state()
                    .name();
        }

        void assertClose(SpotRef ref, ZLinkFrameworkErrorKind kind, int failureCode) {
            ExecutionException failure =
                    assertThrows(
                            ExecutionException.class,
                            () ->
                                    runtime.spotManager()
                                            .close(ref)
                                            .toCompletableFuture()
                                            .get(10, TimeUnit.SECONDS));
            ZLinkFrameworkException framework =
                    assertInstanceOf(ZLinkFrameworkException.class, failure.getCause());
            assertEquals(kind, framework.kind(), framework.getMessage());
            for (Throwable current = framework; current != null; current = current.getCause()) {
                if (current instanceof ZLinkUserSpotOperationException target) {
                    assertEquals(failureCode, target.failureCode(), target.getMessage());
                    return;
                }
            }
            fail("the target classification is missing from " + framework);
        }

        @Override
        public void close() {
            runtime.close();
        }
    }

    /** Rewrites the same Spot authority once right after the source reads it. */
    private static final class RewritingStore implements ZLinkLocationStore {
        private final ZLinkLocationStore inner;
        private final String spotId;
        final AtomicBoolean rewriteAfterNextRead = new AtomicBoolean();

        RewritingStore(ZLinkLocationStore inner, String spotId) {
            this.inner = inner;
            this.spotId = spotId;
        }

        @Override
        public CompletionStage<ZLinkStoreReadResult> read(
                ZLinkStoreKey key, ZLinkStoreCancellation cancellation) {
            boolean authority =
                    key.value().startsWith("authority\0") && key.value().endsWith("\0" + spotId);
            if (!authority || !rewriteAfterNextRead.compareAndSet(true, false)) {
                return inner.read(key, cancellation);
            }
            ZLinkLocationRepository repository = new ZLinkProviderLocationRepository(inner);
            String authorityKey = ZLinkAuthorityKeyCodec.spot(spotId);
            return inner.read(key, cancellation)
                    .thenCompose(
                            result ->
                                    repository
                                            .read(authorityKey, () -> false)
                                            .thenCompose(
                                                    read -> {
                                                        var snapshot =
                                                                (ZLinkAuthoritySnapshot) read;
                                                        return repository.compareExchange(
                                                                authorityKey,
                                                                new ZLinkAuthorityExpectFound(
                                                                        snapshot.storeVersion()),
                                                                new ZLinkAuthorityPut(
                                                                        snapshot.payload()),
                                                                () -> false);
                                                    })
                                            .thenApply(ignored -> result));
        }

        @Override
        public CompletionStage<ZLinkStoreWriteResult> write(
                ZLinkStoreWriteRequest request, ZLinkStoreCancellation cancellation) {
            return inner.write(request, cancellation);
        }

        @Override
        public CompletionStage<ZLinkStoreScanResult> scan(
                ZLinkStoreScanRequest request, ZLinkStoreCancellation cancellation) {
            return inner.scan(request, cancellation);
        }
    }

    public static final class ClassifySpot
            implements ZLinkSpot<systems.zlink.framework.actors.ZLinkActor> {
        private final ZLinkSpotContext context;

        public ClassifySpot(ZLinkSpotContext context) {
            this.context = context;
        }

        @Override
        public ZLinkSpotContext context() {
            return context;
        }

        @Override
        public CompletionStage<Void> onJoinedActor(
                systems.zlink.framework.actors.ZLinkActor actor) {
            return CompletableFuture.completedFuture(null);
        }

        @Override
        public CompletionStage<Void> onLeaveActor(systems.zlink.framework.actors.ZLinkActor actor) {
            return CompletableFuture.completedFuture(null);
        }
    }
}
