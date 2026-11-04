package systems.zlink.framework.runtime.internal.locations;

import systems.zlink.framework.locationprovider.ZLinkLocationStore;

public final class ZLinkProviderLocationRepositoryTestAccess {
    private ZLinkProviderLocationRepositoryTestAccess() {}

    public static ZLinkLocationStore provider(ZLinkProviderLocationRepository repository) {
        return repository.provider;
    }
}
