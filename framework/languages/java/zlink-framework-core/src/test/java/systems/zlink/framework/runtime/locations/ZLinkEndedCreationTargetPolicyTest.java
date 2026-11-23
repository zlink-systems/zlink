package systems.zlink.framework.runtime.locations;

import static org.junit.jupiter.api.Assertions.*;

import org.junit.jupiter.api.Test;
import org.junit.jupiter.params.ParameterizedTest;
import org.junit.jupiter.params.provider.CsvSource;

import systems.zlink.framework.actors.*;
import systems.zlink.framework.errors.ZLinkFrameworkErrorKind;
import systems.zlink.framework.errors.ZLinkFrameworkException;
import systems.zlink.framework.locationprovider.*;
import systems.zlink.framework.locationprovider.ZLinkStoreCancellation;
import systems.zlink.framework.locations.*;
import systems.zlink.framework.monitoring.ZLinkListenerKind;
import systems.zlink.framework.runtime.binding.ZLinkJavaBackendAdapterFactory;
import systems.zlink.framework.runtime.configuration.DefaultZLinkFrameworkOptions;
import systems.zlink.framework.runtime.host.ZLinkFrameworkRuntime;
import systems.zlink.framework.runtime.host.ZLinkFrameworkRuntimeTestAccess;
import systems.zlink.framework.runtime.internal.configuration.ZLinkObjectFactoryRegistration.RelocationPolicy;
import systems.zlink.framework.runtime.internal.handlers.ZLinkHandlerActivator;
import systems.zlink.framework.runtime.internal.locations.*;
import systems.zlink.framework.spots.*;

import java.nio.charset.StandardCharsets;
import java.time.Duration;
import java.util.List;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionException;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.concurrent.atomic.AtomicReference;

final class ZLinkEndedCreationTargetPolicyTest {
    private static final String ID = "ended";
    private static final String TYPE = "room";
    private static final String MESH = "mesh";
    private static final AtomicInteger ACTOR_FACTORIES = new AtomicInteger();
    private static final AtomicInteger SPOT_FACTORIES = new AtomicInteger();

    @ParameterizedTest
    @CsvSource({"memory, ACTOR", "provider, ACTOR", "memory, USER_SPOT", "provider, USER_SPOT"})
    void unregisteredRequesterCreatesOnRemoteDisabledTarget(
            String backend, ZLinkPlacementObjectKind kind) throws Exception {
        ZLinkLocationStore store =
                backend.equals("memory")
                        ? new ZLinkInMemoryLocationStore()
                        : new ZLinkInMemoryProviderLocationStore();
        ZLinkLocationRepository repository =
                store instanceof ZLinkLocationRepository inMemory
                        ? inMemory
                        : new ZLinkProviderLocationRepository(store);
        var before = endedAuthority(repository, kind);
        ACTOR_FACTORIES.set(0);
        SPOT_FACTORIES.set(0);
        var targetOptions = new DefaultZLinkFrameworkOptions();
        targetOptions.addLocationStore(store);
        targetOptions
                .addRouteMesh(MESH)
                .listen("tcp://127.0.0.1:0")
                .objects()
                .server()
                .addEntrySpot(Entry.class)
                .addActorFactory(TYPE, Player.class, Factory.class, f -> f.disableRelocation())
                .addSpotFactory(TYPE, Room.class, f -> f.disableRelocation());
        try (var target = start(targetOptions, repository)) {
            var sourceOptions = new DefaultZLinkFrameworkOptions();
            sourceOptions.addLocationStore(store);
            var node = sourceOptions.addRouteMesh(MESH).listen("tcp://127.0.0.1:0");
            node.objects().client();
            node.peerConnections()
                    .connect(target.listenerStatus(ZLinkListenerKind.ROUTE_MESH, MESH).endpoint());
            try (var requester = start(sourceOptions, repository)) {
                if (kind == ZLinkPlacementObjectKind.ACTOR) {
                    var created =
                            assertInstanceOf(
                                    ZLinkActorCreateResult.Created.class,
                                    requester
                                            .actorManager()
                                            .getOrCreate(ID, TYPE)
                                            .submit()
                                            .toCompletableFuture()
                                            .get(10, TimeUnit.SECONDS));
                    assertTrue(created.actor().objectGeneration() > before.objectGeneration());
                    assertEquals(1, ACTOR_FACTORIES.get());
                } else {
                    var created =
                            requester
                                    .spotManager()
                                    .getOrCreate(ID, TYPE)
                                    .submit()
                                    .toCompletableFuture()
                                    .get(10, TimeUnit.SECONDS);
                    assertEquals(ZLinkSpotCreateState.CREATED, created.state());
                    assertTrue(created.spot().objectGeneration() > before.objectGeneration());
                    assertEquals(1, SPOT_FACTORIES.get());
                }
            }
        }
    }

