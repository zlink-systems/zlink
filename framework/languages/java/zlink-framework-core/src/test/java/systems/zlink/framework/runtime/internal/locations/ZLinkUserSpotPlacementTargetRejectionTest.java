package systems.zlink.framework.runtime.internal.locations;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertInstanceOf;
import static org.junit.jupiter.api.Assertions.assertThrows;

import org.junit.jupiter.api.Test;

import systems.zlink.framework.actors.ZLinkActor;
import systems.zlink.framework.errors.ZLinkFrameworkErrorKind;
import systems.zlink.framework.errors.ZLinkFrameworkException;
import systems.zlink.framework.locationprovider.ZLinkLocationStore;
import systems.zlink.framework.locationprovider.ZLinkStoreCancellation;
import systems.zlink.framework.locationprovider.ZLinkStoreKey;
import systems.zlink.framework.locationprovider.ZLinkStoreReadFound;
import systems.zlink.framework.locationprovider.ZLinkStoreReadMissing;
import systems.zlink.framework.locationprovider.ZLinkStoreReadResult;
import systems.zlink.framework.locationprovider.ZLinkStoreScanRequest;
import systems.zlink.framework.locationprovider.ZLinkStoreScanResult;
import systems.zlink.framework.locationprovider.ZLinkStoreWriteRequest;
import systems.zlink.framework.locationprovider.ZLinkStoreWriteResult;
import systems.zlink.framework.locations.ZLinkPageRequest;
import systems.zlink.framework.runtime.configuration.DefaultZLinkFrameworkOptions;
import systems.zlink.framework.runtime.host.ZLinkFrameworkRuntimeTestAccess;
import systems.zlink.framework.runtime.locations.ZLinkInMemoryLocationStore;
import systems.zlink.framework.spots.ZLinkSpot;
import systems.zlink.framework.spots.ZLinkSpotContext;
import systems.zlink.framework.spots.ZLinkSpotCreateState;

import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.ExecutionException;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicReference;

final class ZLinkUserSpotPlacementTargetRejectionTest {
    @Test
    void rejectedTargetIsExcludedForTheRestOfTheOperation() throws Exception {
        String mesh = "target-rejection";
        var store = new MissingTargetRead();
        var options = new DefaultZLinkFrameworkOptions();
        options.addLocationStore(store);
        options.addRouteMesh(mesh)
                .listen("tcp://127.0.0.1:0")
                .objects()
                .server()
                .addSpotFactory("room", Room.class, factory -> factory.disableRelocation());

        try (var runtime = ZLinkFrameworkRuntimeTestAccess.start(options)) {
            var descriptor =
                    new ZLinkProviderLocationRepository(store)
                            .listMeshNodes(mesh, new ZLinkPageRequest(10, null))
                            .toCompletableFuture()
                            .get(5, TimeUnit.SECONDS)
                            .items()
                            .getFirst();
            store.missingTarget.set(
                    ZLinkProviderDescriptorRepository.meshKey(mesh, descriptor.rid()));
            var manager = runtime.spotManager();
            var rejected = manager.getOrCreate("room", "room").submit().toCompletableFuture();
            var failure =
                    assertInstanceOf(
                            ZLinkFrameworkException.class,
                            assertThrows(
                                            ExecutionException.class,
                                            () -> rejected.get(5, TimeUnit.SECONDS))
                                    .getCause());
            assertEquals(ZLinkFrameworkErrorKind.UNAVAILABLE, failure.kind());

            var created =
                    manager.getOrCreate("room", "room")
                            .submit()
                            .toCompletableFuture()
                            .get(5, TimeUnit.SECONDS);
            assertEquals(ZLinkSpotCreateState.CREATED, created.state());
        }
    }

    private static final class MissingTargetRead implements ZLinkLocationStore {
        private final ZLinkInMemoryLocationStore store = new ZLinkInMemoryLocationStore();
        private final AtomicReference<ZLinkStoreKey> missingTarget = new AtomicReference<>();

        @Override
        public CompletionStage<ZLinkStoreReadResult> read(
                ZLinkStoreKey key, ZLinkStoreCancellation cancellation) {
            return store.read(key, cancellation)
                    .thenApply(
                            read -> {
                                var missing = missingTarget.get();
                                if (read instanceof ZLinkStoreReadFound found
                                        && key.equals(missing)
                                        && missingTarget.compareAndSet(missing, null)) {
                                    return new ZLinkStoreReadMissing(found.value().storeNow());
                                }
                                return read;
                            });
        }

        @Override
        public CompletionStage<ZLinkStoreWriteResult> write(
                ZLinkStoreWriteRequest request, ZLinkStoreCancellation cancellation) {
            return store.write(request, cancellation);
        }

        @Override
        public CompletionStage<ZLinkStoreScanResult> scan(
                ZLinkStoreScanRequest request, ZLinkStoreCancellation cancellation) {
            return store.scan(request, cancellation);
        }
    }

    public record Room(ZLinkSpotContext context) implements ZLinkSpot<ZLinkActor> {
        @Override
        public CompletionStage<Void> onJoinedActor(ZLinkActor actor) {
            return CompletableFuture.completedFuture(null);
        }

        @Override
        public CompletionStage<Void> onLeaveActor(ZLinkActor actor) {
            return CompletableFuture.completedFuture(null);
        }
    }
}
