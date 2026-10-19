package systems.zlink.framework.runtime.internal.locations;

import static org.junit.jupiter.api.Assertions.*;

import systems.zlink.framework.locationprovider.ZLinkLocationStore;
import systems.zlink.framework.runtime.internal.metrics.ZLinkRuntimeMetricProbe;

import java.util.*;
import java.util.concurrent.*;

final class ZLinkLocationOperationMetricTest {
    @org.junit.jupiter.params.ParameterizedTest
    @org.junit.jupiter.params.provider.CsvSource({
        "renew,lease_renew",
        "claim,compare_exchange",
        "owner_read,read",
        "release,release",
        "list,read",
        "authority_read,read",
        "remove_all_owner,release"
    })
    void failuresUseTheLogicalStoreOperation(String selectedMethod, String operation)
            throws Exception {
        var original = new IllegalStateException("store unavailable");
        var store =
                (ZLinkLocationStore)
                        java.lang.reflect.Proxy.newProxyInstance(
                                ZLinkLocationStore.class.getClassLoader(),
                                new Class<?>[] {ZLinkLocationStore.class},
                                (proxy, method, args) -> CompletableFuture.failedFuture(original));
        try (var metrics = ZLinkRuntimeMetricProbe.install()) {
            var repository = new ZLinkProviderLocationRepository(store);
            assertThrows(
                    CompletionException.class,
                    () -> {
                        CompletionStage<?> request =
                                switch (selectedMethod) {
                                    case "renew" ->
                                            repository.renewOwnerLease(
                                                    new ZLinkLocationOwnerToken("owner", 1),
                                                    java.time.Duration.ofSeconds(30));
                                    case "claim" ->
                                            repository.claimOwnerLease(
                                                    "owner", java.time.Duration.ofSeconds(30));
                                    case "owner_read" -> repository.readOwnerLease("owner");
                                    case "release" ->
                                            repository.releaseOwnerLease(
                                                    new ZLinkLocationOwnerToken("owner", 1));
                                    case "list" ->
                                            repository.listMeshNodes(
                                                    "mesh",
                                                    new systems.zlink.framework.locations
                                                            .ZLinkPageRequest(1, null));
                                    case "authority_read" ->
                                            repository.read(
                                                    systems.zlink.framework.runtime.locations
                                                            .ZLinkAuthorityKeyCodec.actor("actor"),
                                                    () -> false);
                                    case "remove_all_owner" ->
                                            repository.removeAllByOwner(
                                                    new ZLinkLocationOwnerToken("owner", 1));
                                    default -> throw new AssertionError(selectedMethod);
                                };
                        request.toCompletableFuture().join();
                    });
            assertEquals(
                    operation, metrics.only("zlink.location.store.errors").tags().get("operation"));
        }
    }
}
