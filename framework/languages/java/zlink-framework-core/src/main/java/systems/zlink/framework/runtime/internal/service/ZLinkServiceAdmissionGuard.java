package systems.zlink.framework.runtime.internal.service;

import java.util.Objects;

/**
 * Validates one admission against the expected route. Core selects the physical pipe of each RID
 * (Core ROUTER §10.1); this guard only checks the logical identity carried by the handshake.
 */
public final class ZLinkServiceAdmissionGuard {
    private ZLinkServiceAdmissionGuard() {}

    public static boolean matchesExpectedRoute(
            String expectedEndpoint,
            String expectedSecurityIdentity,
            long expectedLifecycleGeneration,
            ZLinkServiceNodeDescriptor incoming) {
        Objects.requireNonNull(incoming, "incoming");
        return (expectedEndpoint == null || expectedEndpoint.equals(incoming.advertisedEndpoint()))
                && (expectedSecurityIdentity == null
                        || expectedSecurityIdentity.equals(incoming.securityIdentity()))
                && (expectedLifecycleGeneration == 0
                        || expectedLifecycleGeneration == incoming.lifecycleGeneration());
    }
}
