package systems.zlink.framework.runtime.internal.locations;

import com.fasterxml.jackson.core.JsonProcessingException;
import com.fasterxml.jackson.databind.ObjectMapper;

import systems.zlink.framework.locationprovider.ZLinkLocationStore;
import systems.zlink.framework.locationprovider.ZLinkStoreCancellation;
import systems.zlink.framework.locationprovider.ZLinkStoreCondition;
import systems.zlink.framework.locationprovider.ZLinkStoreDelete;
import systems.zlink.framework.locationprovider.ZLinkStoreKey;
import systems.zlink.framework.locationprovider.ZLinkStoreMissingCondition;
import systems.zlink.framework.locationprovider.ZLinkStoreMutation;
import systems.zlink.framework.locationprovider.ZLinkStorePut;
import systems.zlink.framework.locationprovider.ZLinkStoreReadFound;
import systems.zlink.framework.locationprovider.ZLinkStoreReadMissing;
import systems.zlink.framework.locationprovider.ZLinkStoreScanCursor;
import systems.zlink.framework.locationprovider.ZLinkStoreScanExpired;
import systems.zlink.framework.locationprovider.ZLinkStoreScanItem;
import systems.zlink.framework.locationprovider.ZLinkStoreScanPageResult;
import systems.zlink.framework.locationprovider.ZLinkStoreScanRequest;
import systems.zlink.framework.locationprovider.ZLinkStoreVersion;
import systems.zlink.framework.locationprovider.ZLinkStoreVersionCondition;
import systems.zlink.framework.locationprovider.ZLinkStoreWriteRequest;
import systems.zlink.framework.runtime.locations.ZLinkAuthorityKeyCodec;

import java.io.IOException;
import java.time.Instant;
import java.util.List;
import java.util.Optional;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;

/** Builds the authority conditions and mutations that publish or release an Entry identity. */
final class ZLinkProviderEntrySpotClaims {
    private static final String DOMAIN = "entry-spot-id";
    private static final ObjectMapper JSON = new ObjectMapper();
    private final ZLinkLocationStore provider;

    ZLinkProviderEntrySpotClaims(ZLinkLocationStore provider) {
        this.provider = provider;
    }

    CompletionStage<Boolean> claim(
            ZLinkStoreKey descriptorKey,
            ZLinkMeshNodeDescriptor descriptor,
            List<ZLinkStoreCondition> conditions,
            List<ZLinkStoreMutation> mutations,
            ZLinkStoreCancellation cancellation) {
        String spotId = descriptor.entrySpotId().orElseThrow();
        ZLinkStoreKey authorityKey = spotAuthorityKey(spotId);
        Claim desired = Claim.from(descriptorKey, descriptor);
        return availableClaim(spotId, desired, cancellation)
                .thenApply(
                        available -> {
                            if (available.isEmpty()) return false;
                            Observation entry = available.orElseThrow();
                            conditions.add(new ZLinkStoreMissingCondition(authorityKey));
                            conditions.add(entry.condition());
                            if (!entry.live()) {
                                mutations.add(
                                        new ZLinkStorePut(entry.key(), desired.encode(), null));
                            }
                            return true;
                        });
    }

    CompletionStage<Boolean> conflicts(
            ZLinkStoreKey descriptorKey,
            ZLinkMeshNodeDescriptor descriptor,
            ZLinkStoreCancellation cancellation) {
        return availableClaim(
                        descriptor.entrySpotId().orElseThrow(),
                        Claim.from(descriptorKey, descriptor),
                        cancellation)
                .thenApply(Optional::isEmpty);
    }

    private CompletionStage<Optional<Observation>> availableClaim(
            String spotId, Claim desired, ZLinkStoreCancellation cancellation) {
        return provider.read(spotAuthorityKey(spotId), cancellation)
                .thenCompose(
                        read ->
                                read instanceof ZLinkStoreReadFound
                                        ? completed(Optional.empty())
                                        : observe(spotId, cancellation)
                                                .thenApply(
                                                        entry ->
                                                                entry.live()
                                                                                && !desired.equals(
                                                                                        entry
                                                                                                .activeClaim())
                                                                        ? Optional.empty()
                                                                        : Optional.of(entry)));
    }

