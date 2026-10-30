package systems.zlink.framework.runtime.spots;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertInstanceOf;
import static org.junit.jupiter.api.Assertions.assertThrows;

import org.junit.jupiter.api.Test;

import systems.zlink.framework.actors.ZLinkActor;
import systems.zlink.framework.errors.ZLinkFrameworkErrorKind;
import systems.zlink.framework.errors.ZLinkFrameworkException;
import systems.zlink.framework.locations.ZLinkPageRequest;
import systems.zlink.framework.runtime.configuration.DefaultZLinkFrameworkOptions;
import systems.zlink.framework.runtime.host.ZLinkFrameworkRuntimeTestAccess;
import systems.zlink.framework.runtime.internal.locations.ZLinkLocationWriteIntent;
import systems.zlink.framework.runtime.internal.locations.ZLinkLocationWriteStatus;
import systems.zlink.framework.runtime.internal.locations.ZLinkMeshNodeDescriptor;
import systems.zlink.framework.runtime.internal.locations.ZLinkProviderLocationRepository;
import systems.zlink.framework.runtime.locations.ZLinkInMemoryLocationStore;
import systems.zlink.framework.spots.ZLinkSpot;
import systems.zlink.framework.spots.ZLinkSpotContext;
import systems.zlink.framework.spots.ZLinkSpotCreateState;

import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.ExecutionException;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicInteger;

final class ZLinkUserSpotExistingCapacityTest {
    private static final AtomicInteger FACTORY_CALLS = new AtomicInteger();

    @Test
    void existingReadySpotDoesNotRequireNewPlacementCapacity() throws Exception {
        FACTORY_CALLS.set(0);
        var options = new DefaultZLinkFrameworkOptions();
        var store = new ZLinkInMemoryLocationStore();
        options.addLocationStore(store);
        options.addRouteMesh("existing-capacity")
                .listen("tcp://127.0.0.1:0")
                .setSpotCapacity(1)
                .objects()
                .server()
                .addSpotFactory("room", Room.class, factory -> factory.disableRelocation());
        try (var runtime = ZLinkFrameworkRuntimeTestAccess.start(options)) {
            var manager = runtime.spotManager();
            var created =
                    manager.getOrCreate("room-one", "room")
                            .submit()
                            .toCompletableFuture()
                            .get(5, TimeUnit.SECONDS);
            assertEquals(ZLinkSpotCreateState.CREATED, created.state());
            var repository = new ZLinkProviderLocationRepository(store);
            var descriptor =
                    repository
                            .listMeshNodes("existing-capacity", new ZLinkPageRequest(10, null))
                            .toCompletableFuture()
                            .get(5, TimeUnit.SECONDS)
                            .items()
                            .getFirst();
            var capacity =
                    runtime.monitoringMeshNodeProjection("existing-capacity").objectCapacity();
            assertEquals(1, capacity.spots().active());
            assertEquals(1, capacity.spots().limit());
            var full =
                    new ZLinkMeshNodeDescriptor(
                            descriptor.meshName(),
                            descriptor.rid(),
                            descriptor.lifecycleGeneration(),
                            descriptor.descriptorRevision() + 1,
                            descriptor.endpoint(),
                            descriptor.channelWeights(),
                            descriptor.applicationVersion(),
                            descriptor.objectCapabilities(),
                            descriptor.objectRole(),
                            descriptor.entrySpotId(),
                            descriptor.placementWeight(),
                            capacity,
                            descriptor.activationConcurrency(),
                            descriptor.maintenanceWave(),
                            descriptor.state(),
                            descriptor.securityIdentity(),
                            descriptor.ownerId(),
                            descriptor.leaseGeneration(),
                            descriptor.updatedAt());
            assertEquals(
                    ZLinkLocationWriteStatus.STORED,
                    repository
                            .updateMeshNode(full, ZLinkLocationWriteIntent.RENEW)
                            .toCompletableFuture()
                            .get(5, TimeUnit.SECONDS)
                            .status());
            assertEquals(
                    1,
                    repository
                            .listMeshNodes("existing-capacity", new ZLinkPageRequest(10, null))
                            .toCompletableFuture()
                            .get(5, TimeUnit.SECONDS)
                            .items()
                            .getFirst()
                            .capacity()
                            .spots()
                            .active());
            var other = manager.getOrCreate("room-two", "room").submit().toCompletableFuture();
            var failure =
                    assertThrows(ExecutionException.class, () -> other.get(5, TimeUnit.SECONDS));
            assertEquals(
                    ZLinkFrameworkErrorKind.UNAVAILABLE,
                    assertInstanceOf(ZLinkFrameworkException.class, failure.getCause()).kind());
            var existing =
                    manager.getOrCreate("room-one", "room")
                            .submit()
                            .toCompletableFuture()
                            .get(5, TimeUnit.SECONDS);
            assertEquals(ZLinkSpotCreateState.EXISTING, existing.state());
            assertEquals(created.spot(), existing.spot());
            assertEquals(1, FACTORY_CALLS.get());
        }
    }

    public static final class Room implements ZLinkSpot<ZLinkActor> {
        private final ZLinkSpotContext context;

        public Room(ZLinkSpotContext context) {
            this.context = context;
            FACTORY_CALLS.incrementAndGet();
        }

        @Override
        public ZLinkSpotContext context() {
            return context;
        }

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
