package systems.zlink.framework.runtime.locations;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.locations.ZLinkPageRequest;
import systems.zlink.framework.runtime.internal.backend.ZLinkInternalMeshNode;
import systems.zlink.framework.runtime.internal.locations.ZLinkLocationRepository;
import systems.zlink.framework.runtime.internal.locations.ZLinkMeshNodeDescriptor;
import systems.zlink.framework.runtime.internal.locations.ZLinkRelocationReplyRow;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceRelocationWireCodec;

import java.time.Duration;
import java.util.Arrays;
import java.util.HexFormat;
import java.util.List;
import java.util.Map;
import java.util.Objects;
import java.util.Optional;
import java.util.UUID;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.ConcurrentHashMap;
import java.util.function.LongSupplier;

/** Resolves one authenticated peer lifecycle to its durable owner fence. */
public final class ZLinkMeshPeerAuthorityResolver
        implements ZLinkInternalMeshNode.PeerAuthorityResolver {
    private static final int PAGE_SIZE = 128;

    private final ZLinkLocationRuntime localRuntime;
    private final ZLinkLocationRepository store;
    private final ZLinkOwnerLeaseTracker leases;
    private final long cacheNanos;
    private final LongSupplier nanoTime;
    private final Map<Key, CachedDescriptor> descriptors = new ConcurrentHashMap<>();

    public ZLinkMeshPeerAuthorityResolver(ZLinkLocationRepository store, Duration pollingInterval) {
        this(store, pollingInterval, System::nanoTime);
    }

    public ZLinkMeshPeerAuthorityResolver(
            ZLinkLocationRepository store,
            Duration pollingInterval,
            ZLinkLocationRuntime localRuntime) {
        this(store, pollingInterval, System::nanoTime, localRuntime);
    }

    ZLinkMeshPeerAuthorityResolver(
            ZLinkLocationRepository store, Duration pollingInterval, LongSupplier nanoTime) {
        this(store, pollingInterval, nanoTime, null);
    }

    ZLinkMeshPeerAuthorityResolver(
            ZLinkLocationRepository store,
            Duration pollingInterval,
            LongSupplier nanoTime,
            ZLinkLocationRuntime localRuntime) {
        this.localRuntime = localRuntime;
        this.store = Objects.requireNonNull(store, "store");
        if (pollingInterval == null || pollingInterval.isZero() || pollingInterval.isNegative()) {
            throw new IllegalArgumentException("pollingInterval must be positive");
        }
        this.leases = new ZLinkOwnerLeaseTracker(store, pollingInterval, nanoTime);
        this.cacheNanos = pollingInterval.toNanos();
        this.nanoTime = Objects.requireNonNull(nanoTime, "nanoTime");
    }

    @Override
    public CompletionStage<Boolean> confirmsRelocationReply(
            ZLinkInternalMeshNode.PeerAuthorityFence requester,
            ZLinkServiceRelocationWireCodec.ReplyRelay relay,
            List<byte[]> payload) {
        var source =
                new ZLinkRelocationReplyRow.Source(
                        requester.ownerId(),
                        Long.toUnsignedString(requester.ownerLeaseGeneration()),
                        requester.sourceNodeRid().toHex(),
                        Long.toUnsignedString(requester.sourceNodeGeneration()));
        var id = new UUID(relay.relocation().high(), relay.relocation().low());
        String operation =
                HexFormat.of().toHexDigits(relay.operation().high())
                        + HexFormat.of().toHexDigits(relay.operation().low());
        return store.readRelocationReply(id, source, operation, () -> false)
                .thenApply(
                        found ->
                                found.filter(row -> replyMatches(row, relay, payload)).isPresent());
    }

    private static boolean replyMatches(
            ZLinkRelocationReplyRow row,
            ZLinkServiceRelocationWireCodec.ReplyRelay relay,
            List<byte[]> payload) {
        if (row.state().equals(ZLinkRelocationReplyRow.ACCEPTED)
                || !row.writer().ownerId().equals(relay.coordinator().ownerId())
                || row.writer().leaseGeneration() != relay.coordinator().leaseGeneration()
                || !row.replyRouteId().equals(Long.toUnsignedString(relay.replyRouteId()))) {
            return false;
        }
        var stored = new ZLinkServiceRelocationWireCodec().decodeStoredReply(row.reply());
        return stored.relay().equals(relay)
                && (stored.payload() == null
                        ? payload.isEmpty()
                        : payload.size() == 1
                                && Arrays.equals(payload.getFirst(), stored.payload().payload()));
    }

    @Override
    public boolean acceptsSessionOwnerLease(
            ZLinkInternalMeshNode.PeerAuthorityFence current, String ownerId, long generation) {
        return localRuntime == null
                ? ZLinkInternalMeshNode.PeerAuthorityResolver.super.acceptsSessionOwnerLease(
                        current, ownerId, generation)
                : localRuntime.acceptsSessionOwnerLease(
                        ownerId, current.ownerLeaseGeneration(), generation);
    }

    @Override
    public CompletionStage<Optional<ZLinkInternalMeshNode.PeerAuthorityFence>> resolve(
            String meshName, RoutingId sourceNodeRid, long sourceNodeGeneration) {
        Objects.requireNonNull(meshName, "meshName");
        Objects.requireNonNull(sourceNodeRid, "sourceNodeRid");
        if (sourceNodeGeneration == 0) {
            return CompletableFuture.completedFuture(Optional.empty());
        }
        Key key = new Key(meshName, sourceNodeRid, sourceNodeGeneration);
        CachedDescriptor cached = descriptors.get(key);
        long now = nanoTime.getAsLong();
        if (cached != null && now < cached.expiresAtNanos()) {
            return validateLease(key, cached.descriptor());
        }
        descriptors.remove(key, cached);
        return find(key, null)
                .thenCompose(
                        found -> {
                            if (found.isEmpty()) {
                                return CompletableFuture.completedFuture(Optional.empty());
                            }
                            ZLinkMeshNodeDescriptor descriptor = found.orElseThrow();
                            descriptors.put(
                                    key,
                                    new CachedDescriptor(
                                            descriptor,
                                            Math.addExact(nanoTime.getAsLong(), cacheNanos)));
                            return validateLease(key, descriptor);
                        });
    }

    private CompletionStage<Optional<ZLinkMeshNodeDescriptor>> find(Key key, String continuation) {
        return store.listMeshNodes(key.meshName(), new ZLinkPageRequest(PAGE_SIZE, continuation))
                .thenCompose(
                        page -> {
                            Optional<ZLinkMeshNodeDescriptor> exact =
                                    page.items().stream()
                                            .filter(
                                                    candidate ->
                                                            candidate
                                                                            .rid()
                                                                            .equals(
                                                                                    key
                                                                                            .sourceNodeRid())
                                                                    && candidate
                                                                                    .lifecycleGeneration()
                                                                            == key
                                                                                    .sourceNodeGeneration())
                                            .findFirst();
                            if (exact.isPresent() || page.continuationToken() == null) {
                                return CompletableFuture.completedFuture(exact);
                            }
                            return find(key, page.continuationToken());
                        });
    }

    private CompletionStage<Optional<ZLinkInternalMeshNode.PeerAuthorityFence>> validateLease(
            Key key, ZLinkMeshNodeDescriptor descriptor) {
        return leases.remainingAdmissionLifetime(descriptor.ownerId(), descriptor.leaseGeneration())
                .thenApply(
                        remaining -> {
                            if (remaining == null) {
                                descriptors.remove(key);
                                return Optional.empty();
                            }
                            return Optional.of(
                                    new ZLinkInternalMeshNode.PeerAuthorityFence(
                                            key.sourceNodeRid(),
                                            key.sourceNodeGeneration(),
                                            descriptor.ownerId(),
                                            descriptor.leaseGeneration()));
                        });
    }

    private record Key(String meshName, RoutingId sourceNodeRid, long sourceNodeGeneration) {}

    private record CachedDescriptor(ZLinkMeshNodeDescriptor descriptor, long expiresAtNanos) {}
}