    CompletionStage<Void> release(
            ZLinkStoreKey descriptorKey,
            ZLinkMeshNodeDescriptor descriptor,
            List<ZLinkStoreCondition> conditions,
            List<ZLinkStoreMutation> mutations,
            ZLinkStoreCancellation cancellation) {
        if (descriptor.entrySpotId().isEmpty()) return completed(null);
        ZLinkStoreKey key = key(descriptor.entrySpotId().orElseThrow());
        return provider.read(key, cancellation)
                .thenAccept(
                        read -> {
                            if (read instanceof ZLinkStoreReadFound found) {
                                conditions.add(
                                        new ZLinkStoreVersionCondition(
                                                key, found.value().version()));
                                if (Claim.from(descriptorKey, descriptor)
                                        .equals(Claim.decode(found.value().bytes()))) {
                                    mutations.add(new ZLinkStoreDelete(key));
                                }
                            } else {
                                conditions.add(new ZLinkStoreMissingCondition(key));
                            }
                        });
    }

    CompletionStage<Observation> observe(String spotId, ZLinkStoreCancellation cancellation) {
        ZLinkStoreKey key = key(spotId);
        return provider.read(key, cancellation)
                .thenCompose(
                        read -> {
                            if (read instanceof ZLinkStoreReadMissing missing) {
                                return completed(
                                        new Observation(key, null, null, missing.storeNow()));
                            }
                            var value = ((ZLinkStoreReadFound) read).value();
                            Claim claim = Claim.decode(value.bytes());
                            return provider.read(
                                            ZLinkOwnerLeaseRecordCodec.key(claim.ownerId()),
                                            cancellation)
                                    .thenApply(
                                            lease ->
                                                    new Observation(
                                                            key,
                                                            value.version(),
                                                            ZLinkProviderOwnerLeaseRepository
                                                                            .isLive(
                                                                                    lease,
                                                                                    claim.ownerId(),
                                                                                    claim
                                                                                            .ownerLeaseGeneration())
                                                                    ? claim
                                                                    : null,
                                                            value.storeNow()));
                        });
    }

    CompletionStage<Void> removeAllByOwner(
            ZLinkLocationOwnerToken owner, ZLinkStoreCancellation cancellation) {
        return removeAllByOwner(owner, cancellation, null, 0);
    }

    private CompletionStage<Void> removeAllByOwner(
            ZLinkLocationOwnerToken owner,
            ZLinkStoreCancellation cancellation,
            ZLinkStoreScanCursor cursor,
            int restarts) {
        return provider.scan(new ZLinkStoreScanRequest(DOMAIN + "\0", cursor, 1000), cancellation)
                .thenCompose(
                        result -> {
                            if (result instanceof ZLinkStoreScanExpired) {
                                if (restarts >= 8) {
                                    return CompletableFuture.failedFuture(
                                            new IllegalStateException(
                                                    "Entry Spot cleanup scan repeatedly expired"));
                                }
                                return removeAllByOwner(owner, cancellation, null, restarts + 1);
                            }
                            var page = ((ZLinkStoreScanPageResult) result).value();
                            CompletionStage<Void> cleanup = completed(null);
                            for (var item : page.items()) {
                                cleanup =
                                        cleanup.thenCompose(
                                                ignored ->
                                                        releaseOwnedClaim(
                                                                item, owner, cancellation));
                            }
                            return page.nextCursor() == null
                                    ? cleanup
                                    : cleanup.thenCompose(
                                            ignored ->
                                                    removeAllByOwner(
                                                            owner,
                                                            cancellation,
                                                            page.nextCursor(),
                                                            restarts));
                        });
    }

