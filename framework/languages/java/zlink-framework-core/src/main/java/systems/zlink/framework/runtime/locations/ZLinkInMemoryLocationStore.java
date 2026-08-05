package systems.zlink.framework.runtime.locations;

import java.time.Clock;
import java.time.Duration;
import java.time.Instant;
import java.util.ArrayList;
import java.util.Comparator;
import java.util.HashMap;
import java.util.List;
import java.util.Map;
import java.util.Objects;
import java.util.OptionalLong;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.function.Function;
import java.util.function.Predicate;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.errors.ZLinkConfigurationException;
import systems.zlink.framework.locations.ZLinkCapacityUsage;
import systems.zlink.framework.runtime.internal.locations.ZLinkClientServerServerDescriptor;
import systems.zlink.framework.runtime.internal.locations.ZLinkClientServerServerDescriptorKey;
import systems.zlink.framework.runtime.internal.locations.ZLinkFanoutPublisherDescriptor;
import systems.zlink.framework.runtime.internal.locations.ZLinkFanoutPublisherDescriptorKey;
import systems.zlink.framework.runtime.internal.locations.ZLinkLocationOwnerToken;
import systems.zlink.framework.locations.ZLinkLocationPage;
import systems.zlink.framework.runtime.internal.locations.ZLinkLocationRepository;
import systems.zlink.framework.runtime.internal.locations.ZLinkLocationWriteIntent;
import systems.zlink.framework.runtime.internal.locations.ZLinkLocationWriteResult;
import systems.zlink.framework.runtime.internal.locations.ZLinkLocationWriteStatus;
import systems.zlink.framework.runtime.internal.locations.ZLinkMeshNodeDescriptor;
import systems.zlink.framework.runtime.internal.locations.ZLinkMeshNodeDescriptorKey;
import systems.zlink.framework.runtime.internal.locations.ZLinkOwnerLease;
import systems.zlink.framework.runtime.internal.locations.ZLinkOwnerLeaseRenewal;
import systems.zlink.framework.runtime.internal.locations.ZLinkOwnerLeaseSnapshot;
import systems.zlink.framework.locations.ZLinkPageRequest;
import systems.zlink.framework.locations.ZLinkPlacementCapacity;
import systems.zlink.framework.locations.ZLinkPlacementObjectKind;
import systems.zlink.framework.locations.ZLinkSpotTypeCapacity;

