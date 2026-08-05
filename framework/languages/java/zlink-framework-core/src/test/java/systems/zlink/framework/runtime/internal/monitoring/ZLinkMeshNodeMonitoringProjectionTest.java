package systems.zlink.framework.runtime.internal.monitoring;

import static org.junit.jupiter.api.Assertions.assertEquals;

import java.time.Instant;
import java.util.List;
import java.util.Map;
import java.util.Optional;
import org.junit.jupiter.api.Test;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.locations.ZLinkActivationConcurrency;
import systems.zlink.framework.locations.ZLinkCapacityUsage;
import systems.zlink.framework.runtime.internal.locations.ZLinkMeshNodeDescriptor;
import systems.zlink.framework.locations.ZLinkMeshNodeObjectRole;
import systems.zlink.framework.locations.ZLinkObjectCapability;
import systems.zlink.framework.locations.ZLinkObjectMaintenancePolicyKind;
import systems.zlink.framework.locations.ZLinkPlacementCapacity;
import systems.zlink.framework.locations.ZLinkPlacementObjectKind;
import systems.zlink.framework.locations.ZLinkSpotTypeCapacity;
import systems.zlink.framework.runtime.host.ZLinkFrameworkRuntimeState;
import systems.zlink.framework.runtime.mesh.MeshNodeRegistration;

final class ZLinkMeshNodeMonitoringProjectionTest {
    @Test
    void descriptorProjectionPreservesCapacityAndActivationValues() {
        ZLinkPlacementCapacity capacity = new ZLinkPlacementCapacity(
            new ZLinkCapacityUsage(3, 2, 0),
            new ZLinkCapacityUsage(4, 1, 10),
            List.of(new ZLinkSpotTypeCapacity(
                ZLinkPlacementObjectKind.USER_SPOT,
                "room",
                new ZLinkCapacityUsage(2, 1, 5))));
        ZLinkMeshNodeDescriptor descriptor = new ZLinkMeshNodeDescriptor(
            "mesh",
            RoutingId.from("node"),
            1,
            7,
            "inproc://mesh",
            Map.of(),
            0,
            List.of(new ZLinkObjectCapability(
                ZLinkPlacementObjectKind.USER_SPOT,
                "room",
                ZLinkObjectMaintenancePolicyKind.DISABLED,
                false,
                5)),
            ZLinkMeshNodeObjectRole.SERVER,
            Optional.of("entry"),
            90,
            capacity,
            new ZLinkActivationConcurrency(2, 8),
            Optional.empty(),
            ZLinkFrameworkRuntimeState.SERVING,
            "security",
            "owner",
            1,
            Instant.now());

        ZLinkMeshNodeMonitoringProjection projection =
            ZLinkMeshNodeMonitoringProjection.fromDescriptor(descriptor);

        assertEquals(capacity, projection.objectCapacity());
        assertEquals(0, projection.objectCapacity().actors().limit());
        assertEquals(2, projection.activationConcurrency().active());
        assertEquals(8, projection.activationConcurrency().limit());
        assertEquals(5, projection.objectCapacity().spotTypes().getFirst().usage().limit());
    }

    @Test
    void registrationFallbackKeepsConfiguredLimitsWhenDescriptorIsUnavailable() {
        MeshNodeRegistration registration = new MeshNodeRegistration("mesh");
        registration.setActorCapacity(0);
        registration.setSpotCapacity(24);
        registration.setActivationConcurrency(6);

        ZLinkMeshNodeMonitoringProjection projection =
            ZLinkMeshNodeMonitoringProjection.fromRegistration(
                registration,
                3,
                75);

        assertEquals(ZLinkMeshNodeObjectRole.NONE, projection.objectRole());
        assertEquals(75, projection.placementWeight());
        assertEquals(0, projection.objectCapacity().actors().limit());
        assertEquals(24, projection.objectCapacity().spots().limit());
        assertEquals(6, projection.activationConcurrency().limit());
    }
}
