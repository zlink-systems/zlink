package systems.zlink.framework.runtime.locations;

import systems.zlink.framework.locationprovider.ZLinkLocationStore;
import systems.zlink.framework.runtime.internal.handlers.ZLinkHandlerActivator;
import systems.zlink.framework.runtime.internal.locations
    .ZLinkProviderLocationRepository;

public final class ZLinkLocationStoreResolver {
    private ZLinkLocationStoreResolver() {
    }

    public static ZLinkRegisteredLocationStores resolve(
        ZLinkLocationRegistration registration,
        ZLinkHandlerActivator factory) {
        if (registration == null || !registration.enabled()) {
            return null;
        }

        return fromUnified(registration.storeInstance());
    }

    private static ZLinkRegisteredLocationStores fromUnified(ZLinkLocationStore store) {
        return ZLinkRegisteredLocationStores.fromUnified(
            new ZLinkProviderLocationRepository(store));
    }

}
