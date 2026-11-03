package systems.zlink.framework.runtime.binding;

import systems.zlink.framework.runtime.internal.backend.ZLinkBackendSpot;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendSpotDispatchHandler;
import systems.zlink.framework.runtime.internal.backend.ZLinkInternalSpotNode;
import systems.zlink.framework.runtime.internal.execution.ZLinkStateLane;

/** Test-only access to the binding-owned SpotNode state used by runtime tests. */
public final class ZLinkJavaRawSpotNodeTestAccess {
    private ZLinkJavaRawSpotNodeTestAccess() {}

    public static void replaceStateLaneForTest(
            ZLinkInternalSpotNode node, ZLinkStateLane stateLane) {
        ((ZLinkJavaRawSpotNode) node).stateLane = stateLane;
    }

    public static String nativeActorSpotId(ZLinkInternalSpotNode node, String actorId) {
        var stay = ((ZLinkJavaRawSpotNode) node).actors.get(actorId);
        return stay == null ? null : stay.spotId();
    }

    public static ZLinkBackendSpotDispatchHandler dispatchHandler(ZLinkBackendSpot spot) {
        return ((ZLinkJavaRawSpot) spot).dispatchHandler;
    }

    public static ZLinkInternalSpotNode.MessageFollowRelayHandler messageFollowRelayHandler(
            ZLinkInternalSpotNode node) {
        return ((ZLinkJavaRawSpotNode) node).messageFollowRelayHandler;
    }

    public static ZLinkInternalSpotNode.RelocationStagingIngressHandler
            relocationStagingIngressHandler(ZLinkInternalSpotNode node) {
        return ((ZLinkJavaRawSpotNode) node).relocationStagingIngressHandler;
    }
}