    private static ZLinkFrameworkRuntime start(
            DefaultZLinkFrameworkOptions options, ZLinkLocationRepository repository) {
        return ZLinkFrameworkRuntimeTestAccess.startHostWithConstruction(
                options,
                new ZLinkJavaBackendAdapterFactory(),
                ZLinkHandlerActivator.reflection(),
                null,
                new ZLinkFrameworkRuntimeTestAccess.Construction()
                        .locationStores(ZLinkRegisteredLocationStores.fromUnified(repository)));
    }

    public record Player(ZLinkActorContext context) implements ZLinkActor {}

    public static final class Factory implements ZLinkActorFactory {
        public CompletionStage<ZLinkActor> create(ZLinkActorContext context) {
            ACTOR_FACTORIES.incrementAndGet();
            return CompletableFuture.completedFuture(new Player(context));
        }
    }

    public record Entry(ZLinkEntrySpotContext context) implements ZLinkEntrySpot<Player> {
        public CompletionStage<Void> onJoinedActor(Player actor) {
            return CompletableFuture.completedFuture(null);
        }

        public CompletionStage<Void> onLeaveActor(Player actor) {
            return CompletableFuture.completedFuture(null);
        }
    }

    public record Room(ZLinkSpotContext context) implements ZLinkSpot<ZLinkActor> {
        public Room {
            SPOT_FACTORIES.incrementAndGet();
        }

        public CompletionStage<Void> onJoinedActor(ZLinkActor actor) {
            return CompletableFuture.completedFuture(null);
        }

        public CompletionStage<Void> onLeaveActor(ZLinkActor actor) {
            return CompletableFuture.completedFuture(null);
        }
    }

    @ParameterizedTest(name = "{0} {1} target={2}")
    @CsvSource({
        "memory, ACTOR, DISABLED", "provider, ACTOR, DISABLED",
        "memory, USER_SPOT, DISABLED", "provider, USER_SPOT, DISABLED",
        "memory, ACTOR, RECREATE", "provider, ACTOR, RECREATE",
        "memory, USER_SPOT, RECREATE", "provider, USER_SPOT, RECREATE",
        "memory, ACTOR, ", "provider, ACTOR, ",
        "memory, USER_SPOT, ", "provider, USER_SPOT, "
    })
    void reclaimUsesSelectedTargetCapability(
            String backend,
            ZLinkPlacementObjectKind kind,
            ZLinkObjectMaintenancePolicyKind policy) {
        ZLinkLocationRepository store =
                backend.equals("provider")
                        ? new ZLinkProviderLocationRepository(
                                new ZLinkInMemoryProviderLocationStore())
                        : new ZLinkInMemoryLocationStore();
        var before = endedAuthority(store, kind);
        var targetOwner = claim(store, "target");
        var target = descriptor(store, targetOwner, kind, policy);
        String key = key(kind);

        // Missing requester registration in the positive case and conflicting requester
        // registration in the negative cases.
        var next =
                request(
                        target,
                        targetOwner,
                        key,
                        kind,
                        TYPE,
                        policy == ZLinkObjectMaintenancePolicyKind.DISABLED
                                ? null
                                : new RelocationPolicy.Disabled());
        if (policy == ZLinkObjectMaintenancePolicyKind.DISABLED) {
            var reserved =
                    assertInstanceOf(
                                    ZLinkObjectReserved.class,
                                    store.reserve(next, () -> false).toCompletableFuture().join())
                            .reservation();
            assertTrue(reserved.objectGeneration() > before.objectGeneration());
            assertEquals(targetOwner, reserved.targetOwner());
        } else {
            var failure =
                    assertThrows(
                            RuntimeException.class,
                            () -> store.reserve(next, () -> false).toCompletableFuture().join());
            assertEquals(
                    ZLinkFrameworkErrorKind.UNAVAILABLE,
                    assertInstanceOf(
                                    ZLinkFrameworkException.class,
                                    failure instanceof CompletionException
                                            ? failure.getCause()
                                            : failure)
                            .kind());
            var after =
                    assertInstanceOf(
                            ZLinkAuthoritySnapshot.class,
                            store.read(key, () -> false).toCompletableFuture().join());
            assertEquals(before.storeVersion(), after.storeVersion());
            assertArrayEquals(before.payload(), after.payload());
        }
    }