public final class ZLinkInMemoryLocationStore
    implements ZLinkLocationRepository,
        systems.zlink.framework.locationprovider.ZLinkLocationStore {

    private final Object gate = new Object();
    private final Clock clock;
    private final ZLinkInMemoryAuthorityStore authority;
    private final ZLinkInMemoryProviderLocationStore opaque;
    private final Map<String, LeaseRow> leases = new HashMap<>();
    private long ownerLeaseGeneration;
    private final RowTable<ZLinkMeshNodeDescriptor> meshNodes =
        new RowTable<>();
    private final Map<String, Long> meshNodeStamps = new HashMap<>();
    private final Map<String, EntrySpotClaim> entrySpotClaims =
        new HashMap<>();
    private final RowTable<ZLinkFanoutPublisherDescriptor> fanoutPublishers =
        new RowTable<>();
    private final RowTable<ZLinkClientServerServerDescriptor> clientServers =
        new RowTable<>();

    public ZLinkInMemoryLocationStore() {
        this(Clock.systemUTC());
    }

    public ZLinkInMemoryLocationStore(Clock clock) {
        this.clock = Objects.requireNonNull(clock, "clock");
        this.opaque = new ZLinkInMemoryProviderLocationStore(clock);
        this.authority = new ZLinkInMemoryAuthorityStore(
            gate,
            clock,
            this::isExactOwnerLeaseLive,
            this::findMeshNodeDescriptor,
            this::isSpotIdentityClaimed);
    }

    @Override
    public CompletionStage<systems.zlink.framework.locationprovider
        .ZLinkStoreReadResult> read(
            systems.zlink.framework.locationprovider.ZLinkStoreKey key,
            systems.zlink.framework.locationprovider
                .ZLinkStoreCancellation cancellation) {
        return opaque.read(key, cancellation);
    }

    @Override
    public CompletionStage<systems.zlink.framework.locationprovider
        .ZLinkStoreWriteResult> write(
            systems.zlink.framework.locationprovider
                .ZLinkStoreWriteRequest request,
            systems.zlink.framework.locationprovider
                .ZLinkStoreCancellation cancellation) {
        return opaque.write(request, cancellation);
    }

    @Override
    public CompletionStage<systems.zlink.framework.locationprovider
        .ZLinkStoreScanResult> scan(
            systems.zlink.framework.locationprovider
                .ZLinkStoreScanRequest request,
            systems.zlink.framework.locationprovider
                .ZLinkStoreCancellation cancellation) {
        return opaque.scan(request, cancellation);
    }

    @Override
    public CompletionStage<systems.zlink.framework.runtime.internal.locations.ZLinkAuthorityReadResult> read(
        String key,
        systems.zlink.framework.runtime.internal.locations.ZLinkStoreCancellation cancellation) {
        return authority.read(key, cancellation);
    }

    @Override
    public CompletionStage<systems.zlink.framework.runtime.internal.locations.ZLinkAuthorityWriteResult>
        compareExchange(
            String key,
            systems.zlink.framework.runtime.internal.locations.ZLinkAuthorityExpectation expectation,
            systems.zlink.framework.runtime.internal.locations.ZLinkAuthorityMutation mutation,
            systems.zlink.framework.runtime.internal.locations.ZLinkStoreCancellation cancellation) {
        return authority.compareExchange(key, expectation, mutation, cancellation);
    }

    @Override
    public CompletionStage<systems.zlink.framework.runtime.internal.locations.ZLinkAuthorityScanResult> list(
        String prefix,
        java.util.Optional<systems.zlink.framework.runtime.internal.locations.ZLinkAuthorityScanCursor> cursor,
        int limit,
        systems.zlink.framework.runtime.internal.locations.ZLinkStoreCancellation cancellation) {
        return authority.list(prefix, cursor, limit, cancellation);
    }

    @Override
    public CompletionStage<systems.zlink.framework.runtime.internal.locations.ZLinkObjectReserveResult> reserve(
        systems.zlink.framework.runtime.internal.locations.ZLinkObjectReservationRequest request,
        systems.zlink.framework.runtime.internal.locations.ZLinkStoreCancellation cancellation) {
        return authority.reserve(request, cancellation);
    }

    @Override
    public CompletionStage<systems.zlink.framework.runtime.internal.locations.ZLinkObjectCommitResult> commit(
        systems.zlink.framework.runtime.internal.locations.ZLinkObjectReservation reservation,
        byte[] readyPayload,
        systems.zlink.framework.runtime.internal.locations.ZLinkStoreCancellation cancellation) {
        return authority.commit(reservation, readyPayload, cancellation);
    }

    @Override
    public CompletionStage<systems.zlink.framework.runtime.internal.locations.ZLinkObjectCommitResult> commit(
        systems.zlink.framework.runtime.internal.locations.ZLinkObjectReservation reservation,
        byte[] readyPayload,
        systems.zlink.framework.runtime.internal.locations.ZLinkCreationOperationTerminal terminal,
        systems.zlink.framework.runtime.internal.locations.ZLinkStoreCancellation cancellation) {
        return authority.commit(
            reservation,
            readyPayload,
            terminal,
            cancellation);
    }

    @Override
    public CompletionStage<systems.zlink.framework.runtime.internal.locations.ZLinkObjectRejectResult> reject(
        systems.zlink.framework.runtime.internal.locations.ZLinkObjectReservation reservation,
        systems.zlink.framework.runtime.internal.locations.ZLinkCreationOperationTerminal terminal,
        systems.zlink.framework.runtime.internal.locations.ZLinkStoreCancellation cancellation) {
        return authority.reject(reservation, terminal, cancellation);
    }

    @Override
    public CompletionStage<systems.zlink.framework.runtime.internal.locations.ZLinkObjectAbortResult> abort(
        systems.zlink.framework.runtime.internal.locations.ZLinkObjectReservation reservation,
        systems.zlink.framework.runtime.internal.locations.ZLinkStoreCancellation cancellation) {
        return authority.abort(reservation, cancellation);
    }

    @Override
    public CompletionStage<systems.zlink.framework.runtime.internal.locations.ZLinkObjectAbortResult> abort(
        systems.zlink.framework.runtime.internal.locations.ZLinkObjectReservation reservation,
        systems.zlink.framework.runtime.internal.locations.ZLinkCreationOperationTerminal terminal,
        systems.zlink.framework.runtime.internal.locations.ZLinkStoreCancellation cancellation) {
        return authority.abort(reservation, terminal, cancellation);
    }

    @Override
    public CompletionStage<systems.zlink.framework.runtime.internal.locations.ZLinkCreationTerminalReadResult>
        readCreationTerminal(
            systems.zlink.framework.runtime.internal.locations.ZLinkCreationOperationIdentity operation,
            systems.zlink.framework.runtime.internal.locations.ZLinkStoreCancellation cancellation) {
        return authority.readCreationTerminal(operation, cancellation);
    }

    @Override
    public CompletionStage<systems.zlink.framework.runtime.internal.locations.ZLinkRelocationCapacityReserveResult>
        reserveRelocationCapacity(
            systems.zlink.framework.runtime.internal.locations.ZLinkRelocationCapacityReservationRequest request,
            systems.zlink.framework.runtime.internal.locations.ZLinkStoreCancellation cancellation) {
        return authority.reserveRelocationCapacity(request, cancellation);
    }

    @Override
    public CompletionStage<systems.zlink.framework.runtime.internal.locations.ZLinkRelocationCapacityAbortResult>
        abortRelocationCapacity(
            systems.zlink.framework.runtime.internal.locations.ZLinkRelocationCapacityFence fence,
            systems.zlink.framework.runtime.internal.locations.ZLinkStoreCancellation cancellation) {
        return authority.abortRelocationCapacity(fence, cancellation);
    }

    @Override
    public CompletionStage<systems.zlink.framework.runtime.internal.locations.ZLinkAggregatePrepareResult>
        prepareAggregate(
            systems.zlink.framework.runtime.internal.locations.ZLinkAggregatePrepareRequest request,
            systems.zlink.framework.runtime.internal.locations.ZLinkStoreCancellation cancellation) {
        return authority.prepareAggregate(request, cancellation);
    }

    @Override
    public CompletionStage<systems.zlink.framework.runtime.internal.locations.ZLinkAggregateCommitResult>
        commitAggregate(
            systems.zlink.framework.runtime.internal.locations.ZLinkAggregateFence fence,
            systems.zlink.framework.runtime.internal.locations.ZLinkStoreCancellation cancellation) {
        return authority.commitAggregate(fence, cancellation);
    }

    @Override
    public CompletionStage<systems.zlink.framework.runtime.internal.locations.ZLinkAggregateAbortResult>
        abortAggregate(
            systems.zlink.framework.runtime.internal.locations.ZLinkAggregateFence fence,
            systems.zlink.framework.runtime.internal.locations.ZLinkStoreCancellation cancellation) {
        return authority.abortAggregate(fence, cancellation);
    }

    @Override
    public CompletionStage<java.util.Optional<systems.zlink.framework.runtime.internal.locations.ZLinkAggregateProgressSnapshot>>
        readAggregateProgress(
            systems.zlink.framework.runtime.internal.locations.ZLinkAggregateFence fence,
            systems.zlink.framework.runtime.internal.locations.ZLinkStoreCancellation cancellation) {
        return authority.readAggregateProgress(fence, cancellation);
    }

    @Override
    public CompletionStage<systems.zlink.framework.runtime.internal.locations.ZLinkAggregateProgressWriteResult>
        compareExchangeAggregateProgress(
            systems.zlink.framework.runtime.internal.locations.ZLinkAggregateFence fence,
            String expectedStoreVersion,
            systems.zlink.framework.runtime.internal.locations.ZLinkAggregateProgress progress,
            systems.zlink.framework.runtime.internal.locations.ZLinkStoreCancellation cancellation) {
        return authority.compareExchangeAggregateProgress(
            fence, expectedStoreVersion, progress, cancellation);
    }

    @Override
    public CompletionStage<java.util.List<systems.zlink.framework.runtime.internal.locations.ZLinkAggregateProgressSnapshot>>
        listAggregateProgress(
            systems.zlink.framework.runtime.internal.locations.ZLinkStoreCancellation cancellation) {
        return authority.listAggregateProgress(cancellation);
    }

    @Override
    public CompletionStage<Boolean> removeAggregateProgress(
        systems.zlink.framework.runtime.internal.locations.ZLinkAggregateFence fence,
        String expectedStoreVersion,
        systems.zlink.framework.runtime.internal.locations.ZLinkStoreCancellation cancellation) {
        return authority.removeAggregateProgress(
            fence, expectedStoreVersion, cancellation);
    }

    @Override
    public CompletionStage<ZLinkLocationWriteResult> updateMeshNode(
        ZLinkMeshNodeDescriptor descriptor,
        ZLinkLocationWriteIntent intent) {
        Objects.requireNonNull(descriptor, "descriptor");
        Objects.requireNonNull(intent, "intent");
        synchronized (gate) {
            ZLinkLocationOwnerToken owner =
                new ZLinkLocationOwnerToken(
                    descriptor.ownerId(),
                    descriptor.leaseGeneration());
            if (!isExactOwnerLeaseLive(owner)) {
                return completed(
                    ZLinkLocationWriteResult.ignoredStale());
            }
            String key = meshNodeKey(
                new ZLinkMeshNodeDescriptorKey(
                    descriptor.meshName(),
                    descriptor.rid()));
            ZLinkMeshNodeDescriptor current =
                meshNodes.rows.get(key);
            String entryAuthorityKey = descriptor.entrySpotId()
                .map(ZLinkAuthorityKeyCodec::spot)
                .orElse(null);
            EntrySpotClaim entryClaim = entryAuthorityKey == null
                ? null
                : entrySpotClaims.get(entryAuthorityKey);
            if (entryClaim != null
                && !isExactOwnerLeaseLive(entryClaim.owner())) {
                entrySpotClaims.remove(entryAuthorityKey);
                entryClaim = null;
            }
            if ((intent == ZLinkLocationWriteIntent.NEW_CLAIM
                    || intent == ZLinkLocationWriteIntent.TAKEOVER)
                && ((entryClaim != null
                        && !entryClaim.matches(key, descriptor))
                    || (entryAuthorityKey != null
                        && authority.containsAuthority(
                            entryAuthorityKey)))) {
                return completed(
                    ZLinkLocationWriteResult.rejectedConflict());
            }
            if (intent == ZLinkLocationWriteIntent.NEW_CLAIM
                && current != null
                && isExactOwnerLeaseLive(
                    new ZLinkLocationOwnerToken(
                        current.ownerId(),
                        current.leaseGeneration()))) {
                return completed(
                    ZLinkLocationWriteResult.rejectedConflict());
            }
            if (intent == ZLinkLocationWriteIntent.TAKEOVER
                && current != null
                && isExactOwnerLeaseLive(
                    new ZLinkLocationOwnerToken(
                        current.ownerId(),
                        current.leaseGeneration()))) {
                return completed(
                    ZLinkLocationWriteResult.rejectedConflict());
            }
            if (intent == ZLinkLocationWriteIntent.RENEW
                && current != null
                && descriptor.descriptorRevision()
                    == current.descriptorRevision()) {
                if (hasSameDescriptorFields(current, descriptor)) {
                    return completed(ZLinkLocationWriteResult.stored(
                        current.lifecycleGeneration(),
                        current.updatedAt()));
                }
                throw new IllegalArgumentException(
                    "same descriptor revision has different bytes");
            }
            if (intent == ZLinkLocationWriteIntent.RENEW
                && (current == null
                    || !current.ownerId().equals(
                        descriptor.ownerId())
                    || current.leaseGeneration()
                        != descriptor.leaseGeneration()
                    || current.lifecycleGeneration()
                        != descriptor.lifecycleGeneration()
                    || !hasSameImmutableDescriptorFields(
                        current,
                        descriptor)
                    || descriptor.descriptorRevision()
                        <= current.descriptorRevision())) {
                return completed(
                    ZLinkLocationWriteResult.ignoredStale());
            }
            Instant now = clock.instant();
            meshNodes.rows.put(
                key,
                copyDescriptor(descriptor, now));
            bumpMeshNodeStamp(descriptor.meshName());
            if (entryAuthorityKey != null) {
                entrySpotClaims.put(
                    entryAuthorityKey,
                    new EntrySpotClaim(
                        key,
                        descriptor.lifecycleGeneration(),
                        owner));
            }
            return completed(ZLinkLocationWriteResult.stored(
                descriptor.lifecycleGeneration(),
                now));
        }
    }

    @Override
    public CompletionStage<ZLinkLocationWriteStatus> removeMeshNode(
        ZLinkMeshNodeDescriptorKey key,
        ZLinkLocationOwnerToken owner) {
        synchronized (gate) {
            ZLinkMeshNodeDescriptor current =
                meshNodes.rows.get(meshNodeKey(key));
            if (current == null
                || !current.ownerId().equals(owner.ownerId())
                || current.leaseGeneration()
                    != owner.leaseGeneration()) {
                return completed(
                    ZLinkLocationWriteStatus.IGNORED_STALE);
            }
            current.entrySpotId().ifPresent(spotId -> {
                String authorityKey = ZLinkAuthorityKeyCodec.spot(spotId);
                EntrySpotClaim claim = entrySpotClaims.get(authorityKey);
                if (claim != null
                    && claim.matches(meshNodeKey(key), current)) {
                    entrySpotClaims.remove(authorityKey);
                }
            });
            meshNodes.rows.remove(meshNodeKey(key));
            bumpMeshNodeStamp(key.meshName());
            return completed(ZLinkLocationWriteStatus.STORED);
        }
    }

    @Override
    public CompletionStage<ZLinkLocationPage<ZLinkMeshNodeDescriptor>>
        listMeshNodes(String meshName, ZLinkPageRequest page) {
        ZLinkLocationPage<ZLinkMeshNodeDescriptor> stored;
        synchronized (gate) {
            stored = page(
                meshNodes,
                descriptor -> descriptor.meshName().equals(meshName),
                page);
        }
        return completed(new ZLinkLocationPage<>(
            stored.items().stream()
                .map(this::projectCapacity)
                .toList(),
            stored.continuationToken()));
    }

    @Override
    public CompletionStage<ZLinkLocationWriteResult> updateClientServer(
        ZLinkClientServerServerDescriptor descriptor,
        ZLinkLocationWriteIntent intent) {
        Objects.requireNonNull(descriptor, "descriptor");
        Objects.requireNonNull(intent, "intent");
        ZLinkLocationOwnerToken owner = new ZLinkLocationOwnerToken(
            descriptor.ownerId(), descriptor.leaseGeneration());
        synchronized (gate) {
            if (!isExactOwnerLeaseLive(owner)) {
                return completed(ZLinkLocationWriteResult.ignoredStale());
            }
            String key = descriptor.channelName() + ":" + descriptor.serverRid().toHex();
            ZLinkClientServerServerDescriptor current = clientServers.rows.get(key);
            if (current != null
                && isExactOwnerLeaseLive(new ZLinkLocationOwnerToken(
                    current.ownerId(), current.leaseGeneration()))
                && intent == ZLinkLocationWriteIntent.NEW_CLAIM) {
                return completed(ZLinkLocationWriteResult.rejectedConflict());
            }
            if (current != null && intent == ZLinkLocationWriteIntent.RENEW
                && (!current.ownerId().equals(descriptor.ownerId())
                    || current.leaseGeneration() != descriptor.leaseGeneration()
                    || current.lifecycleGeneration() != descriptor.lifecycleGeneration()
                    || descriptor.descriptorRevision() < current.descriptorRevision())) {
                return completed(ZLinkLocationWriteResult.ignoredStale());
            }
            long generation = current == null
                ? clientServers.generations.getOrDefault(key, 0L) + 1L
                : clientServers.generations.getOrDefault(key, 1L);
            clientServers.generations.put(key, generation);
            Instant now = clock.instant();
            clientServers.rows.put(key, new ZLinkClientServerServerDescriptor(
                descriptor.channelName(), descriptor.serverRid(),
                descriptor.lifecycleGeneration(), descriptor.descriptorRevision(),
                descriptor.endpoint(), descriptor.weight(), descriptor.state(),
                descriptor.securityIdentity(), descriptor.ownerId(),
                descriptor.leaseGeneration(), now));
            return completed(ZLinkLocationWriteResult.stored(generation, now));
        }
    }

    @Override
    public CompletionStage<ZLinkLocationWriteStatus> removeClientServer(
        ZLinkClientServerServerDescriptorKey key,
        ZLinkLocationOwnerToken owner) {
        Objects.requireNonNull(key, "key");
        Objects.requireNonNull(owner, "owner");
        synchronized (gate) {
            String encoded = key.channelName() + ":" + key.serverRid().toHex();
            ZLinkClientServerServerDescriptor current = clientServers.rows.get(encoded);
            if (current == null || !current.ownerId().equals(owner.ownerId())
                || current.leaseGeneration() != owner.leaseGeneration()) {
                return completed(ZLinkLocationWriteStatus.IGNORED_STALE);
            }
            clientServers.rows.remove(encoded);
            return completed(ZLinkLocationWriteStatus.STORED);
        }
    }

    @Override
    public CompletionStage<ZLinkLocationPage<ZLinkClientServerServerDescriptor>>
        listClientServers(String channelName, ZLinkPageRequest page) {
        Objects.requireNonNull(channelName, "channelName");
        synchronized (gate) {
            return completed(page(
                clientServers,
                row -> row.channelName().equals(channelName)
                    && isExactOwnerLeaseLive(new ZLinkLocationOwnerToken(
                        row.ownerId(), row.leaseGeneration())),
                page));
        }
    }

    @Override
    public CompletionStage<ZLinkLocationWriteResult> updateFanoutPublisher(
        ZLinkFanoutPublisherDescriptor descriptor,
        ZLinkLocationWriteIntent intent) {
        Objects.requireNonNull(descriptor, "descriptor");
        Objects.requireNonNull(intent, "intent");
        validateFanoutDescriptor(descriptor);
        synchronized (gate) {
            ZLinkLocationOwnerToken owner = new ZLinkLocationOwnerToken(
                descriptor.ownerId(),
                descriptor.leaseGeneration());
            if (!isExactOwnerLeaseLive(owner)) {
                return completed(ZLinkLocationWriteResult.ignoredStale());
            }
            String key = fanoutPublisherKey(
                new ZLinkFanoutPublisherDescriptorKey(
                    descriptor.channelName(),
                    descriptor.publisherRid()));
            ZLinkFanoutPublisherDescriptor current =
                fanoutPublishers.rows.get(key);
            boolean currentLive = current != null
                && isExactOwnerLeaseLive(new ZLinkLocationOwnerToken(
                    current.ownerId(),
                    current.leaseGeneration()));
            if ((intent == ZLinkLocationWriteIntent.NEW_CLAIM
                || intent == ZLinkLocationWriteIntent.TAKEOVER)
                && currentLive
                && !hasSameFanoutDescriptorFields(current, descriptor)) {
                return completed(ZLinkLocationWriteResult.rejectedConflict());
            }
            if (current == null || !currentLive) {
                if (intent != ZLinkLocationWriteIntent.NEW_CLAIM
                    && intent != ZLinkLocationWriteIntent.TAKEOVER) {
                    return completed(ZLinkLocationWriteResult.ignoredStale());
                }
                long generation =
                    fanoutPublishers.generations.getOrDefault(key, 0L) + 1L;
                Instant now = clock.instant();
                fanoutPublishers.generations.put(key, generation);
                fanoutPublishers.rows.put(
                    key,
                    copyFanoutDescriptor(descriptor, now));
                return completed(
                    ZLinkLocationWriteResult.stored(generation, now));
            }
            long generation =
                fanoutPublishers.generations.getOrDefault(key, 1L);
            if (hasSameFanoutDescriptorFields(current, descriptor)) {
                return completed(ZLinkLocationWriteResult.stored(
                    generation,
                    current.updatedAt()));
            }
            if (!current.ownerId().equals(descriptor.ownerId())
                || current.leaseGeneration() != descriptor.leaseGeneration()
                || current.lifecycleGeneration()
                    != descriptor.lifecycleGeneration()
                || descriptor.descriptorRevision()
                    <= current.descriptorRevision()
                || !hasSameImmutableFanoutDescriptorFields(
                    current,
                    descriptor)) {
                return completed(ZLinkLocationWriteResult.ignoredStale());
            }
            Instant now = clock.instant();
            fanoutPublishers.rows.put(
                key,
                copyFanoutDescriptor(descriptor, now));
            return completed(ZLinkLocationWriteResult.stored(generation, now));
        }
    }

    @Override
    public CompletionStage<ZLinkLocationWriteStatus> removeFanoutPublisher(
        ZLinkFanoutPublisherDescriptorKey key,
        ZLinkLocationOwnerToken owner) {
        Objects.requireNonNull(key, "key");
        Objects.requireNonNull(owner, "owner");
        synchronized (gate) {
            String encoded = fanoutPublisherKey(key);
            ZLinkFanoutPublisherDescriptor current =
                fanoutPublishers.rows.get(encoded);
            if (current == null
                || !current.ownerId().equals(owner.ownerId())
                || current.leaseGeneration() != owner.leaseGeneration()) {
                return completed(ZLinkLocationWriteStatus.IGNORED_STALE);
            }
            fanoutPublishers.rows.remove(encoded);
            return completed(ZLinkLocationWriteStatus.STORED);
        }
    }

    @Override
    public CompletionStage<ZLinkLocationPage<ZLinkFanoutPublisherDescriptor>>
        listFanoutPublishers(String channelName, ZLinkPageRequest page) {
        Objects.requireNonNull(channelName, "channelName");
        synchronized (gate) {
            return completed(page(
                fanoutPublishers,
                row -> row.channelName().equals(channelName)
                    && isExactOwnerLeaseLive(new ZLinkLocationOwnerToken(
                        row.ownerId(),
                        row.leaseGeneration())),
                page));
        }
    }

    @Override
    public CompletionStage<systems.zlink.framework.runtime.internal.locations.ZLinkOwnerLeaseClaimResult>
        claimOwnerLease(
        String ownerId,
        Duration leaseTtl) {
        synchronized (gate) {
            Instant now = clock.instant();
            LeaseRow current = leases.get(ownerId);
            if (current != null && current.expiresAt().isAfter(now)) {
                return completed(new systems.zlink.framework.runtime.internal.locations.ZLinkOwnerLeaseClaimConflict());
            }
            if (ownerLeaseGeneration == Long.MAX_VALUE) {
                return completed(new systems.zlink.framework.runtime.internal.locations.ZLinkOwnerLeaseGenerationExhausted());
            }
            ZLinkLocationOwnerToken token = new ZLinkLocationOwnerToken(
                ownerId,
                ++ownerLeaseGeneration);
            Instant expiresAt = now.plus(leaseTtl);
            leases.put(ownerId, new LeaseRow(token, expiresAt));
            return completed(new systems.zlink.framework.runtime.internal.locations.ZLinkOwnerLeaseClaimed(token, expiresAt, now));
        }
    }

    @Override
    public CompletionStage<systems.zlink.framework.runtime.internal.locations.ZLinkOwnerLeaseReadResult>
        readOwnerLease(String ownerId) {
        synchronized (gate) {
            Instant now = clock.instant();
            LeaseRow current = leases.get(ownerId);
            if (current == null || !current.expiresAt().isAfter(now)) {
                leases.remove(ownerId);
                return completed(new systems.zlink.framework.runtime.internal.locations.ZLinkOwnerLeaseMissing());
            }
            return completed(new systems.zlink.framework.runtime.internal.locations.ZLinkOwnerLeaseFound(
                    current.token(),
                    current.expiresAt(),
                    now));
        }
    }

    @Override
    public CompletionStage<systems.zlink.framework.runtime.internal.locations.ZLinkOwnerLeaseRenewResult>
        renewOwnerLease(
            ZLinkLocationOwnerToken token,
            Duration leaseTtl) {
        synchronized (gate) {
            Instant now = clock.instant();
            LeaseRow current = leases.get(token.ownerId());
            if (current == null
                || !current.expiresAt().isAfter(now)
                || !current.token().equals(token)) {
                return completed(new systems.zlink.framework.runtime.internal.locations.ZLinkOwnerLeaseRenewStale());
            }
            Instant expiresAt = now.plus(leaseTtl);
            leases.put(token.ownerId(), new LeaseRow(token, expiresAt));
            return completed(new systems.zlink.framework.runtime.internal.locations.ZLinkOwnerLeaseRenewed(expiresAt, now));
        }
    }

    @Override
    public CompletionStage<systems.zlink.framework.runtime.internal.locations.ZLinkOwnerLeaseReleaseResult>
        releaseOwnerLease(ZLinkLocationOwnerToken token) {
        synchronized (gate) {
            Instant now = clock.instant();
            LeaseRow current = leases.get(token.ownerId());
            if (current == null
                || !current.expiresAt().isAfter(now)
                || !current.token().equals(token)) {
                if (current != null
                    && !current.expiresAt().isAfter(now)) {
                    leases.remove(token.ownerId());
                }
                return completed(systems.zlink.framework.runtime.internal.locations.ZLinkOwnerLeaseReleaseResult.STALE);
            }
            leases.remove(token.ownerId());
            return completed(systems.zlink.framework.runtime.internal.locations.ZLinkOwnerLeaseReleaseResult.RELEASED);
        }
    }

    @Override
    public CompletionStage<Long> removeAllByOwner(
        ZLinkLocationOwnerToken owner) {
        Objects.requireNonNull(owner, "owner");
        synchronized (gate) {
            if (!isExactOwnerLeaseLive(owner)) {
                return CompletableFuture.failedFuture(
                    new IllegalStateException(
                        "Owner cleanup token is stale."));
            }
            String ownerId = owner.ownerId();
            long removed = 0;
            List<Map.Entry<String, ZLinkMeshNodeDescriptor>>
                descriptorEntries = meshNodes.rows.entrySet()
                .stream()
                .filter(entry -> entry.getValue().ownerId()
                        .equals(ownerId)
                    && entry.getValue().leaseGeneration()
                        == owner.leaseGeneration())
                .toList();
            descriptorEntries.forEach(entry -> {
                entry.getValue().entrySpotId().ifPresent(spotId -> {
                    String authorityKey =
                        ZLinkAuthorityKeyCodec.spot(spotId);
                    EntrySpotClaim claim =
                        entrySpotClaims.get(authorityKey);
                    if (claim != null
                        && claim.matches(
                            entry.getKey(),
                            entry.getValue())) {
                        entrySpotClaims.remove(authorityKey);
                    }
                });
                meshNodes.rows.remove(entry.getKey());
                bumpMeshNodeStamp(entry.getValue().meshName());
            });
            removed += descriptorEntries.size();
            List<String> fanoutKeys = fanoutPublishers.rows.entrySet()
                .stream()
                .filter(entry -> entry.getValue().ownerId().equals(ownerId)
                    && entry.getValue().leaseGeneration()
                        == owner.leaseGeneration())
                .map(Map.Entry::getKey)
                .toList();
            fanoutKeys.forEach(fanoutPublishers.rows::remove);
            removed += fanoutKeys.size();
            return completed(removed);
        }
    }

    @Override
    public CompletionStage<OptionalLong> getMeshNodeChangeStamp(
        String meshName) {
        if (meshName == null || meshName.isBlank()) {
            throw new IllegalArgumentException("meshName must be non-blank");
        }
        synchronized (gate) {
            Long stamp = meshNodeStamps.get(meshName);
            return completed(stamp == null
                ? OptionalLong.empty()
                : OptionalLong.of(stamp));
        }
    }

    private void bumpMeshNodeStamp(String meshName) {
        meshNodeStamps.merge(meshName, 1L, Math::addExact);
    }

    private <TRow> ZLinkLocationPage<TRow> page(
        RowTable<TRow> table,
        Predicate<TRow> matches,
        ZLinkPageRequest request) {
        ZLinkPageRequest safeRequest = request == null ? ZLinkPageRequest.firstPage() : request;
        List<Map.Entry<String, TRow>> ordered = table.rows.entrySet().stream()
            .filter(pair -> matches.test(pair.getValue()))
            .sorted(Comparator.comparing(Map.Entry::getKey))
            .toList();

        int offset = parseOffset(safeRequest.continuationToken());
        int size = safeRequest.pageSize() > 0 ? safeRequest.pageSize() : Integer.MAX_VALUE;
        List<TRow> items = new ArrayList<>();
        for (int i = offset; i < ordered.size() && items.size() < size; i++) {
            items.add(ordered.get(i).getValue());
        }

        int nextOffset = offset + items.size();
        String next = nextOffset < ordered.size() ? Integer.toString(nextOffset) : null;
        return new ZLinkLocationPage<>(List.copyOf(items), next);
    }

    private int parseOffset(String token) {
        if (token == null || token.isBlank()) {
            return 0;
        }
        try {
            return Math.max(0, Integer.parseInt(token));
        } catch (NumberFormatException ignored) {
            return 0;
        }
    }

    private boolean isOwnerLive(String ownerId, Instant now) {
        LeaseRow lease = leases.get(ownerId);
        return lease != null && lease.expiresAt().isAfter(now);
    }

    private boolean isExactOwnerLeaseLive(
        ZLinkLocationOwnerToken token) {
        synchronized (gate) {
            LeaseRow lease = leases.get(token.ownerId());
            return lease != null
                && lease.token().equals(token)
                && lease.expiresAt().isAfter(clock.instant());
        }
    }

    private ZLinkMeshNodeDescriptor findMeshNodeDescriptor(
        ZLinkMeshNodeDescriptorKey key,
        long lifecycleGeneration,
        ZLinkLocationOwnerToken owner) {
        synchronized (gate) {
            return meshNodes.rows.get(meshNodeKey(key));
        }
    }

    private static String meshNodeKey(
        ZLinkMeshNodeDescriptorKey key) {
        return key.meshName().length()
            + ":"
            + key.meshName()
            + key.rid().toHex().length()
            + ":"
            + key.rid().toHex();
    }

    private static String fanoutPublisherKey(
        ZLinkFanoutPublisherDescriptorKey key) {
        return ZLinkLocationKeyCodec.encodeFanoutPublisherKey(key);
    }

    private static void validateFanoutDescriptor(
        ZLinkFanoutPublisherDescriptor descriptor) {
        if (descriptor.channelName() == null
            || descriptor.channelName().isBlank()
            || descriptor.publisherRid() == null
            || descriptor.endpoint() == null
            || descriptor.endpoint().isBlank()
            || descriptor.securityIdentity() == null
            || descriptor.securityIdentity().isBlank()
            || descriptor.ownerId() == null
            || descriptor.ownerId().isBlank()) {
            throw new IllegalArgumentException(
                "fanout descriptor identity and endpoint are required");
        }
        if (descriptor.lifecycleGeneration() < 1
            || descriptor.descriptorRevision() < 1
            || descriptor.leaseGeneration() < 1) {
            throw new IllegalArgumentException(
                "fanout descriptor generations must be positive");
        }
    }

    private static ZLinkFanoutPublisherDescriptor copyFanoutDescriptor(
        ZLinkFanoutPublisherDescriptor descriptor,
        Instant updatedAt) {
        return new ZLinkFanoutPublisherDescriptor(
            descriptor.channelName(),
            descriptor.publisherRid(),
            descriptor.lifecycleGeneration(),
            descriptor.descriptorRevision(),
            descriptor.endpoint(),
            descriptor.state(),
            descriptor.securityIdentity(),
            descriptor.ownerId(),
            descriptor.leaseGeneration(),
            updatedAt);
    }

    private static boolean hasSameImmutableFanoutDescriptorFields(
        ZLinkFanoutPublisherDescriptor current,
        ZLinkFanoutPublisherDescriptor candidate) {
        return current.channelName().equals(candidate.channelName())
            && current.publisherRid().equals(candidate.publisherRid())
            && current.lifecycleGeneration()
                == candidate.lifecycleGeneration()
            && current.endpoint().equals(candidate.endpoint())
            && current.securityIdentity().equals(
                candidate.securityIdentity())
            && current.ownerId().equals(candidate.ownerId())
            && current.leaseGeneration() == candidate.leaseGeneration();
    }

    private static boolean hasSameFanoutDescriptorFields(
        ZLinkFanoutPublisherDescriptor current,
        ZLinkFanoutPublisherDescriptor candidate) {
        return hasSameImmutableFanoutDescriptorFields(current, candidate)
            && current.descriptorRevision()
                == candidate.descriptorRevision()
            && current.state() == candidate.state();
    }

    private static ZLinkMeshNodeDescriptor copyDescriptor(
        ZLinkMeshNodeDescriptor descriptor,
        Instant updatedAt) {
        return new ZLinkMeshNodeDescriptor(
            descriptor.meshName(),
            descriptor.rid(),
            descriptor.lifecycleGeneration(),
            descriptor.descriptorRevision(),
            descriptor.endpoint(),
            descriptor.channelWeights(),
            descriptor.applicationVersion(),
            descriptor.objectCapabilities(),
            descriptor.objectRole(),
            descriptor.entrySpotId(),
            descriptor.placementWeight(),
            descriptor.capacity(),
            descriptor.activationConcurrency(),
            descriptor.maintenanceWave(),
            descriptor.state(),
            descriptor.securityIdentity(),
            descriptor.ownerId(),
            descriptor.leaseGeneration(),
            updatedAt);
    }

    private ZLinkMeshNodeDescriptor projectCapacity(
        ZLinkMeshNodeDescriptor descriptor) {
        var key = new ZLinkMeshNodeDescriptorKey(
            descriptor.meshName(), descriptor.rid());
        long[] actors = authority.kindCapacity(
            key, descriptor.lifecycleGeneration(), true);
        long[] spots = authority.kindCapacity(
            key, descriptor.lifecycleGeneration(), false);
        var spotTypes = descriptor.objectCapabilities().stream()
            .filter(capability ->
                capability.objectKind()
                    != ZLinkPlacementObjectKind.ACTOR)
            .map(capability -> {
                long[] usage = authority.typeCapacity(
                    key,
                    descriptor.lifecycleGeneration(),
                    capability.objectKind(),
                    capability.stableType());
                return new ZLinkSpotTypeCapacity(
                    capability.objectKind(),
                    capability.stableType(),
                    new ZLinkCapacityUsage(
                        Math.toIntExact(usage[0]),
                        Math.toIntExact(usage[1]),
                        capability.spotLimit()));
            })
            .toList();
        return new ZLinkMeshNodeDescriptor(
            descriptor.meshName(),
            descriptor.rid(),
            descriptor.lifecycleGeneration(),
            descriptor.descriptorRevision(),
            descriptor.endpoint(),
            descriptor.channelWeights(),
            descriptor.applicationVersion(),
            descriptor.objectCapabilities(),
            descriptor.objectRole(),
            descriptor.entrySpotId(),
            descriptor.placementWeight(),
            new ZLinkPlacementCapacity(
                new ZLinkCapacityUsage(
                    Math.toIntExact(actors[0]),
                    Math.toIntExact(actors[1]),
                    descriptor.capacity().actors().limit()),
                new ZLinkCapacityUsage(
                    Math.toIntExact(spots[0]),
                    Math.toIntExact(spots[1]),
                    descriptor.capacity().spots().limit()),
                spotTypes),
            descriptor.activationConcurrency(),
            descriptor.maintenanceWave(),
            descriptor.state(),
            descriptor.securityIdentity(),
            descriptor.ownerId(),
            descriptor.leaseGeneration(),
            descriptor.updatedAt());
    }

    private static boolean hasSameImmutableDescriptorFields(
        ZLinkMeshNodeDescriptor current,
        ZLinkMeshNodeDescriptor candidate) {
        return current.meshName().equals(candidate.meshName())
            && current.rid().equals(candidate.rid())
            && current.lifecycleGeneration()
                == candidate.lifecycleGeneration()
            && current.endpoint().equals(candidate.endpoint())
            && current.channelWeights().keySet().equals(
                candidate.channelWeights().keySet())
            && current.applicationVersion()
                == candidate.applicationVersion()
            && hasSameImmutableCapabilities(
                current.objectCapabilities(),
                candidate.objectCapabilities())
            && current.objectRole() == candidate.objectRole()
            && current.entrySpotId().equals(candidate.entrySpotId())
            && current.capacity().actors().limit()
                == candidate.capacity().actors().limit()
            && current.capacity().spots().limit()
                == candidate.capacity().spots().limit()
            && current.securityIdentity().equals(
                candidate.securityIdentity())
            && current.ownerId().equals(candidate.ownerId())
            && current.leaseGeneration()
                == candidate.leaseGeneration();
    }

    private static boolean hasSameImmutableCapabilities(
        List<systems.zlink.framework.locations.ZLinkObjectCapability> current,
        List<systems.zlink.framework.locations.ZLinkObjectCapability> candidate) {
        if (current.size() != candidate.size()) {
            return false;
        }
        for (int index = 0; index < current.size(); index++) {
            var left = current.get(index);
            var right = candidate.get(index);
            if (left.objectKind() != right.objectKind()
                || !left.stableType().equals(right.stableType())
                || left.policy() != right.policy()
                || left.hasSnapshotAdapter()
                    != right.hasSnapshotAdapter()
                || left.spotLimit() != right.spotLimit()) {
                return false;
            }
        }
        return true;
    }

    private static boolean hasSameDescriptorFields(
        ZLinkMeshNodeDescriptor current,
        ZLinkMeshNodeDescriptor candidate) {
        return current.meshName().equals(candidate.meshName())
            && current.rid().equals(candidate.rid())
            && current.lifecycleGeneration()
                == candidate.lifecycleGeneration()
            && current.descriptorRevision()
                == candidate.descriptorRevision()
            && current.endpoint().equals(candidate.endpoint())
            && current.channelWeights().equals(candidate.channelWeights())
            && current.applicationVersion()
                == candidate.applicationVersion()
            && current.objectCapabilities().equals(
                candidate.objectCapabilities())
            && current.objectRole() == candidate.objectRole()
            && current.placementWeight() == candidate.placementWeight()
            && current.capacity().equals(candidate.capacity())
            && current.maintenanceWave().equals(
                candidate.maintenanceWave())
            && current.state() == candidate.state()
            && current.securityIdentity().equals(
                candidate.securityIdentity())
            && current.ownerId().equals(candidate.ownerId())
            && current.leaseGeneration()
                == candidate.leaseGeneration();
    }

    private boolean isSpotIdentityClaimed(String authorityKey) {
        synchronized (gate) {
            EntrySpotClaim claim = entrySpotClaims.get(authorityKey);
            if (claim == null) {
                return false;
            }
            if (!isExactOwnerLeaseLive(claim.owner())) {
                entrySpotClaims.remove(authorityKey);
                return false;
            }
            return true;
        }
    }

    private static <T> CompletionStage<T> completed(T value) {
        return CompletableFuture.completedFuture(value);
    }

    private static final class RowTable<TRow> {
        private final Map<String, TRow> rows = new HashMap<>();
        private final Map<String, Long> generations = new HashMap<>();
    }

    private record LeaseRow(
        ZLinkLocationOwnerToken token,
        Instant expiresAt) {
    }

    private record EntrySpotClaim(
        String descriptorKey,
        long lifecycleGeneration,
        ZLinkLocationOwnerToken owner) {
        private boolean matches(
            String expectedDescriptorKey,
            ZLinkMeshNodeDescriptor descriptor) {
            return descriptorKey.equals(expectedDescriptorKey)
                && lifecycleGeneration
                    == descriptor.lifecycleGeneration()
                && owner.ownerId().equals(descriptor.ownerId())
                && owner.leaseGeneration()
                    == descriptor.leaseGeneration();
        }
    }
}
