package systems.zlink.framework.runtime.locations;

import systems.zlink.framework.runtime.internal.handlers.ZLinkHandlerActivator;
import systems.zlink.framework.runtime.internal.locations.ZLinkProviderLocationRepository;

public final class ZLinkLocationStoreResolver {
    private ZLinkLocationStoreResolver() {}

    public static ZLinkRegisteredLocationStores resolve(
            ZLinkLocationRegistration registration, ZLinkHandlerActivator factory) {
        if (registration == null || !registration.enabled()) {
            return null;
        }

        return ZLinkRegisteredLocationStores.fromUnified(
                new ZLinkProviderLocationRepository(
                        registration.storeInstance(),
                        ZLinkLocationRuntime.SYSTEM_MONOTONIC_NANOS,
                        registration.options().ownerLeaseFencingMargin()));
    }
}
