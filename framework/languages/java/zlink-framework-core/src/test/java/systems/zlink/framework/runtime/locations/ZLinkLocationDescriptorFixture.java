package systems.zlink.framework.runtime.locations;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.locations.*;
import systems.zlink.framework.runtime.host.ZLinkFrameworkRuntimeState;
import systems.zlink.framework.runtime.internal.locations.ZLinkLocationOwnerToken;
import systems.zlink.framework.runtime.internal.locations.ZLinkMeshNodeDescriptor;

import java.time.Instant;
import java.util.ArrayList;
import java.util.Map;
import java.util.Optional;

final class ZLinkLocationDescriptorFixture {
    private ZLinkLocationDescriptorFixture() {}

    static ZLinkMeshNodeDescriptor descriptor(
            ZLinkLocationOwnerToken owner,
            String rid,
            ZLinkPlacementObjectKind kind,
            int actorLimit) {
        return descriptor(
                owner, rid, kind, actorLimit, ZLinkObjectMaintenancePolicyKind.DISABLED, 1);
    }

    static ZLinkMeshNodeDescriptor descriptor(
            ZLinkLocationOwnerToken owner,
            String rid,
            ZLinkPlacementObjectKind kind,
            int actorLimit,
            long revision) {
        return descriptor(
                owner, rid, kind, actorLimit, ZLinkObjectMaintenancePolicyKind.DISABLED, revision);
    }

    static ZLinkMeshNodeDescriptor descriptor(
            ZLinkLocationOwnerToken owner,
            String rid,
            ZLinkPlacementObjectKind kind,
            int actorLimit,
            ZLinkObjectMaintenancePolicyKind spotPolicy) {
        return descriptor(owner, rid, kind, actorLimit, spotPolicy, 1);
    }

    static ZLinkMeshNodeDescriptor descriptor(
            ZLinkLocationOwnerToken owner,
            String rid,
            ZLinkPlacementObjectKind kind,
            int actorLimit,
            ZLinkObjectMaintenancePolicyKind spotPolicy,
            long revision) {
        var capabilities = new ArrayList<ZLinkObjectCapability>();
        if (spotPolicy != null) {
            capabilities.add(new ZLinkObjectCapability(kind, "room", spotPolicy, false, 1));
        }
        capabilities.add(
                new ZLinkObjectCapability(
                        ZLinkPlacementObjectKind.ACTOR,
                        "player",
                        ZLinkObjectMaintenancePolicyKind.DISABLED,
                        false,
                        0));
        return new ZLinkMeshNodeDescriptor(
                "mesh",
                RoutingId.from(rid),
                1,
                revision,
                "tcp://127.0.0.1:7100",
                Map.of(),
                1,
                capabilities,
                ZLinkMeshNodeObjectRole.SERVER,
                Optional.of(rid + "-entry-00000000-0000-4000-8000-000000000001"),
                100,
                new ZLinkPlacementCapacity(
                        new ZLinkCapacityUsage(0, 0, actorLimit),
                        new ZLinkCapacityUsage(0, 0, 1),
                        java.util.List.of()),
                new ZLinkActivationConcurrency(0, 64),
                Optional.empty(),
                ZLinkFrameworkRuntimeState.SERVING,
                "security",
                owner.ownerId(),
                owner.leaseGeneration(),
                Instant.now());
    }
}
