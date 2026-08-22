package systems.zlink.framework.runtime.internal.locations;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertThrows;
import static org.junit.jupiter.api.Assertions.assertTrue;

import java.time.Instant;
import java.util.List;
import java.util.Map;
import java.util.Optional;
import org.junit.jupiter.api.Test;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.locations.ZLinkActivationConcurrency;
import systems.zlink.framework.locations.ZLinkCapacityUsage;
import systems.zlink.framework.locations.ZLinkMeshNodeObjectRole;
import systems.zlink.framework.locations.ZLinkObjectCapability;
import systems.zlink.framework.locations.ZLinkObjectMaintenancePolicyKind;
import systems.zlink.framework.locations.ZLinkPlacementCapacity;
import systems.zlink.framework.locations.ZLinkPlacementObjectKind;
import systems.zlink.framework.runtime.host.ZLinkFrameworkRuntimeState;

/**
 * Covers the unidirectional entrySpotId/objectRole invariant: an
 * entrySpotId, when present, requires role SERVER, but a SERVER descriptor
 * is not required to publish an entrySpotId (Node/C++ Object Servers may
 * legitimately publish with no Entry Spot registered).
 */
final class ZLinkMeshNodeDescriptorTest {
    private static final Instant NOW = Instant.parse("2026-08-22T00:00:00Z");
    private static final RoutingId RID = RoutingId.from("mesh-node");

    @Test
    void serverDescriptorWithoutEntrySpotIdDecodesSuccessfully() {
        ZLinkMeshNodeDescriptor descriptor = descriptor(
            ZLinkMeshNodeObjectRole.SERVER,
            Optional.empty());

        assertEquals(
            ZLinkMeshNodeObjectRole.SERVER,
            descriptor.objectRole());
        assertTrue(descriptor.entrySpotId().isEmpty());
    }

    @Test
    void serverDescriptorWithEntrySpotIdDecodesSuccessfully() {
        ZLinkMeshNodeDescriptor descriptor = descriptor(
            ZLinkMeshNodeObjectRole.SERVER,
            Optional.of("mesh-entry-1"));

        assertEquals(
            ZLinkMeshNodeObjectRole.SERVER,
            descriptor.objectRole());
        assertEquals(
            Optional.of("mesh-entry-1"),
            descriptor.entrySpotId());
    }

    @Test
    void nonServerDescriptorWithEntrySpotIdIsRejected() {
        IllegalArgumentException error = assertThrows(
            IllegalArgumentException.class,
            () -> descriptor(
                ZLinkMeshNodeObjectRole.NONE,
                Optional.of("mesh-entry-1")));

        assertTrue(error.getMessage()
            .contains("Only an Object Server descriptor must publish"
                + " entrySpotId"));
    }

    @Test
    void nonServerDescriptorWithoutEntrySpotIdDecodesSuccessfully() {
        ZLinkMeshNodeDescriptor descriptor = descriptor(
            ZLinkMeshNodeObjectRole.NONE,
            Optional.empty());

        assertEquals(
            ZLinkMeshNodeObjectRole.NONE,
            descriptor.objectRole());
        assertTrue(descriptor.entrySpotId().isEmpty());
    }

    @Test
    void nonServerDescriptorWithObjectCapabilitiesDecodesSuccessfully() {
        ZLinkObjectCapability capability = new ZLinkObjectCapability(
            ZLinkPlacementObjectKind.ACTOR,
            "player",
            ZLinkObjectMaintenancePolicyKind.DISABLED,
            false,
            0);

        ZLinkMeshNodeDescriptor descriptor = descriptor(
            ZLinkMeshNodeObjectRole.NONE,
            Optional.empty(),
            List.of(capability));

        assertEquals(
            List.of(capability),
            descriptor.objectCapabilities());
    }

    private static ZLinkMeshNodeDescriptor descriptor(
        ZLinkMeshNodeObjectRole role,
        Optional<String> entrySpotId) {
        return descriptor(role, entrySpotId, List.of());
    }

    private static ZLinkMeshNodeDescriptor descriptor(
        ZLinkMeshNodeObjectRole role,
        Optional<String> entrySpotId,
        List<ZLinkObjectCapability> objectCapabilities) {
        return new ZLinkMeshNodeDescriptor(
            "mesh",
            RID,
            1,
            1,
            "tcp://127.0.0.1:7000",
            Map.of(),
            1,
            objectCapabilities,
            role,
            entrySpotId,
            100,
            new ZLinkPlacementCapacity(
                new ZLinkCapacityUsage(0, 0, 0),
                new ZLinkCapacityUsage(0, 0, 0),
                List.of()),
            new ZLinkActivationConcurrency(0, 1),
            Optional.empty(),
            ZLinkFrameworkRuntimeState.SERVING,
            "security",
            "owner",
            1,
            NOW);
    }
}
