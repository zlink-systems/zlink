package systems.zlink.framework.runtime.internal.locations;

public final class ZLinkAggregateRelocationCoordinatorTestAccess {
    private ZLinkAggregateRelocationCoordinatorTestAccess() {}

    public static ZLinkLocationRepository authorityStore(
            ZLinkAggregateRelocationCoordinator coordinator) {
        return coordinator.authorityStore;
    }

    public static void replaceAuthorityStore(
            ZLinkAggregateRelocationCoordinator coordinator,
            ZLinkLocationRepository authorityStore) {
        coordinator.authorityStore = authorityStore;
    }
}
