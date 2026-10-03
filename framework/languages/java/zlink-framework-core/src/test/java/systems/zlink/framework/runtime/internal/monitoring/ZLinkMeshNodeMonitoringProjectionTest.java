package systems.zlink.framework.runtime.internal.monitoring;

import static org.junit.jupiter.api.Assertions.assertEquals;

import org.junit.jupiter.api.Test;

import systems.zlink.framework.locations.ZLinkMeshNodeObjectRole;
import systems.zlink.framework.runtime.mesh.MeshNodeRegistration;

final class ZLinkMeshNodeMonitoringProjectionTest {
    @Test
    void registrationProjectionKeepsConfiguredLimits() {
        MeshNodeRegistration registration = new MeshNodeRegistration("mesh");
        registration.setActorCapacity(0);
        registration.setSpotCapacity(24);
        registration.setActivationConcurrency(6);

        ZLinkMeshNodeMonitoringProjection projection =
                ZLinkMeshNodeMonitoringProjection.fromRegistration(registration, 3, 75);

        assertEquals(ZLinkMeshNodeObjectRole.NONE, projection.objectRole());
        assertEquals(75, projection.placementWeight());
        assertEquals(0, projection.objectCapacity().actors().limit());
        assertEquals(24, projection.objectCapacity().spots().limit());
        assertEquals(6, projection.activationConcurrency().limit());
    }
}