    @Test
    void providerRejectsDescriptorVersionChangeBeforeReleaseCommit() {
        var provider = new ZLinkInMemoryProviderLocationStore();
        var changes = new AtomicInteger();
        var authorityKey = new ZLinkStoreKey("authority\0actor\0" + ID);
        var targetDescriptor = new AtomicReference<ZLinkMeshNodeDescriptor>();
        var targetKey = new AtomicReference<ZLinkStoreKey>();
        var intercept =
                new ZLinkLocationStore() {
                    public CompletionStage<ZLinkStoreReadResult> read(
                            ZLinkStoreKey key, ZLinkStoreCancellation cancel) {
                        return provider.read(key, cancel);
                    }

                    public CompletionStage<ZLinkStoreScanResult> scan(
                            ZLinkStoreScanRequest request, ZLinkStoreCancellation cancel) {
                        return provider.scan(request, cancel);
                    }

                    public CompletionStage<ZLinkStoreWriteResult> write(
                            ZLinkStoreWriteRequest request, ZLinkStoreCancellation cancel) {
                        if (!request.mutations().contains(new ZLinkStoreDelete(authorityKey))
                                || !changes.compareAndSet(0, 1))
                            return provider.write(request, cancel);
                        var target = targetDescriptor.get();
                        var descriptorKey = targetKey.get();
                        return provider.read(descriptorKey, cancel)
                                .thenCompose(
                                        read -> {
                                            var previous =
                                                    assertInstanceOf(
                                                                    ZLinkStoreReadFound.class, read)
                                                            .value();
                                            var original =
                                                    new String(
                                                            previous.bytes(),
                                                            StandardCharsets.UTF_8);
                                            var revision =
                                                    Long.toUnsignedString(
                                                            target.descriptorRevision());
                                            var changed =
                                                    original.replace(
                                                                    "\"policy\":\"disabled\"",
                                                                    "\"policy\":\"recreate\"")
                                                            .replace(
                                                                    "\"descriptorRevision\":\""
                                                                            + revision
                                                                            + "\"",
                                                                    "\"descriptorRevision\":\""
                                                                            + (target
                                                                                            .descriptorRevision()
                                                                                    + 1)
                                                                            + "\"");
                                            assertNotEquals(original, changed);
                                            return provider.write(
                                                    new ZLinkStoreWriteRequest(
                                                            List.of(
                                                                    new ZLinkStoreVersionCondition(
                                                                            descriptorKey,
                                                                            previous.version())),
                                                            List.of(
                                                                    new ZLinkStorePut(
                                                                            descriptorKey,
                                                                            changed.getBytes(
                                                                                    StandardCharsets
                                                                                            .UTF_8),
                                                                            null))),
                                                    cancel);
                                        })
                                .thenCompose(
                                        changed -> {
                                            assertInstanceOf(ZLinkStoreWriteApplied.class, changed);
                                            return provider.write(request, cancel);
                                        })
                                .thenApply(
                                        release ->
                                                assertInstanceOf(
                                                        ZLinkStoreWriteConflict.class, release));
                    }
                };
        var store = new ZLinkProviderLocationRepository(intercept);
        var kind = ZLinkPlacementObjectKind.ACTOR;
        var before = endedAuthority(store, kind);
        var targetOwner = claim(store, "target");
        var target =
                descriptor(store, targetOwner, kind, ZLinkObjectMaintenancePolicyKind.DISABLED);
        targetDescriptor.set(target);
        targetKey.set(
                new ZLinkStoreKey("mesh-node\0" + target.meshName() + "\0" + target.rid().toHex()));
        var next =
                request(
                        target,
                        targetOwner,
                        key(kind),
                        kind,
                        TYPE,
                        new RelocationPolicy.Disabled());
        var failure =
                assertThrows(
                        CompletionException.class,
                        () -> store.reserve(next, () -> false).toCompletableFuture().join());
        assertEquals(1, changes.get());
        assertEquals(
                ZLinkFrameworkErrorKind.UNAVAILABLE,
                assertInstanceOf(ZLinkFrameworkException.class, failure.getCause()).kind());
        assertEquals(
                before.storeVersion(),
                assertInstanceOf(
                                ZLinkAuthoritySnapshot.class,
                                store.read(key(kind), () -> false).toCompletableFuture().join())
                        .storeVersion());
    }

