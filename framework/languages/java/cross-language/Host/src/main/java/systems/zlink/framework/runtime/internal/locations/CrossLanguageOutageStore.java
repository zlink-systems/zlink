package systems.zlink.framework.runtime.internal.locations;

import java.lang.reflect.Method;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.locationprovider.*;
import systems.zlink.framework.locationprovider.ZLinkStoreCancellation;
import systems.zlink.framework.runtime.locations.ZLinkAuthorityKeyCodec;

/** Test-only fault injection at the opaque Store boundary; Redis owns all data. */
public final class CrossLanguageOutageStore implements ZLinkLocationStore {
    private final ZLinkLocationStore redis;
    private final ZLinkStoreKey descriptorKey;
    private final ZLinkStoreKey actorKey;
    private final String authorityIdentity;
    private final boolean target;
    private volatile boolean unavailable;
    private volatile boolean recovering;
    private volatile boolean commitReleased;
    private volatile long descriptorLease;
    private volatile long preserveLease;
    private final CompletableFuture<Void> failedOperation = new CompletableFuture<>();
    private final CompletableFuture<Void> descriptorSeen = new CompletableFuture<>();
    private final CompletableFuture<Void> casSeen = new CompletableFuture<>();
    private final CompletableFuture<Void> descriptorRelease = new CompletableFuture<>();
    private final CompletableFuture<Void> casRelease = new CompletableFuture<>();
    private final CompletableFuture<Void> commitSeen = new CompletableFuture<>();
    private final CompletableFuture<Void> commitRelease = new CompletableFuture<>();

    public CrossLanguageOutageStore(
            ZLinkLocationStore redis, String meshName, String nodeRid, String actorId, boolean target) {
        this.redis = redis;
        this.target = target;
        descriptorKey = ZLinkProviderDescriptorRepository.meshKey(meshName, RoutingId.from(nodeRid));
        authorityIdentity = ZLinkAuthorityKeyCodec.actor(actorId);
        actorKey = ZLinkProviderAuthorityRepository.authorityKey(authorityIdentity);
    }

    public ZLinkProviderLocationRepository observer() {
        return new ZLinkProviderLocationRepository(redis);
    }

    public void outage() {
        unavailable = true;
    }

    public void recover() {
        recovering = true;
        unavailable = false;
    }

    public CompletionStage<Void> failedOperation() { return failedOperation; }
    public CompletionStage<Void> commitSeen() { return commitSeen; }
    public long descriptorLease() { return descriptorLease; }
    public long preserveLease() { return preserveLease; }
    public CompletionStage<Void> bothConfirmationsSeen() {
        return CompletableFuture.allOf(descriptorSeen, casSeen);
    }

    public CompletionStage<Void> releaseDescriptor() {
        return descriptorSeen.thenRun(() -> descriptorRelease.complete(null));
    }

    public CompletionStage<Void> releaseCas() {
        return casSeen.thenRun(() -> casRelease.complete(null));
    }

    public void releaseCommit() {
        commitReleased = true;
        commitRelease.complete(null);
    }

    private <T> CompletionStage<T> failure() {
        failedOperation.complete(null);
        return CompletableFuture.failedFuture(new IllegalStateException("injected Location Store outage"));
    }

    @Override
    public CompletionStage<ZLinkStoreReadResult> read(ZLinkStoreKey key, ZLinkStoreCancellation cancellation) {
        if (unavailable) return failure();
        return redis.read(key, cancellation);
    }

    @Override
    public CompletionStage<ZLinkStoreScanResult> scan(
            ZLinkStoreScanRequest request, ZLinkStoreCancellation cancellation) {
        return unavailable ? failure() : redis.scan(request, cancellation);
    }

