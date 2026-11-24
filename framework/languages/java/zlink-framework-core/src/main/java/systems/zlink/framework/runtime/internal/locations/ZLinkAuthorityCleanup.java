package systems.zlink.framework.runtime.internal.locations;

import systems.zlink.framework.errors.ZLinkFrameworkErrorKind;
import systems.zlink.framework.errors.ZLinkFrameworkException;
import systems.zlink.framework.locations.ZLinkObjectMaintenancePolicyKind;

/** Location runtime §6.1 creation authority release rules. */
public final class ZLinkAuthorityCleanup {
    private ZLinkAuthorityCleanup() {}

    public static void requireDisabledCreationPolicy(
            ZLinkObjectReservationRequest request, ZLinkMeshNodeDescriptor target) {
        if (target != null
                && target.lifecycleGeneration() == request.targetDescriptorLifecycleGeneration()
                && target.objectCapabilities().stream()
                        .anyMatch(
                                capability ->
                                        capability.objectKind() == request.objectKind()
                                                && capability
                                                        .stableType()
                                                        .equals(request.stableType())
                                                && capability.policy()
                                                        == ZLinkObjectMaintenancePolicyKind
                                                                .DISABLED)) return;
        throw new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.UNAVAILABLE, "Object owner lease is unavailable");
    }
}