    @Test
    void providerReleasesEndedAuthorityAfterDisabledTargetWeightChanges() throws Exception {
        var provider = new ZLinkInMemoryProviderLocationStore();
        var changes = new AtomicInteger();
        var authorityKey = new ZLinkStoreKey("authority\0actor\0" + ID);
        var targetKey = new AtomicReference<ZLinkMeshNodeDescriptorKey>();
        var repositoryRef = new AtomicReference<ZLinkProviderLocationRepository>();
        var intercept =
                new ZLinkLocationStore() {
                    public CompletionStage<ZLinkStoreReadResult> read(
                            ZLinkStoreKey key, ZLinkStoreCancellation cancel) {
                        return provider.read(key, cancel);
                    }

                    public CompletionStage<ZLinkStoreScanResult> scan(
                            ZLinkStoreScanRequest request, ZLinkStoreCancellation cancel) {
                        return provider.scan(request, cancel);
                    }

                    public CompletionStage<ZLinkStoreWriteResult> write(
                            ZLinkStoreWriteRequest request, ZLinkStoreCancellation cancel) {
                        if (!request.mutations().contains(new ZLinkStoreDelete(authorityKey))
                                || !changes.compareAndSet(0, 1))
                            return provider.write(request, cancel);
                        var repository = repositoryRef.get();
                        var descriptorKey = targetKey.get();
                        return repository
                                .listMeshNodes(MESH, ZLinkPageRequest.firstPage())
                                .thenCompose(
                                        page -> {
                                            var target =
                                                    page.items().stream()
                                                            .filter(
                                                                    descriptor ->
                                                                            descriptorKey.equals(
                                                                                    new ZLinkMeshNodeDescriptorKey(
                                                                                            descriptor
                                                                                                    .meshName(),
                                                                                            descriptor
                                                                                                    .rid())))
                                                            .findFirst()
                                                            .orElseThrow();
                                            return repository.updateMeshNode(
                                                    updateDescriptor(
                                                            target,
                                                            ZLinkPlacementObjectKind.ACTOR,
                                                            null,
                                                            target.placementWeight() + 1),
                                                    ZLinkLocationWriteIntent.RENEW);
                                        })
                                .thenCompose(
                                        changed -> {
                                            assertEquals(
                                                    ZLinkLocationWriteStatus.STORED,
                                                    changed.status());
                                            return provider.write(request, cancel);
                                        })
                                .thenApply(
                                        release ->
                                                assertInstanceOf(
                                                        ZLinkStoreWriteConflict.class, release));
                    }
                };
        var store = new ZLinkProviderLocationRepository(intercept);
        repositoryRef.set(store);
        var before = endedAuthority(store, ZLinkPlacementObjectKind.ACTOR);
        ACTOR_FACTORIES.set(0);
        SPOT_FACTORIES.set(0);
        var targetWeight = new AtomicInteger();
        var targetWeightAfter = new AtomicInteger();

        var targetOptions = new DefaultZLinkFrameworkOptions();
        targetOptions.addLocationStore(provider);
        targetOptions
                .addRouteMesh(MESH)
                .listen("tcp://127.0.0.1:0")
                .objects()
                .server()
                .addEntrySpot(Entry.class)
                .addActorFactory(TYPE, Player.class, Factory.class, f -> f.disableRelocation())
                .addSpotFactory(TYPE, Room.class, f -> f.disableRelocation());
        try (var target = start(targetOptions, store)) {
            var endpoint = target.listenerStatus(ZLinkListenerKind.ROUTE_MESH, MESH).endpoint();
            var publishedTarget =
                    store
                            .listMeshNodes(MESH, ZLinkPageRequest.firstPage())
                            .toCompletableFuture()
                            .join()
                            .items()
                            .stream()
                            .filter(descriptor -> descriptor.endpoint().equals(endpoint))
                            .filter(
                                    descriptor ->
                                            descriptor.objectCapabilities().stream()
                                                    .anyMatch(
                                                            capability ->
                                                                    capability.objectKind()
                                                                                    == ZLinkPlacementObjectKind
                                                                                            .ACTOR
                                                                            && capability
                                                                                    .stableType()
                                                                                    .equals(TYPE)
                                                                            && capability.policy()
                                                                                    == ZLinkObjectMaintenancePolicyKind
                                                                                            .DISABLED))
                            .findFirst()
                            .orElseThrow();
            targetWeight.set(publishedTarget.placementWeight());
            targetKey.set(
                    new ZLinkMeshNodeDescriptorKey(
                            publishedTarget.meshName(), publishedTarget.rid()));

            var sourceOptions = new DefaultZLinkFrameworkOptions();
            sourceOptions.addLocationStore(provider);
            var node = sourceOptions.addRouteMesh(MESH).listen("tcp://127.0.0.1:0");
            node.objects().client();
            node.peerConnections().connect(endpoint);
            try (var requester = start(sourceOptions, store)) {
                var created =
                        assertInstanceOf(
                                ZLinkActorCreateResult.Created.class,
                                requester
                                        .actorManager()
                                        .getOrCreate(ID, TYPE)
                                        .submit()
                                        .toCompletableFuture()
                                        .get(10, TimeUnit.SECONDS));
                assertTrue(created.actor().objectGeneration() > before.objectGeneration());
                assertEquals(1, ACTOR_FACTORIES.get());
            }
            var targetAfter =
                    store
                            .listMeshNodes(MESH, ZLinkPageRequest.firstPage())
                            .toCompletableFuture()
                            .join()
                            .items()
                            .stream()
                            .filter(
                                    descriptor ->
                                            targetKey
                                                    .get()
                                                    .equals(
                                                            new ZLinkMeshNodeDescriptorKey(
                                                                    descriptor.meshName(),
                                                                    descriptor.rid())))
                            .findFirst()
                            .orElseThrow();
            targetWeightAfter.set(targetAfter.placementWeight());
            assertTrue(
                    targetAfter.objectCapabilities().stream()
                            .anyMatch(
                                    capability ->
                                            capability.objectKind()
                                                            == ZLinkPlacementObjectKind.ACTOR
                                                    && capability.stableType().equals(TYPE)
                                                    && capability.policy()
                                                            == ZLinkObjectMaintenancePolicyKind
                                                                    .DISABLED));
        }
        assertEquals(1, changes.get());
        assertEquals(targetWeight.get() + 1, targetWeightAfter.get());
    }

