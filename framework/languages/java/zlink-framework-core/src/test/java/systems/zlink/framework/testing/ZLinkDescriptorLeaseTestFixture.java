package systems.zlink.framework.testing;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.locationprovider.ZLinkLocationStore;
import systems.zlink.framework.locationprovider.ZLinkStoreCancellation;
import systems.zlink.framework.locationprovider.ZLinkStoreReadFound;
import systems.zlink.framework.locationprovider.ZLinkStoreReadResult;
import systems.zlink.framework.locationprovider.ZLinkStoreScanRequest;
import systems.zlink.framework.locationprovider.ZLinkStoreScanResult;
import systems.zlink.framework.locationprovider.ZLinkStoreValue;
import systems.zlink.framework.locationprovider.ZLinkStoreWriteRequest;
import systems.zlink.framework.locationprovider.ZLinkStoreWriteResult;
import systems.zlink.framework.locations.ZLinkActivationConcurrency;
import systems.zlink.framework.locations.ZLinkCapacityUsage;
import systems.zlink.framework.locations.ZLinkLocationOptions;
import systems.zlink.framework.locations.ZLinkLocationPage;
import systems.zlink.framework.locations.ZLinkMeshNodeObjectRole;
import systems.zlink.framework.locations.ZLinkPlacementCapacity;
import systems.zlink.framework.runtime.host.ZLinkFrameworkRuntimeState;
import systems.zlink.framework.runtime.internal.locations.ZLinkLocationOwnerToken;
import systems.zlink.framework.runtime.internal.locations.ZLinkLocationRepository;
import systems.zlink.framework.runtime.internal.locations.ZLinkMeshNodeDescriptor;
import systems.zlink.framework.runtime.internal.locations.ZLinkOwnerLeaseClaimed;
import systems.zlink.framework.runtime.internal.locations.ZLinkOwnerLeaseFound;
import systems.zlink.framework.runtime.internal.locations.ZLinkProviderLocationRepository;
import systems.zlink.framework.runtime.locations.ZLinkInMemoryProviderLocationStore;
import systems.zlink.framework.runtime.locations.ZLinkRegisteredLocationStores;
import systems.zlink.framework.runtime.locations.ZLinkStoreLocationResolvers;

import java.lang.reflect.Proxy;
import java.time.Clock;
import java.time.Duration;
import java.time.Instant;
import java.time.ZoneOffset;
import java.util.HashMap;
import java.util.List;
import java.util.Map;
import java.util.Optional;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;

/** Deterministic owner-lease reads for descriptor target-selection tests. */
public final class ZLinkDescriptorLeaseTestFixture {
    public static final Instant STORE_NOW = Instant.parse("2026-07-27T00:00:00Z");

    private ZLinkDescriptorLeaseTestFixture() {}

    public enum LeaseState {
        LIVE,
        EXPIRED,
        MISSING_EXPIRY
    }

    public static ZLinkStoreLocationResolvers resolver(
            List<ZLinkMeshNodeDescriptor> descriptors, Map<String, LeaseState> leases) {
        Map<String, ZLinkMeshNodeDescriptor> byOwner = new HashMap<>();
        for (ZLinkMeshNodeDescriptor descriptor : descriptors) {
            byOwner.put(descriptor.ownerId(), descriptor);
        }
        Map<String, ZLinkLocationRepository> corrupt = new HashMap<>();
        leases.forEach(
                (ownerId, state) -> {
                    if (state == LeaseState.MISSING_EXPIRY) {
                        corrupt.put(ownerId, corruptLeaseRepository(ownerId));
                    }
                });
        ZLinkLocationRepository store =
                (ZLinkLocationRepository)
                        Proxy.newProxyInstance(
                                ZLinkDescriptorLeaseTestFixture.class.getClassLoader(),
                                new Class<?>[] {ZLinkLocationRepository.class},
                                (proxy, method, arguments) ->
                                        switch (method.getName()) {
                                            case "listMeshNodes" ->
                                                    CompletableFuture.completedFuture(
                                                            new ZLinkLocationPage<>(
                                                                    descriptors, null));
                                            case "readOwnerLease" -> {
                                                String ownerId = (String) arguments[0];
                                                LeaseState state = leases.get(ownerId);
                                                ZLinkMeshNodeDescriptor descriptor =
                                                        byOwner.get(ownerId);
                                                if (state == LeaseState.MISSING_EXPIRY) {
                                                    yield corrupt.get(ownerId)
                                                            .readOwnerLease(ownerId);
                                                }
                                                Instant expiresAt =
                                                        state == LeaseState.LIVE
                                                                ? STORE_NOW.plusSeconds(30)
                                                                : STORE_NOW;
                                                yield CompletableFuture.completedFuture(
                                                        new ZLinkOwnerLeaseFound(
                                                                new ZLinkLocationOwnerToken(
                                                                        ownerId,
                                                                        descriptor
                                                                                .leaseGeneration()),
                                                                expiresAt,
                                                                STORE_NOW));
                                            }
                                            default ->
                                                    throw new UnsupportedOperationException(
                                                            method.getName());
                                        });
        return new ZLinkStoreLocationResolvers(
                ZLinkRegisteredLocationStores.fromUnified(store), new ZLinkLocationOptions());
    }

