package systems.zlink.framework.runtime.internal.service;

import systems.zlink.framework.runtime.internal.execution.ZLinkStateLane;

import java.util.UUID;

public final class ZLinkServiceOperationRegistryTestAccess {
    private ZLinkServiceOperationRegistryTestAccess() {}

    public static ZLinkStateLane ownerLane(ZLinkServiceOperationRegistry registry) {
        return registry.ownerLaneForTest();
    }

    public static UUID onlyPendingId(ZLinkServiceOperationRegistry registry) {
        var ids = registry.pendingIdsForTest();
        if (ids.size() != 1) throw new AssertionError("expected one pending request: " + ids);
        return ids.iterator().next();
    }

    public static boolean maintenanceStarted(ZLinkServiceOperationRegistry registry) {
        return registry.maintenance != null;
    }
}
