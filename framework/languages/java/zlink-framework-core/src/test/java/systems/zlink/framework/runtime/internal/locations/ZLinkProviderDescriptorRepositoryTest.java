package systems.zlink.framework.runtime.internal.locations;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertInstanceOf;

import java.time.Duration;
import java.time.Instant;
import java.util.List;
import java.util.Map;
import java.util.Optional;
import org.junit.jupiter.api.Test;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.locations.ZLinkActivationConcurrency;
import systems.zlink.framework.locations.ZLinkCapacityUsage;
import systems.zlink.framework.runtime.internal.locations.ZLinkLocationWriteIntent;
import systems.zlink.framework.runtime.internal.locations.ZLinkLocationWriteStatus;
import systems.zlink.framework.runtime.internal.locations.ZLinkMeshNodeDescriptor;
import systems.zlink.framework.runtime.internal.locations.ZLinkMeshNodeDescriptorKey;
import systems.zlink.framework.locations.ZLinkMeshNodeObjectRole;
import systems.zlink.framework.runtime.internal.locations.ZLinkOwnerLeaseClaimed;
import systems.zlink.framework.locations.ZLinkPageRequest;
import systems.zlink.framework.locations.ZLinkPlacementCapacity;
import systems.zlink.framework.runtime.host.ZLinkFrameworkRuntimeState;
import systems.zlink.framework.runtime.locations
    .ZLinkInMemoryProviderLocationStore;

class ZLinkProviderDescriptorRepositoryTest {
    @Test
    void meshDescriptorUsesOwnerVersionFenceAndOpaqueSnapshot()
        throws Exception {
        var provider = new ZLinkInMemoryProviderLocationStore();
        var owners = new ZLinkProviderOwnerLeaseRepository(provider);
        var descriptors = new ZLinkProviderDescriptorRepository(provider);
        var owner = assertInstanceOf(
            ZLinkOwnerLeaseClaimed.class,
            owners.claim("owner-a", Duration.ofMinutes(1))
                .toCompletableFuture().get()).token();

        var first = descriptor(owner, 1);
        assertEquals(
            ZLinkLocationWriteStatus.STORED,
            descriptors.updateMeshNode(
                    first,
                    ZLinkLocationWriteIntent.NEW_CLAIM)
                .toCompletableFuture().get().status());
        assertEquals(
            first.rid(),
            descriptors.listMeshNodes(
                    "game",
                    ZLinkPageRequest.firstPage())
                .toCompletableFuture().get()
                .items().getFirst().rid());

        var renewed = descriptor(owner, 2);
        assertEquals(
            ZLinkLocationWriteStatus.STORED,
            descriptors.updateMeshNode(
                    renewed,
                    ZLinkLocationWriteIntent.RENEW)
                .toCompletableFuture().get().status());

        owners.release(owner).toCompletableFuture().get();
        assertEquals(
            ZLinkLocationWriteStatus.IGNORED_STALE,
            descriptors.updateMeshNode(
                    descriptor(owner, 3),
                    ZLinkLocationWriteIntent.RENEW)
                .toCompletableFuture().get().status());
        assertEquals(
            ZLinkLocationWriteStatus.STORED,
            descriptors.removeMeshNode(
                    new ZLinkMeshNodeDescriptorKey(
                        "game",
                        first.rid()),
                    owner)
                .toCompletableFuture().get());
    }

    @Test
    void sameLifecycleRenewRejectsImmutableTopologyChanges()
        throws Exception {
        var provider = new ZLinkInMemoryProviderLocationStore();
        var owners = new ZLinkProviderOwnerLeaseRepository(provider);
        var descriptors = new ZLinkProviderDescriptorRepository(provider);
        var owner = assertInstanceOf(
            ZLinkOwnerLeaseClaimed.class,
            owners.claim("owner-a", Duration.ofMinutes(1))
                .toCompletableFuture().get()).token();

        assertEquals(
            ZLinkLocationWriteStatus.STORED,
            descriptors.updateMeshNode(
                    descriptor(owner, 1),
                    ZLinkLocationWriteIntent.NEW_CLAIM)
                .toCompletableFuture().get().status());

        for (ZLinkMeshNodeDescriptor changed : List.of(
            descriptor(
                owner, 2, "tcp://127.0.0.1:7001",
                Map.of(), ZLinkMeshNodeObjectRole.SERVER, "security-a"),
            descriptor(
                owner, 2, "tcp://127.0.0.1:7000",
                Map.of("events", 1), ZLinkMeshNodeObjectRole.SERVER,
                "security-a"),
            descriptor(
                owner, 2, "tcp://127.0.0.1:7000",
                Map.of(), ZLinkMeshNodeObjectRole.CLIENT, "security-a"),
            descriptor(
                owner, 2, "tcp://127.0.0.1:7000",
                Map.of(), ZLinkMeshNodeObjectRole.SERVER, "security-b"))) {
            assertEquals(
                ZLinkLocationWriteStatus.IGNORED_STALE,
                descriptors.updateMeshNode(
                        changed,
                        ZLinkLocationWriteIntent.RENEW)
                    .toCompletableFuture().get().status());
        }
    }

    private static ZLinkMeshNodeDescriptor descriptor(
        systems.zlink.framework.runtime.internal.locations.ZLinkLocationOwnerToken owner,
        long revision) {
        return descriptor(
            owner,
            revision,
            "tcp://127.0.0.1:7000",
            Map.of(),
            ZLinkMeshNodeObjectRole.SERVER,
            "security-a");
    }

    private static ZLinkMeshNodeDescriptor descriptor(
        ZLinkLocationOwnerToken owner,
        long revision,
        String endpoint,
        Map<String, Integer> channels,
        ZLinkMeshNodeObjectRole role,
        String securityIdentity) {
        return new ZLinkMeshNodeDescriptor(
            "game",
            RoutingId.from("node-a"),
            1,
            revision,
            endpoint,
            channels,
            1,
            List.of(),
            role,
            role == ZLinkMeshNodeObjectRole.SERVER
                ? Optional.of(
                    "node-a-entry-00000000-0000-4000-8000-000000000001")
                : Optional.empty(),
            100,
            new ZLinkPlacementCapacity(
                new ZLinkCapacityUsage(0, 0, 100),
                new ZLinkCapacityUsage(0, 0, 100),
                List.of()),
            new ZLinkActivationConcurrency(0, 64),
            Optional.empty(),
            ZLinkFrameworkRuntimeState.SERVING,
            securityIdentity,
            owner.ownerId(),
            owner.leaseGeneration(),
            Instant.parse("2026-07-29T00:00:00Z"));
    }
}
