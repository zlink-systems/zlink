package systems.zlink.framework.runtime.internal.locations;

import systems.zlink.framework.locationprovider.ZLinkStoreKey;

public final class ZLinkProviderAuthorityRepositoryTestAccess {
    private ZLinkProviderAuthorityRepositoryTestAccess() {}

    public static ZLinkStoreKey authorityKey(String key) {
        return ZLinkProviderAuthorityRepository.authorityKey(key);
    }
}