    private CompletionStage<Void> releaseOwnedClaim(
            ZLinkStoreScanItem item,
            ZLinkLocationOwnerToken owner,
            ZLinkStoreCancellation cancellation) {
        Claim claim = Claim.decode(item.value().bytes());
        if (!claim.ownerId().equals(owner.ownerId())
                || claim.ownerLeaseGeneration() != owner.leaseGeneration()) {
            return completed(null);
        }
        return provider.read(claim.descriptorKey(), cancellation)
                .thenCompose(
                        read -> {
                            if (!(read instanceof ZLinkStoreReadFound descriptor))
                                return completed(null);
                            var mesh =
                                    ZLinkProviderDescriptorRepository.decodeMeshNodeRecord(
                                            descriptor.value().bytes());
                            if (!claim.equals(Claim.from(claim.descriptorKey(), mesh))
                                    || mesh.entrySpotId()
                                            .map(ZLinkProviderEntrySpotClaims::key)
                                            .filter(item.key()::equals)
                                            .isEmpty()) {
                                return completed(null);
                            }
                            var request =
                                    new ZLinkStoreWriteRequest(
                                            List.of(
                                                    ZLinkOwnerLeaseRecordCodec.valueCondition(
                                                            owner.ownerId(),
                                                            owner.leaseGeneration()),
                                                    new ZLinkStoreVersionCondition(
                                                            claim.descriptorKey(),
                                                            descriptor.value().version()),
                                                    new ZLinkStoreVersionCondition(
                                                            item.key(), item.value().version())),
                                            List.of(new ZLinkStoreDelete(item.key())));
                            return provider.write(request, cancellation).thenApply(ignored -> null);
                        });
    }

    private static ZLinkStoreKey key(String spotId) {
        return ZLinkOpaqueRecordKey.of(DOMAIN, spotId);
    }

    private static ZLinkStoreKey spotAuthorityKey(String spotId) {
        return ZLinkProviderAuthorityRepository.authorityKey(ZLinkAuthorityKeyCodec.spot(spotId));
    }

    private static <T> CompletionStage<T> completed(T value) {
        return CompletableFuture.completedFuture(value);
    }

    record Observation(
            ZLinkStoreKey key, ZLinkStoreVersion version, Claim activeClaim, Instant storeNow) {
        boolean live() {
            return activeClaim != null;
        }

        ZLinkStoreCondition condition() {
            return version == null
                    ? new ZLinkStoreMissingCondition(key)
                    : new ZLinkStoreVersionCondition(key, version);
        }

        void fenceReserve(
                List<ZLinkStoreCondition> conditions, List<ZLinkStoreMutation> mutations) {
            conditions.add(condition());
            if (version != null) mutations.add(new ZLinkStoreDelete(key));
        }
    }

    private record Claim(
            ZLinkStoreKey descriptorKey,
            long lifecycleGeneration,
            String ownerId,
            long ownerLeaseGeneration) {
        static Claim from(ZLinkStoreKey descriptorKey, ZLinkMeshNodeDescriptor descriptor) {
            return new Claim(
                    descriptorKey,
                    descriptor.lifecycleGeneration(),
                    descriptor.ownerId(),
                    descriptor.leaseGeneration());
        }

        byte[] encode() {
            var root = JSON.createObjectNode();
            root.put("descriptorKey", descriptorKey.value());
            root.put("descriptorLifecycleGeneration", Long.toUnsignedString(lifecycleGeneration));
            root.put("ownerId", ownerId);
            root.put("ownerLeaseGeneration", Long.toUnsignedString(ownerLeaseGeneration));
            try {
                return JSON.writeValueAsBytes(root);
            } catch (JsonProcessingException failure) {
                throw new IllegalStateException("Failed to encode Entry Spot claim", failure);
            }
        }

        static Claim decode(byte[] bytes) {
            try {
                var root = JSON.readTree(bytes);
                return new Claim(
                        new ZLinkStoreKey(root.required("descriptorKey").asText()),
                        Long.parseUnsignedLong(
                                root.required("descriptorLifecycleGeneration").asText()),
                        root.required("ownerId").asText(),
                        Long.parseUnsignedLong(root.required("ownerLeaseGeneration").asText()));
            } catch (IOException | IllegalArgumentException failure) {
                throw new IllegalStateException("Invalid Entry Spot claim record", failure);
            }
        }
    }
}
