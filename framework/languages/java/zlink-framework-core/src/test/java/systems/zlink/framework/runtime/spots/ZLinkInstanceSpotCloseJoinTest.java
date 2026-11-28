package systems.zlink.framework.runtime.spots;

import static org.junit.jupiter.api.Assertions.*;

import org.junit.jupiter.api.Test;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.configuration.ZLinkMessageFlowLogMode;
import systems.zlink.framework.locationprovider.*;
import systems.zlink.framework.runtime.InMemoryRelocationStore;
import systems.zlink.framework.runtime.configuration.DefaultZLinkFrameworkOptions;
import systems.zlink.framework.runtime.host.ZLinkFrameworkRuntimeTestAccess;
import systems.zlink.framework.runtime.internal.locations.ZLinkAuthorityMissing;
import systems.zlink.framework.runtime.internal.locations.ZLinkProviderLocationRepository;
import systems.zlink.framework.runtime.locations.ZLinkAuthorityKeyCodec;
import systems.zlink.framework.runtime.locations.ZLinkInMemoryProviderLocationStore;
import systems.zlink.framework.spots.*;

import java.time.Duration;
import java.util.UUID;
import java.util.concurrent.CancellationException;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicInteger;

final class ZLinkInstanceSpotCloseJoinTest {
    private static final String MESH = "instance-close-join-mesh";
    private static final String TYPE = "instance-close-join";
    private static final Duration WAIT = Duration.ofSeconds(10);

    @Test
    void contextCloseJoinPreservesCancellationAfterTheIncarnationStops() throws Exception {
        String spotId = "close-join-" + UUID.randomUUID();
        var store = new DeleteResultGate(spotId);
        var options = new DefaultZLinkFrameworkOptions();
        options.configureLocations().setOwnerLeaseFencingMargin(Duration.ofNanos(1));
        options.configureDispatch().messageFlow(ZLinkMessageFlowLogMode.NORMAL);
        options.addLocationStore(store);
        options.addRelocationStore(new InMemoryRelocationStore());
        options.addRouteMesh(MESH)
                .listen("tcp://127.0.0.1:0")
                .setRoutingId(RoutingId.from(spotId))
                .objects()
                .server()
                .addEntrySpot(ZLinkInstanceSpotCloseConformanceTest.Entry.class)
                .addInstanceSpotFactory(
                        TYPE, Instance.class, factory -> factory.disableRelocation());

        try (var runtime = ZLinkFrameworkRuntimeTestAccess.start(options)) {
            await(ZLinkFrameworkRuntimeTestAccess.startupCompletion(runtime));
            assertTrue(await(ZLinkFrameworkRuntimeTestAccess.renewOwnerLeaseOnce(runtime)));
            await(
                    runtime.route()
                            .requestToSpot(spotId, new Probe("ready"))
                            .instanceSpot(TYPE)
                            .inMesh(MESH)
                            .timeout(WAIT)
                            .submit(Probe.class));
            var spots = ZLinkFrameworkRuntimeTestAccess.spots(runtime);
            var activation = spots.instanceSpotActivations.get(spotId);
            assertNotNull(activation);
            var instance = (Instance) activation.spot();
            var owner = instance.context().close().toCompletableFuture();
            try {
                await(store.authorityDeleted);
                assertInstanceOf(
                        ZLinkAuthorityMissing.class,
                        await(
                                new ZLinkProviderLocationRepository(store)
                                        .read(ZLinkAuthorityKeyCodec.spot(spotId), () -> false)));
                assertEquals(1, instance.closingCalls.get());
                assertSame(activation, spots.instanceSpotActivations.get(spotId));

                var canceled = instance.context().close().toCompletableFuture();
                var joined = instance.context().close().toCompletableFuture();
                assertFalse(canceled.isDone());
                assertFalse(joined.isDone());

                var cancellationAtStop = new CompletableFuture<Boolean>();
                spots.setCountChanged(
                        () -> {
                            if (!spots.instanceSpotActivations.containsKey(spotId)) {
                                cancellationAtStop.complete(
                                        !owner.isDone() && canceled.cancel(false));
                            }
                        });

                store.releaseDeleteResult.complete(null);
                assertTrue(
                        await(cancellationAtStop),
                        "caller cancellation must win before Close completes");
                assertThrows(CancellationException.class, () -> await(canceled));
                assertTrue(await(owner));
                assertTrue(await(joined));
                assertEquals(1, instance.closingCalls.get());
                assertTrue(spots.closingCoordinators.isEmpty());
            } finally {
                store.releaseDeleteResult.complete(null);
            }
        }
    }

    private static <T> T await(CompletionStage<T> stage) throws Exception {
        return stage.toCompletableFuture().get(WAIT.toSeconds(), TimeUnit.SECONDS);
    }

    public record Probe(String value) {}

    public static final class Instance implements ZLinkInstanceSpot {
        private final ZLinkInstanceSpotContext context;
        final AtomicInteger closingCalls = new AtomicInteger();

        public Instance(ZLinkInstanceSpotContext context) {
            this.context = context;
        }

        @Override
        public ZLinkInstanceSpotContext context() {
            return context;
        }

        @Override
        public void configure() {
            context.handlers().addPacket(ProbeHandler.class);
        }

        @Override
        public CompletionStage<Void> onClosing(ZLinkSpotClosingContext closing) {
            closingCalls.incrementAndGet();
            return CompletableFuture.completedFuture(null);
        }
    }

    public static final class ProbeHandler
            implements ZLinkSpotRequestHandler<Instance, Probe, Probe> {
        @Override
        public CompletionStage<Probe> handle(Instance spot, Probe request) {
            return CompletableFuture.completedFuture(request);
        }
    }

    private static final class DeleteResultGate implements ZLinkLocationStore {
        private final ZLinkInMemoryProviderLocationStore inner =
                new ZLinkInMemoryProviderLocationStore();
        private final String spotId;
        final CompletableFuture<Void> authorityDeleted = new CompletableFuture<>();
        final CompletableFuture<Void> releaseDeleteResult = new CompletableFuture<>();

        DeleteResultGate(String spotId) {
            this.spotId = spotId;
        }

        @Override
        public CompletionStage<ZLinkStoreReadResult> read(
                ZLinkStoreKey key, ZLinkStoreCancellation cancellation) {
            return inner.read(key, cancellation);
        }

        @Override
        public CompletionStage<ZLinkStoreWriteResult> write(
                ZLinkStoreWriteRequest request, ZLinkStoreCancellation cancellation) {
            var write = inner.write(request, cancellation);
            if (request.mutations().stream()
                    .noneMatch(
                            mutation ->
                                    mutation instanceof ZLinkStoreDelete delete
                                            && delete.key().value().startsWith("authority\0")
                                            && delete.key().value().endsWith("\0" + spotId))) {
                return write;
            }
            return write.thenCompose(
                    result -> {
                        if (!(result instanceof ZLinkStoreWriteApplied)) {
                            return CompletableFuture.completedFuture(result);
                        }
                        authorityDeleted.complete(null);
                        return releaseDeleteResult.thenApply(ignored -> result);
                    });
        }

        @Override
        public CompletionStage<ZLinkStoreScanResult> scan(
                ZLinkStoreScanRequest request, ZLinkStoreCancellation cancellation) {
            return inner.scan(request, cancellation);
        }
    }
}