    @Override
    public CompletionStage<ZLinkStoreWriteResult> write(
            ZLinkStoreWriteRequest request, ZLinkStoreCancellation cancellation) {
        if (unavailable) return failure();
        if (target && !commitReleased && sourceOwnerId != null) {
            return projectsNewOwner(request).thenCompose(moves -> {
                if (!moves) return redis.write(request, cancellation);
                commitSeen.complete(null);
                return commitRelease.thenCompose(ignored -> redis.write(request, cancellation));
            });
        }
        boolean descriptor = recovering && request.mutations().stream().anyMatch(
            mutation -> mutation instanceof ZLinkStorePut put && descriptorKey.equals(put.key()));
        boolean cas = recovering && request.mutations().stream().anyMatch(
            mutation -> mutation instanceof ZLinkStorePut put && actorKey.equals(put.key()));
        return redis.write(request, cancellation).thenCompose(result -> {
            if (!(result instanceof ZLinkStoreWriteApplied)) return CompletableFuture.completedFuture(result);
            if (descriptor) {
                descriptorLease = request.mutations().stream()
                    .filter(mutation -> mutation instanceof ZLinkStorePut put && descriptorKey.equals(put.key()))
                    .map(mutation -> ZLinkProviderDescriptorRepository.decodeMeshNodeRecord(((ZLinkStorePut) mutation).bytes()))
                    .findFirst().orElseThrow().leaseGeneration();
                descriptorSeen.complete(null);
                return descriptorRelease.thenApply(ignored -> result);
            }
            if (cas) {
                preserveLease = request.mutations().stream()
                    .filter(mutation -> mutation instanceof ZLinkStorePut put && actorKey.equals(put.key()))
                    .map(mutation -> snapshot(((ZLinkStorePut) mutation).bytes()))
                    .findFirst().orElseThrow().ownerLeaseGeneration();
                casSeen.complete(null);
                return casRelease.thenApply(ignored -> result);
            }
            return CompletableFuture.completedFuture(result);
        });
    }

    private CompletionStage<Boolean> projectsNewOwner(ZLinkStoreWriteRequest request) {
        ZLinkLocationStore proposed = new ZLinkLocationStore() {
            @Override
            public CompletionStage<ZLinkStoreReadResult> read(ZLinkStoreKey key, ZLinkStoreCancellation cancellation) {
                return redis.read(key, cancellation).thenApply(current -> {
                    var now = current instanceof ZLinkStoreReadFound found
                        ? found.value().storeNow() : ((ZLinkStoreReadMissing) current).storeNow();
                    for (ZLinkStoreMutation mutation : request.mutations()) {
                        if (mutation instanceof ZLinkStorePut put && put.key().equals(key)) {
                            var expires = put.retention() == null ? null : now.plus(put.retention());
                            return new ZLinkStoreReadFound(new ZLinkStoreValue(put.bytes(),
                                new ZLinkStoreVersion("proposed-test-write"), expires, now));
                        }
                        if (mutation instanceof ZLinkStoreDelete delete && delete.key().equals(key)) {
                            return new ZLinkStoreReadMissing(now);
                        }
                    }
                    return current;
                });
            }

            @Override
            public CompletionStage<ZLinkStoreWriteResult> write(
                    ZLinkStoreWriteRequest ignored, ZLinkStoreCancellation cancellation) {
                return CompletableFuture.failedFuture(new IllegalStateException("projection observation is read-only"));
            }

            @Override
            public CompletionStage<ZLinkStoreScanResult> scan(
                    ZLinkStoreScanRequest scan, ZLinkStoreCancellation cancellation) {
                return redis.scan(scan, cancellation);
            }
        };
        return new ZLinkProviderLocationRepository(proposed).read(authorityIdentity, () -> false)
            .thenApply(projected -> {
                if (!(projected instanceof ZLinkAuthoritySnapshot authority)) return false;
                return !authority.ownerId().equals(sourceOwnerId);
            });
    }

    private volatile String sourceOwnerId;

    public void sourceOwner(String ownerId) { sourceOwnerId = ownerId; }

    private static ZLinkAuthoritySnapshot snapshot(byte[] bytes) {
        try {
            Method method = ZLinkProviderAuthorityRepository.class.getDeclaredMethod("snapshot", ZLinkStoreValue.class);
            method.setAccessible(true);
            return (ZLinkAuthoritySnapshot) method.invoke(null, new ZLinkStoreValue(
                bytes, new ZLinkStoreVersion("test-observation"), null, java.time.Instant.EPOCH));
        } catch (ReflectiveOperationException error) {
            throw new IllegalStateException("existing authority codec is unavailable", error);
        }
    }

}
