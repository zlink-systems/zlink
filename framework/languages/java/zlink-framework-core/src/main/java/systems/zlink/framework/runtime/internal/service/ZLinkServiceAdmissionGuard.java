package systems.zlink.framework.runtime.internal.service;

import java.util.Objects;
import systems.zlink.contracts.core.RoutingId;

/** Validates one admission and selects one physical pipe per peer lifecycle. */
public final class ZLinkServiceAdmissionGuard {
    private ZLinkServiceAdmissionGuard() {
    }

    public static boolean matchesExpectedRoute(
        String expectedEndpoint,
        String expectedSecurityIdentity,
        long expectedLifecycleGeneration,
        ZLinkServiceNodeDescriptor incoming) {
        Objects.requireNonNull(incoming, "incoming");
        return (expectedEndpoint == null
                || expectedEndpoint.equals(incoming.advertisedEndpoint()))
            && (expectedSecurityIdentity == null
                || expectedSecurityIdentity.equals(
                    incoming.securityIdentity()))
            && (expectedLifecycleGeneration == 0
                || expectedLifecycleGeneration
                    == incoming.lifecycleGeneration());
    }

    public static DuplicateConnectionDecision selectConnection(
        RoutingId localRid,
        RoutingId peerRid,
        long currentLifecycleGeneration,
        ConnectionDirection currentDirection,
        String currentDiscriminator,
        long incomingLifecycleGeneration,
        ConnectionDirection incomingDirection,
        String incomingDiscriminator) {
        Objects.requireNonNull(localRid, "localRid");
        Objects.requireNonNull(peerRid, "peerRid");
        Objects.requireNonNull(currentDirection, "currentDirection");
        Objects.requireNonNull(incomingDirection, "incomingDirection");
        Objects.requireNonNull(
            currentDiscriminator, "currentDiscriminator");
        Objects.requireNonNull(
            incomingDiscriminator, "incomingDiscriminator");

        if (currentLifecycleGeneration != 0
            && currentLifecycleGeneration != incomingLifecycleGeneration) {
            return DuplicateConnectionDecision.NOT_DUPLICATE;
        }

        ConnectionDirection preferredDirection =
            localRid.toHex().compareTo(peerRid.toHex()) < 0
                ? ConnectionDirection.OUTBOUND
                : ConnectionDirection.INBOUND;
        if (currentDirection != incomingDirection) {
            return currentDirection == preferredDirection
                ? DuplicateConnectionDecision.KEEP_CURRENT
                : DuplicateConnectionDecision.USE_INCOMING;
        }
        return currentDiscriminator.compareTo(incomingDiscriminator) <= 0
            ? DuplicateConnectionDecision.KEEP_CURRENT
            : DuplicateConnectionDecision.USE_INCOMING;
    }

    public enum ConnectionDirection {
        INBOUND,
        OUTBOUND
    }

    public enum DuplicateConnectionDecision {
        NOT_DUPLICATE,
        KEEP_CURRENT,
        USE_INCOMING
    }
}