    public static ZLinkStoreLocationResolvers resolver(ZLinkLocationRepository repository) {
        return new ZLinkStoreLocationResolvers(
                ZLinkRegisteredLocationStores.fromUnified(repository), new ZLinkLocationOptions());
    }

    public static ZLinkMeshNodeDescriptor descriptor(String rid, String endpoint, String ownerId) {
        return new ZLinkMeshNodeDescriptor(
                "mesh",
                RoutingId.from(rid),
                1,
                1,
                endpoint,
                Map.of(),
                9,
                List.of(),
                ZLinkMeshNodeObjectRole.SERVER,
                Optional.empty(),
                100,
                new ZLinkPlacementCapacity(
                        new ZLinkCapacityUsage(0, 0, 0),
                        new ZLinkCapacityUsage(0, 0, 0),
                        List.of()),
                new ZLinkActivationConcurrency(0, 8),
                Optional.empty(),
                ZLinkFrameworkRuntimeState.SERVING,
                "security-" + ownerId,
                ownerId,
                1,
                STORE_NOW);
    }

    private static ZLinkLocationRepository corruptLeaseRepository(String ownerId) {
        var provider =
                new ZLinkInMemoryProviderLocationStore(Clock.fixed(STORE_NOW, ZoneOffset.UTC));
        var valid = new ZLinkProviderLocationRepository(provider);
        ZLinkLocationOwnerToken token =
                ((ZLinkOwnerLeaseClaimed)
                                valid.claimOwnerLease(ownerId, Duration.ofMinutes(1))
                                        .toCompletableFuture()
                                        .join())
                        .token();
        if (token.leaseGeneration() != 1) {
            throw new IllegalStateException("unexpected fixture generation");
        }
        return new ZLinkProviderLocationRepository(new MissingExpiryStore(provider));
    }

    private static final class MissingExpiryStore implements ZLinkLocationStore {
        private final ZLinkLocationStore delegate;

        private MissingExpiryStore(ZLinkLocationStore delegate) {
            this.delegate = delegate;
        }

        @Override
        public CompletionStage<ZLinkStoreReadResult> read(
                systems.zlink.framework.locationprovider.ZLinkStoreKey key,
                ZLinkStoreCancellation cancellation) {
            return delegate.read(key, cancellation)
                    .thenApply(
                            read -> {
                                if (!(read instanceof ZLinkStoreReadFound found)) {
                                    return read;
                                }
                                ZLinkStoreValue value = found.value();
                                return new ZLinkStoreReadFound(
                                        new ZLinkStoreValue(
                                                value.bytes(),
                                                value.version(),
                                                null,
                                                value.storeNow()));
                            });
        }

        @Override
        public CompletionStage<ZLinkStoreWriteResult> write(
                ZLinkStoreWriteRequest request, ZLinkStoreCancellation cancellation) {
            return delegate.write(request, cancellation);
        }

        @Override
        public CompletionStage<ZLinkStoreScanResult> scan(
                ZLinkStoreScanRequest request, ZLinkStoreCancellation cancellation) {
            return delegate.scan(request, cancellation);
        }
    }
}