    private static ZLinkMeshNodeDescriptor updateDescriptor(
            ZLinkMeshNodeDescriptor descriptor,
            ZLinkPlacementObjectKind kind,
            ZLinkObjectMaintenancePolicyKind policy,
            int placementWeight) {
        var capabilities =
                descriptor.objectCapabilities().stream()
                        .map(
                                capability ->
                                        policy != null
                                                        && capability.objectKind() == kind
                                                        && capability.stableType().equals(TYPE)
                                                ? new ZLinkObjectCapability(
                                                        capability.objectKind(),
                                                        capability.stableType(),
                                                        policy,
                                                        capability.hasSnapshotAdapter(),
                                                        capability.spotLimit(),
                                                        capability.entryAccepted())
                                                : capability)
                        .toList();
        return new ZLinkMeshNodeDescriptor(
                descriptor.meshName(),
                descriptor.rid(),
                descriptor.lifecycleGeneration(),
                descriptor.descriptorRevision() + 1,
                descriptor.endpoint(),
                descriptor.channelWeights(),
                descriptor.applicationVersion(),
                capabilities,
                descriptor.objectRole(),
                descriptor.entrySpotId(),
                placementWeight,
                descriptor.capacity(),
                descriptor.activationConcurrency(),
                descriptor.maintenanceWave(),
                descriptor.state(),
                descriptor.securityIdentity(),
                descriptor.ownerId(),
                descriptor.leaseGeneration(),
                descriptor.updatedAt());
    }

    private static String key(ZLinkPlacementObjectKind kind) {
        return kind == ZLinkPlacementObjectKind.ACTOR
                ? ZLinkAuthorityKeyCodec.actor(ID)
                : ZLinkAuthorityKeyCodec.spot(ID);
    }

