package systems.zlink.framework.runtime.internal.service;

public final class ZLinkServiceOperationRegistryTestAccess {
    private ZLinkServiceOperationRegistryTestAccess() {}

    public static boolean maintenanceStarted(ZLinkServiceOperationRegistry registry) {
        return registry.maintenance != null;
    }
}
