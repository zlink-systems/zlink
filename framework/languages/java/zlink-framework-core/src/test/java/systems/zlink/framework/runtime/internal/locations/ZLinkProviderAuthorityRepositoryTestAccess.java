package systems.zlink.framework.runtime.internal.locations;

import systems.zlink.framework.locationprovider.ZLinkStoreKey;

public final class ZLinkProviderAuthorityRepositoryTestAccess {
    private ZLinkProviderAuthorityRepositoryTestAccess() {}

    public static void assertRelocationPhase(
            systems.zlink.framework.locationprovider.ZLinkLocationStore provider,
            String key,
            String phase) {
        var row =
                (systems.zlink.framework.locationprovider.ZLinkStoreReadFound)
                        provider.read(authorityKey(key), () -> false).toCompletableFuture().join();
        try {
            var value =
                    new com.fasterxml.jackson.databind.ObjectMapper().readTree(row.value().bytes());
            org.junit.jupiter.api.Assertions.assertEquals(
                    phase, value.path("aggregate").path("relocation").path("phase").asText());
        } catch (java.io.IOException failure) {
            throw new IllegalStateException(failure);
        }
    }

    public static ZLinkStoreKey authorityKey(String key) {
        return ZLinkProviderAuthorityRepository.authorityKey(key);
    }
}