    private static ZLinkAuthoritySnapshot endedAuthority(
            ZLinkLocationRepository store, ZLinkPlacementObjectKind kind) {
        var oldOwner = claim(store, "old-" + kind);
        var old = descriptor(store, oldOwner, kind, ZLinkObjectMaintenancePolicyKind.DISABLED);
        boolean actor = kind == ZLinkPlacementObjectKind.ACTOR;
        String key = key(kind);
        var creating = request(old, oldOwner, key, kind, TYPE, null);
        var first =
                assertInstanceOf(
                                ZLinkObjectReserved.class,
                                store.reserve(creating, () -> false).toCompletableFuture().join())
                        .reservation();
        byte[] ready =
                actor
                        ? new ZLinkActorAuthorityPayloadCodec()
                                .encode(
                                        ZLinkActorAuthorityPayloadCodec.State.READY,
                                        TYPE,
                                        ID,
                                        old.entrySpotId().orElseThrow(),
                                        1,
                                        1,
                                        oldOwner.ownerId(),
                                        oldOwner.leaseGeneration(),
                                        old.meshName(),
                                        old.rid(),
                                        1)
                        : new ZLinkServiceAuthorityPayloadCodec()
                                .encodeUser(
                                        ZLinkServiceAuthorityPayloadCodec.State.READY,
                                        TYPE,
                                        ID,
                                        oldOwner.ownerId(),
                                        oldOwner.leaseGeneration(),
                                        old.meshName(),
                                        old.rid(),
                                        1);
        assertEquals(
                ZLinkObjectCommitResult.COMMITTED,
                store.commit(first, ready, () -> false).toCompletableFuture().join());
        var before =
                assertInstanceOf(
                        ZLinkAuthoritySnapshot.class,
                        store.read(key, () -> false).toCompletableFuture().join());
        store.releaseOwnerLease(oldOwner).toCompletableFuture().join();

        return before;
    }

    private static ZLinkLocationOwnerToken claim(ZLinkLocationRepository store, String id) {
        return assertInstanceOf(
                        ZLinkOwnerLeaseClaimed.class,
                        store.claimOwnerLease(id, Duration.ofMinutes(1))
                                .toCompletableFuture()
                                .join())
                .token();
    }

    private static ZLinkMeshNodeDescriptor descriptor(
            ZLinkLocationRepository store,
            ZLinkLocationOwnerToken owner,
            ZLinkPlacementObjectKind kind,
            ZLinkObjectMaintenancePolicyKind policy) {
        var base =
                ZLinkLocationDescriptorFixture.descriptor(
                        owner, owner.ownerId(), ZLinkPlacementObjectKind.USER_SPOT, 1);
        var capabilities =
                List.of(
                        new ZLinkObjectCapability(
                                kind,
                                policy == null ? "other" : "room",
                                policy == null ? ZLinkObjectMaintenancePolicyKind.DISABLED : policy,
                                false,
                                kind == ZLinkPlacementObjectKind.ACTOR ? 0 : 1,
                                kind == ZLinkPlacementObjectKind.ACTOR));
        var descriptor =
                new ZLinkMeshNodeDescriptor(
                        base.meshName(),
                        base.rid(),
                        base.lifecycleGeneration(),
                        base.descriptorRevision(),
                        base.endpoint(),
                        base.channelWeights(),
                        base.applicationVersion(),
                        capabilities,
                        base.objectRole(),
                        base.entrySpotId(),
                        base.placementWeight(),
                        base.capacity(),
                        base.activationConcurrency(),
                        base.maintenanceWave(),
                        base.state(),
                        base.securityIdentity(),
                        base.ownerId(),
                        base.leaseGeneration(),
                        base.updatedAt());
        assertEquals(
                ZLinkLocationWriteStatus.STORED,
                store.updateMeshNode(descriptor, ZLinkLocationWriteIntent.NEW_CLAIM)
                        .toCompletableFuture()
                        .join()
                        .status());
        return descriptor;
    }

    private static ZLinkObjectReservationRequest request(
            ZLinkMeshNodeDescriptor target,
            ZLinkLocationOwnerToken owner,
            String key,
            ZLinkPlacementObjectKind kind,
            String type,
            RelocationPolicy policy) {
        return new ZLinkObjectReservationRequest(
                kind,
                key,
                type,
                "inline:test",
                new byte[32],
                1,
                new ZLinkMeshNodeDescriptorKey(target.meshName(), target.rid()),
                target.lifecycleGeneration(),
                owner,
                new byte[] {1},
                kind == ZLinkPlacementObjectKind.ACTOR
                        ? ZLinkPlacementCapacityBundle.actor(1)
                        : ZLinkPlacementCapacityBundle.spot(kind, type, 1),
                policy);
    }
}
