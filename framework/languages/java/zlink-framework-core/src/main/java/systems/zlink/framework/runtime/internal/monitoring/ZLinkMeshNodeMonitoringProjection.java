package systems.zlink.framework.runtime.internal.monitoring;

import java.util.ArrayList;
import java.util.List;
import java.util.Objects;
import java.util.Optional;
import systems.zlink.framework.locations.ZLinkCapacityUsage;
import systems.zlink.framework.runtime.internal.locations.ZLinkMeshNodeDescriptor;
import systems.zlink.framework.locations.ZLinkMeshNodeObjectRole;
import systems.zlink.framework.locations.ZLinkObjectCapability;
import systems.zlink.framework.locations.ZLinkObjectMaintenancePolicyKind;
import systems.zlink.framework.locations.ZLinkPlacementCapacity;
import systems.zlink.framework.locations.ZLinkPlacementObjectKind;
import systems.zlink.framework.locations.ZLinkSpotTypeCapacity;
import systems.zlink.framework.locations.ZLinkActivationConcurrency;
import systems.zlink.framework.runtime.mesh.MeshNodeRegistration;
import systems.zlink.framework.runtime.internal.configuration
    .ZLinkObjectFactoryRegistration.RelocationPolicy;

/**
 * Internal projection shared by descriptor publication and runtime monitoring.
 */
public record ZLinkMeshNodeMonitoringProjection(
    long descriptorRevision,
    ZLinkMeshNodeObjectRole objectRole,
    int placementWeight,
    ZLinkPlacementCapacity objectCapacity,
    ZLinkActivationConcurrency activationConcurrency,
    List<ZLinkObjectCapability> objectCapabilities,
    long placementReservationFailureCount,
    Optional<String> lastPlacementReservationFailure) {
    public ZLinkMeshNodeMonitoringProjection {
        Objects.requireNonNull(objectRole, "objectRole");
        Objects.requireNonNull(objectCapacity, "objectCapacity");
        Objects.requireNonNull(activationConcurrency, "activationConcurrency");
        objectCapabilities = List.copyOf(
            Objects.requireNonNull(objectCapabilities, "objectCapabilities"));
        lastPlacementReservationFailure = Objects.requireNonNull(
            lastPlacementReservationFailure,
            "lastPlacementReservationFailure");
    }

    public static ZLinkMeshNodeMonitoringProjection fromDescriptor(
        ZLinkMeshNodeDescriptor descriptor) {
        Objects.requireNonNull(descriptor, "descriptor");
        return new ZLinkMeshNodeMonitoringProjection(
            descriptor.descriptorRevision(),
            descriptor.objectRole(),
            descriptor.placementWeight(),
            descriptor.capacity(),
            new ZLinkActivationConcurrency(
                descriptor.activationConcurrency().active(),
                descriptor.activationConcurrency().limit()),
            descriptor.objectCapabilities(),
            0,
            Optional.empty());
    }

    public static ZLinkMeshNodeMonitoringProjection fromRegistration(
        MeshNodeRegistration registration,
        long descriptorRevision,
        int placementWeight) {
        Objects.requireNonNull(registration, "registration");
        List<ZLinkObjectCapability> capabilities = capabilities(registration);
        return new ZLinkMeshNodeMonitoringProjection(
            descriptorRevision,
            registration.objectServer()
                ? ZLinkMeshNodeObjectRole.SERVER
                : registration.objectRoleEnabled()
                    ? ZLinkMeshNodeObjectRole.CLIENT
                    : ZLinkMeshNodeObjectRole.NONE,
            placementWeight,
            new ZLinkPlacementCapacity(
                new ZLinkCapacityUsage(0, 0, registration.actorCapacity()),
                new ZLinkCapacityUsage(0, 0, registration.spotCapacity()),
                capabilities.stream()
                    .filter(capability ->
                        capability.objectKind() != ZLinkPlacementObjectKind.ACTOR)
                    .map(capability -> new ZLinkSpotTypeCapacity(
                        capability.objectKind(),
                        capability.stableType(),
                        new ZLinkCapacityUsage(0, 0, capability.spotLimit())))
                    .toList()),
            new ZLinkActivationConcurrency(0, registration.activationConcurrency()),
            capabilities,
            0,
            Optional.empty());
    }

    public static List<ZLinkObjectCapability> capabilities(
        MeshNodeRegistration registration) {
        Objects.requireNonNull(registration, "registration");
        List<ZLinkObjectCapability> capabilities = new ArrayList<>();
        registration.relocatableSpotFactories().values().forEach(factory ->
            capabilities.add(capability(
                ZLinkPlacementObjectKind.USER_SPOT,
                factory.stableType(),
                factory.options().stableTypeLimit(),
                factory.relocationPolicy())));
        registration.relocatableInstanceSpotFactories().values().forEach(factory ->
            capabilities.add(capability(
                ZLinkPlacementObjectKind.INSTANCE_SPOT,
                factory.stableType(),
                factory.options().stableTypeLimit(),
                factory.relocationPolicy())));
        registration.relocatableActorFactories().values().forEach(factory ->
            capabilities.add(capability(
                ZLinkPlacementObjectKind.ACTOR,
                factory.stableType(),
                0,
                factory.relocationPolicy())));
        return List.copyOf(capabilities);
    }

    private static ZLinkObjectCapability capability(
        ZLinkPlacementObjectKind kind,
        String stableType,
        int stableTypeLimit,
        RelocationPolicy policy) {
        return new ZLinkObjectCapability(
            kind,
            stableType,
            policy
                instanceof RelocationPolicy.PreserveState
                ? ZLinkObjectMaintenancePolicyKind.SNAPSHOT
                : policy
                    instanceof RelocationPolicy.Recreate
                    ? ZLinkObjectMaintenancePolicyKind.RECREATE
                    : ZLinkObjectMaintenancePolicyKind.DISABLED,
            policy
                instanceof RelocationPolicy.PreserveState,
            kind == ZLinkPlacementObjectKind.ACTOR ? 0 : stableTypeLimit);
    }
}
