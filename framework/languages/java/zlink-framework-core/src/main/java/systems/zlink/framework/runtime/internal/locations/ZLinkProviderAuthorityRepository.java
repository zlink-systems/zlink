package systems.zlink.framework.runtime.internal.locations;
import java.time.Duration;
import java.util.UUID;
import java.util.concurrent.ThreadLocalRandom;
import java.util.concurrent.TimeUnit;
import systems.zlink.framework.locationprovider.ZLinkLocationStore;

import com.fasterxml.jackson.core.JsonProcessingException;
import com.fasterxml.jackson.databind.JsonNode;
import com.fasterxml.jackson.databind.ObjectMapper;
import com.fasterxml.jackson.databind.node.ObjectNode;
import java.io.ByteArrayInputStream;
import java.io.ByteArrayOutputStream;
import java.io.DataInputStream;
import java.io.DataOutputStream;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.time.Instant;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Base64;
import java.util.HexFormat;
import java.util.HashMap;
import java.util.List;
import java.util.Map;
import java.util.Objects;
import java.util.Optional;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.locationprovider.*;
import systems.zlink.framework.locations.ZLinkPlacementObjectKind;
import systems.zlink.framework.locations.ZLinkMeshNodeObjectRole;
import systems.zlink.framework.runtime.host.ZLinkFrameworkRuntimeState;
import systems.zlink.framework.runtime.locations.ZLinkAuthorityKeyCodec;

/**
 * Implements Framework-owned authority records over the opaque provider SPI.
 */
final class ZLinkProviderAuthorityRepository {
    private static final ObjectMapper CANONICAL_JSON = new ObjectMapper();
    private static final String CAPACITY_PREFIX = "zlink:v11:capacity:";
    private static final String COUNTER_PREFIX = "zlink:v11:counter:";
    private static final String AGGREGATE_PREFIX = "zlink:v11:aggregate:";
    private static final byte AGGREGATE_STAGING = 0;
    private static final byte AGGREGATE_PREPARED = 1;
    private static final byte AGGREGATE_COMMITTED = 2;
    private static final Duration AGGREGATE_COMMIT_RETRY_WINDOW =
        Duration.ofSeconds(5);
    private static final int AGGREGATE_COMMIT_RETRY_LIMIT = 64;
    private final ZLinkLocationStore
        provider;
    private final ZLinkProviderDescriptorRepository descriptors;
    private final ZLinkAggregateInventoryStore aggregateInventory;

    ZLinkProviderAuthorityRepository(
        ZLinkLocationStore provider) {
        this(provider, new ZLinkProviderDescriptorRepository(provider));
    }

    ZLinkProviderAuthorityRepository(
        ZLinkLocationStore provider,
        ZLinkProviderDescriptorRepository descriptors) {
        this.provider = Objects.requireNonNull(provider, "provider");
        this.descriptors = Objects.requireNonNull(descriptors, "descriptors");
        this.aggregateInventory = new ZLinkAggregateInventoryStore(provider);
    }

    CompletionStage<ZLinkAuthorityReadResult> read(
        String key,
        ZLinkStoreCancellation cancellation) {
        requireKey(key);
        return provider.read(authorityKey(key), adapt(cancellation))
            .thenCompose(result -> projectRead(result, adapt(cancellation)));
    }

    CompletionStage<ZLinkAuthorityWriteResult> compareExchange(
        String key,
        ZLinkAuthorityExpectation expectation,
        ZLinkAuthorityMutation mutation,
        ZLinkStoreCancellation cancellation) {
        requireKey(key);
        Objects.requireNonNull(expectation, "expectation");
        Objects.requireNonNull(mutation, "mutation");
        var opaqueCancellation = adapt(cancellation);
        ZLinkStoreKey rowKey = authorityKey(key);
        return provider.read(rowKey, opaqueCancellation)
            .thenCompose(read -> {
                if (!(read instanceof ZLinkStoreReadFound found)
                    || !(expectation instanceof ZLinkAuthorityExpectFound expected)
                    || !found.value().version().value().equals(
                        expected.storeVersion())) {
                    return projectRead(read, opaqueCancellation)
                        .thenApply(visible ->
                            new ZLinkAuthorityConflict(visible));
                }
                AuthorityRecord current = decode(found.value().bytes());
                if (current.aggregate() != null) {
                    return projectRead(read, opaqueCancellation)
                        .thenApply(visible ->
                            new ZLinkAuthorityConflict(visible));
                }
                List<ZLinkStoreCondition> conditions = new ArrayList<>();
                conditions.add(new ZLinkStoreVersionCondition(
                    rowKey, found.value().version()));
                if (mutation instanceof ZLinkAuthorityDelete) {
                    return requireLiveOwner(current, conditions,
                            opaqueCancellation)
                        .thenCompose(live -> {
                            if (!live) {
                                return completed(
                                    new ZLinkAuthorityConflict(toRead(read)));
                            }
                            return readCapacity(
                                    current.allocation(), opaqueCancellation)
                                .thenCompose(capacity -> {
                                    List<ZLinkStoreMutation> mutations =
                                        new ArrayList<>();
                                    mutations.add(new ZLinkStoreDelete(rowKey));
                                    if (capacity.isPresent()) {
                                        CapacitySnapshot stored =
                                            capacity.orElseThrow();
                                        CapacityRecord next = stored.record()
                                            .adjustActive(
                                                current.allocation()
                                                    .capacityBundle(),
                                                -1);
                                        if (next == null) {
                                            return completed(
                                                new ZLinkAuthorityConflict(
                                                    toRead(read)));
                                        }
                                        conditions.add(
                                            new ZLinkStoreVersionCondition(
                                                stored.key(),
                                                stored.value().version()));
                                        mutations.add(new ZLinkStorePut(
                                            stored.key(),
                                            encodeCapacity(next),
                                            null));
                                    }
                                    return provider.write(
                                            new ZLinkStoreWriteRequest(
                                                conditions, mutations),
                                            opaqueCancellation)
                                        .thenApply(result -> result
                                            instanceof ZLinkStoreWriteApplied applied
                                            ? new ZLinkAuthorityDeleted(
                                                found.value().version().value(),
                                                applied.storeNow())
                                            : new ZLinkAuthorityConflict(
                                                toRead(read)));
                                });
                        });
                }
                if (mutation instanceof ZLinkAuthorityRestore restore) {
                    if (!current.ownerId().equals(
                            restore.expectedOwner().ownerId())
                        || current.ownerLeaseGeneration()
                            != restore.expectedOwner().leaseGeneration()) {
                        return completed(
                            new ZLinkAuthorityConflict(toRead(read)));
                    }
                    AuthorityRecord next = current.withPayload(
                        restore.payload());
                    return put(rowKey, found, next, conditions,
                        opaqueCancellation, read);
                }
                ZLinkAuthorityPut put = (ZLinkAuthorityPut) mutation;
                ZLinkLocationOwnerToken owner =
                    new ZLinkLocationOwnerToken(
                        current.ownerId(), current.ownerLeaseGeneration());
                return requireLiveOwner(owner, conditions,
                        opaqueCancellation)
                    .thenCompose(live -> {
                        if (!live) {
                            return completed(
                                new ZLinkAuthorityConflict(toRead(read)));
                        }
                        return put(
                            rowKey,
                            found,
                            current.withPayload(put.payload()),
                            conditions,
                            opaqueCancellation,
                            read);
                    });
            });
    }

    private CompletionStage<CapacityPlan> reserveCapacity(
        ZLinkObjectReservationRequest request,
        List<ZLinkStoreCondition> conditions,
        systems.zlink.framework.locationprovider.ZLinkStoreCancellation
            cancellation) {
        return descriptors.readMeshNode(
                request.targetDescriptor(), cancellation)
            .thenCompose(descriptor -> {
                if (descriptor.isEmpty()
                    || !descriptorAdmits(
                        descriptor.orElseThrow(), request)) {
                    return completed(new CapacityPlan(
                        CapacityAdmission.UNAVAILABLE,
                        null,
                        null));
                }
                ZLinkStoreKey key = capacityKey(
                    request.targetDescriptor(),
                    request.targetDescriptorLifecycleGeneration());
                return provider.read(key, cancellation)
                    .thenApply(read -> {
                        CapacityRecord current;
                        if (read instanceof ZLinkStoreReadFound found) {
                            current = decodeCapacity(found.value().bytes());
                            if (!capacityFits(
                                current,
                                descriptor.orElseThrow(),
                                request.capacityBundle())) {
                                return new CapacityPlan(
                                    CapacityAdmission.EXHAUSTED,
                                    null,
                                    null);
                            }
                            conditions.add(new ZLinkStoreVersionCondition(
                                key, found.value().version()));
                        } else {
                            current = CapacityRecord.empty();
                            if (!capacityFits(
                                current,
                                descriptor.orElseThrow(),
                                request.capacityBundle())) {
                                return new CapacityPlan(
                                    CapacityAdmission.EXHAUSTED,
                                    null,
                                    null);
                            }
                            conditions.add(new ZLinkStoreMissingCondition(key));
                        }
                        CapacityRecord next = current.adjustPending(
                            request.capacityBundle(), 1);
                        return next == null
                            ? new CapacityPlan(
                                CapacityAdmission.EXHAUSTED, null, null)
                            : new CapacityPlan(
                                CapacityAdmission.ACCEPTED, key, next);
                    });
            });
    }

    private CompletionStage<Optional<CapacitySnapshot>> readCapacity(
        ZLinkPlacementAllocation allocation,
        systems.zlink.framework.locationprovider.ZLinkStoreCancellation
            cancellation) {
        ZLinkStoreKey key = capacityKey(
            allocation.descriptor(),
            allocation.descriptorLifecycleGeneration());
        return provider.read(key, cancellation).thenApply(read ->
            read instanceof ZLinkStoreReadFound found
                ? Optional.of(new CapacitySnapshot(
                    key,
                    found.value(),
                    decodeCapacity(found.value().bytes())))
                : Optional.empty());
    }

    private static boolean descriptorAdmits(
        ZLinkMeshNodeDescriptor descriptor,
        ZLinkObjectReservationRequest request) {
        return descriptor.meshName().equals(
                request.targetDescriptor().meshName())
            && descriptor.rid().equals(request.targetDescriptor().rid())
            && descriptor.lifecycleGeneration()
                == request.targetDescriptorLifecycleGeneration()
            && descriptor.ownerId().equals(request.targetOwner().ownerId())
            && descriptor.leaseGeneration()
                == request.targetOwner().leaseGeneration()
            && descriptor.state() == ZLinkFrameworkRuntimeState.SERVING
            && descriptor.objectRole() == ZLinkMeshNodeObjectRole.SERVER
            && descriptor.placementWeight() > 0
            && descriptor.objectCapabilities().stream().anyMatch(capability ->
                capability.objectKind() == request.objectKind()
                    && capability.stableType().equals(request.stableType()))
            && bundleMatchesObject(
                request.capacityBundle(),
                request.objectKind(),
                request.stableType());
    }

    private static boolean capacityFits(
        CapacityRecord current,
        ZLinkMeshNodeDescriptor descriptor,
        ZLinkPlacementCapacityBundle bundle) {
        return hasCapacity(
                (long) current.actorActive() + current.actorPending(),
                descriptor.capacity().actors().limit(),
                bundle.actorSlots())
            && hasCapacity(
                (long) current.spotActive() + current.spotPending(),
                descriptor.capacity().spots().limit(),
                bundle.spotSlots())
            && bundle.spotType().map(delta -> descriptor.objectCapabilities()
                .stream()
                .filter(capability ->
                    capability.objectKind() == delta.objectKind()
                        && capability.stableType().equals(delta.stableType()))
                .findFirst()
                .map(capability -> hasCapacity(
                    current.typeCount(delta),
                    capability.spotLimit(),
                    delta.slots()))
                .orElse(false)).orElse(true);
    }

    private static boolean hasCapacity(long used, int limit, int requested) {
        return limit == 0 || (long) used + requested <= limit;
    }

    private static boolean bundleMatchesObject(
        ZLinkPlacementCapacityBundle bundle,
        ZLinkPlacementObjectKind objectKind,
        String stableType) {
        if (objectKind == ZLinkPlacementObjectKind.ACTOR) {
            return bundle.actorSlots() > 0
                && bundle.spotSlots() == 0
                && bundle.spotType().isEmpty();
        }
        return bundle.actorSlots() == 0
            && bundle.spotSlots() > 0
            && bundle.spotType().isPresent()
            && bundle.spotType().orElseThrow().objectKind() == objectKind
            && bundle.spotType().orElseThrow().stableType().equals(stableType);
    }

    CompletionStage<ZLinkAuthorityScanResult> list(
        String prefix,
        Optional<ZLinkAuthorityScanCursor> cursor,
        int limit,
        ZLinkStoreCancellation cancellation) {
        if (limit <= 0) {
            throw new IllegalArgumentException(
                "authority scan limit must be positive");
        }
        ZLinkStoreScanCursor providerCursor = cursor
            .map(value -> new ZLinkStoreScanCursor(value.encoded()))
            .orElse(null);
        return provider.scan(
                new ZLinkStoreScanRequest(
                    AUTHORITY_PREIMAGE_PREFIX,
                    providerCursor,
                    limit),
                adapt(cancellation))
            .thenCompose(result -> {
                if (result instanceof ZLinkStoreScanExpired) {
                    return completed(new ZLinkAuthorityScanExpired());
                }
                ZLinkStoreScanPage page =
                    ((ZLinkStoreScanPageResult) result).value();
                List<ZLinkAuthorityEntry> items = new ArrayList<>();
                CompletionStage<Void> visible =
                    CompletableFuture.completedFuture(null);
                for (ZLinkStoreScanItem item : page.items()) {
                    String authorityKey = decodeAuthorityKey(item.key());
                    if (!authorityKey.startsWith(prefix)) {
                        continue;
                    }
                    visible = visible.thenCompose(ignored ->
                        projectRead(
                                new ZLinkStoreReadFound(item.value()),
                                adapt(cancellation))
                            .thenAccept(read -> items.add(
                                new ZLinkAuthorityEntry(
                                    authorityKey,
                                    (ZLinkAuthoritySnapshot) read))));
                }
                return visible.thenApply(ignored -> new ZLinkAuthorityPage(
                        List.copyOf(items),
                        page.nextCursor() == null
                            ? Optional.empty()
                            : Optional.of(new ZLinkAuthorityScanCursor(
                                page.nextCursor().value()))));
            });
    }

    CompletionStage<ZLinkObjectReserveResult> reserve(
        ZLinkObjectReservationRequest request,
        ZLinkStoreCancellation cancellation) {
        Objects.requireNonNull(request, "request");
        var opaqueCancellation = adapt(cancellation);
        ZLinkStoreKey key = authorityKey(request.authorityKey());
        return provider.read(key, opaqueCancellation).thenCompose(read -> {
            if (read instanceof ZLinkStoreReadFound found) {
                ZLinkAuthoritySnapshot current = snapshot(found.value());
                if (decode(found.value().bytes()).aggregate() != null) {
                    return projectRead(read, opaqueCancellation)
                        .thenApply(visible -> new ZLinkObjectConflict(
                            (ZLinkAuthoritySnapshot) visible));
                }
                return completed(
                    current.allocation().stableType().equals(
                        request.stableType())
                        && current.allocation().state()
                            == ZLinkPlacementAllocationState.ACTIVE
                        ? new ZLinkObjectAlreadyExists(current)
                        : current.allocation().stableType().equals(
                            request.stableType())
                            ? new ZLinkObjectConflict(current)
                            : new ZLinkObjectTypeMismatch(current));
            }
            List<ZLinkStoreCondition> conditions = new ArrayList<>();
            conditions.add(new ZLinkStoreMissingCondition(key));
            return requireLiveOwner(
                    request.targetOwner(),
                    conditions,
                    opaqueCancellation)
                .thenCompose(live -> {
                    if (!live) {
                        return completed(new ZLinkObjectConflict(
                            toRead(read)));
                    }
                    return reserveCapacity(
                            request,
                            conditions,
                            opaqueCancellation)
                        .thenCompose(capacity -> {
                            if (capacity.admission()
                                == CapacityAdmission.UNAVAILABLE) {
                                return completed(new ZLinkObjectConflict(
                                    new ZLinkAuthorityMissing(
                                        Instant.now())));
                            }
                            if (capacity.admission()
                                == CapacityAdmission.EXHAUSTED) {
                                return completed(
                                    new ZLinkPlacementCapacityExhausted());
                            }
                            return nextPair(
                                    conditions,
                                    opaqueCancellation)
                                .thenCompose(counters -> {
                            String reservationVersion =
                                UUID.randomUUID().toString();
                            AuthorityRecord record = new AuthorityRecord(
                                request.creatingPayload(),
                                counters.objectGeneration(),
                                counters.ownerGeneration(),
                                request.targetOwner().ownerId(),
                                request.targetOwner().leaseGeneration(),
                                new ZLinkPlacementAllocation(
                                    ZLinkPlacementAllocationState.PENDING,
                                    request.objectKind(),
                                    request.stableType(),
                                    request.targetDescriptor(),
                                    request
                                        .targetDescriptorLifecycleGeneration(),
                                    request.capacityBundle()),
                                Optional.of(new ZLinkPendingObjectCreation(
                                    reservationVersion,
                                    request.creationIntentReference(),
                                    request.creationIntentHash(),
                                    request.creationIntentEncodedSize())));
                            List<ZLinkStoreMutation> mutations =
                                new ArrayList<>(counters.mutations());
                            mutations.add(new ZLinkStorePut(
                                capacity.key(),
                                encodeCapacity(capacity.next()),
                                null));
                            mutations.add(new ZLinkStorePut(
                                key, encode(record), null));
                            return provider.write(
                                    new ZLinkStoreWriteRequest(
                                        conditions, mutations),
                                    opaqueCancellation)
                                .thenCompose(result -> {
                                    if (result
                                        instanceof ZLinkStoreWriteConflict) {
                                        return reserve(request, cancellation);
                                    }
                                    var applied =
                                        (ZLinkStoreWriteApplied) result;
                                    return completed(
                                        new ZLinkObjectReserved(
                                            new ZLinkObjectReservation(
                                                request.authorityKey(),
                                                applied.putVersions()
                                                    .get(key).value(),
                                                counters.objectGeneration(),
                                                counters.ownerGeneration(),
                                                reservationVersion,
                                                request.targetDescriptor(),
                                                request
                                                    .targetDescriptorLifecycleGeneration(),
                                                    request.targetOwner())));
                                });
                        });
                        });
                });
        });
    }

    CompletionStage<ZLinkObjectCommitResult> commit(
        ZLinkObjectReservation reservation,
        byte[] readyPayload,
        ZLinkCreationOperationTerminal terminal,
        ZLinkStoreCancellation cancellation) {
        Objects.requireNonNull(reservation, "reservation");
        Objects.requireNonNull(readyPayload, "readyPayload");
        ZLinkStoreKey key = authorityKey(reservation.authorityKey());
        var opaqueCancellation = adapt(cancellation);
        return provider.read(key, opaqueCancellation).thenCompose(read -> {
            if (!(read instanceof ZLinkStoreReadFound found)) {
                return completed(ZLinkObjectCommitResult.STALE);
            }
            AuthorityRecord current = decode(found.value().bytes());
            if (current.aggregate() != null) {
                return completed(ZLinkObjectCommitResult.STALE);
            }
            if (!matches(current, reservation)) {
                return completed(
                    current.pendingCreation().isEmpty()
                        ? ZLinkObjectCommitResult.ALREADY_COMMITTED
                        : ZLinkObjectCommitResult.STALE);
            }
            return readCapacity(current.allocation(), opaqueCancellation)
                .thenCompose(capacity -> {
                    if (capacity.isEmpty()) {
                        return completed(ZLinkObjectCommitResult.STALE);
                    }
                    CapacitySnapshot stored = capacity.orElseThrow();
                    CapacityRecord nextCapacity = stored.record().transition(
                        current.allocation().capacityBundle());
                    if (nextCapacity == null) {
                        return completed(ZLinkObjectCommitResult.STALE);
                    }
                    AuthorityRecord next = new AuthorityRecord(
                        readyPayload,
                        current.objectGeneration(),
                        current.authorityOwnerGeneration(),
                        current.ownerId(),
                        current.ownerLeaseGeneration(),
                        withState(
                            current.allocation(),
                            ZLinkPlacementAllocationState.ACTIVE),
                        Optional.empty());
                    return provider.write(
                            new ZLinkStoreWriteRequest(
                                List.of(
                                    new ZLinkStoreVersionCondition(
                                        key, found.value().version()),
                                    new ZLinkStoreVersionCondition(
                                        stored.key(),
                                        stored.value().version())),
                                List.of(
                                    new ZLinkStorePut(
                                        key, encode(next), null),
                                    new ZLinkStorePut(
                                        stored.key(),
                                        encodeCapacity(nextCapacity),
                                        null))),
                            opaqueCancellation)
                        .thenApply(result -> result
                            instanceof ZLinkStoreWriteApplied
                            ? ZLinkObjectCommitResult.COMMITTED
                            : ZLinkObjectCommitResult.STALE);
                });
        });
    }

    CompletionStage<ZLinkObjectAbortResult> abort(
        ZLinkObjectReservation reservation,
        ZLinkStoreCancellation cancellation) {
        ZLinkStoreKey key = authorityKey(reservation.authorityKey());
        var opaqueCancellation = adapt(cancellation);
        return provider.read(key, opaqueCancellation).thenCompose(read -> {
            if (!(read instanceof ZLinkStoreReadFound found)
                || decode(found.value().bytes()).aggregate() != null
                || !matches(decode(found.value().bytes()), reservation)) {
                return completed(ZLinkObjectAbortResult.STALE);
            }
            AuthorityRecord current = decode(found.value().bytes());
            return readCapacity(current.allocation(), opaqueCancellation)
                .thenCompose(capacity -> {
                    if (capacity.isEmpty()) {
                        return completed(ZLinkObjectAbortResult.STALE);
                    }
                    CapacitySnapshot stored = capacity.orElseThrow();
                    CapacityRecord next = stored.record().adjustPending(
                        current.allocation().capacityBundle(), -1);
                    if (next == null) {
                        return completed(ZLinkObjectAbortResult.STALE);
                    }
                    return provider.write(
                            new ZLinkStoreWriteRequest(
                                List.of(
                                    new ZLinkStoreVersionCondition(
                                        key, found.value().version()),
                                    new ZLinkStoreVersionCondition(
                                        stored.key(),
                                        stored.value().version())),
                                List.of(
                                    new ZLinkStoreDelete(key),
                                    new ZLinkStorePut(
                                        stored.key(),
                                        encodeCapacity(next),
                                        null))),
                            opaqueCancellation)
                        .thenApply(result -> result
                            instanceof ZLinkStoreWriteApplied
                            ? ZLinkObjectAbortResult.ABORTED
                            : ZLinkObjectAbortResult.STALE);
                });
        });
    }

    CompletionStage<ZLinkObjectRejectResult> reject(
        ZLinkObjectReservation reservation,
        ZLinkCreationOperationTerminal terminal,
        ZLinkStoreCancellation cancellation) {
        Objects.requireNonNull(terminal, "terminal");
        return abort(reservation, cancellation).thenApply(result -> switch (
            result) {
            case ABORTED -> ZLinkObjectRejectResult.REJECTED;
            case ALREADY_ABORTED -> ZLinkObjectRejectResult.ALREADY_REJECTED;
            case STALE -> ZLinkObjectRejectResult.STALE;
        });
    }

    CompletionStage<ZLinkCreationTerminalReadResult> readCreationTerminal(
        ZLinkCreationOperationIdentity operation,
        ZLinkStoreCancellation cancellation) {
        Objects.requireNonNull(operation, "operation");
        return completed(new ZLinkCreationTerminalMissing());
    }

    CompletionStage<ZLinkAggregatePrepareResult> prepareAggregate(
        ZLinkAggregatePrepareRequest request,
        ZLinkStoreCancellation cancellation) {
        Objects.requireNonNull(request, "request");
        ZLinkAggregateFence fence = new ZLinkAggregateFence(
            request.aggregateId(), request.aggregateGeneration());
        ZLinkStoreKey key = aggregateKey(fence);
        var opaqueCancellation = adapt(cancellation);
        return provider.read(key, opaqueCancellation).thenCompose(existing -> {
            if (existing instanceof ZLinkStoreReadFound found) {
                PreparedAggregate prepared =
                    decodeAggregate(found.value().bytes());
                if (!sameAggregateRequest(prepared, request)) {
                    return completed(new ZLinkAggregateConflict());
                }
                if (prepared.state() == AGGREGATE_COMMITTED) {
                    return completed(new ZLinkAggregateStale());
                }
                if (prepared.state() == AGGREGATE_PREPARED) {
                    return aggregateInventory.load(
                            fence,
                            prepared.participantCount(),
                            prepared.inventoryDigest(),
                            opaqueCancellation)
                        .thenApply(ignored ->
                            new ZLinkAggregateAlreadyPrepared(fence));
                }
                if (prepared.state() != AGGREGATE_STAGING) {
                    return completed(new ZLinkAggregateConflict());
                }
                return continueAggregatePrepare(
                    request,
                    fence,
                    key,
                    found,
                    opaqueCancellation,
                    false);
            }
            return provider.write(
                    new ZLinkStoreWriteRequest(
                        List.of(new ZLinkStoreMissingCondition(key)),
                        List.of(new ZLinkStorePut(
                            key,
                            encodeAggregate(AGGREGATE_STAGING, request),
                            null))),
                    opaqueCancellation)
                .thenCompose(result -> {
                    if (!(result instanceof ZLinkStoreWriteApplied applied)) {
                        return prepareAggregate(request, cancellation);
                    }
                    ZLinkStoreVersion version = applied.putVersions().get(key);
                    if (version == null) {
                        return failed(new IllegalStateException(
                            "aggregate staging did not return a StoreVersion"));
                    }
                    return continueAggregatePrepare(
                        request,
                        fence,
                        key,
                        new ZLinkStoreReadFound(new ZLinkStoreValue(
                            encodeAggregate(AGGREGATE_STAGING, request),
                            version,
                            null,
                            applied.storeNow())),
                        opaqueCancellation,
                        true);
                });
        });
    }

    private CompletionStage<ZLinkAggregatePrepareResult>
        continueAggregatePrepare(
        ZLinkAggregatePrepareRequest request,
        ZLinkAggregateFence fence,
        ZLinkStoreKey key,
        ZLinkStoreReadFound staging,
        systems.zlink.framework.locationprovider.ZLinkStoreCancellation
            cancellation,
        boolean ownsStaging) {
        return aggregateInventory.store(request, cancellation)
            .thenCompose(ignored -> installAggregateMarkers(
                request,
                fence,
                cancellation))
            .thenCompose(marked -> {
                if (!marked) {
                    CompletionStage<Boolean> cleanup = ownsStaging
                        ? abortOwnedAggregateStaging(
                            fence,
                            staging.value().version().value(),
                            cancellation)
                        : completed(false);
                    return cleanup.thenApply(ignored ->
                        new ZLinkAggregateConflict());
                }
                return provider.write(
                        new ZLinkStoreWriteRequest(
                            List.of(new ZLinkStoreVersionCondition(
                                key, staging.value().version())),
                            List.of(new ZLinkStorePut(
                                key,
                                encodeAggregate(
                                    AGGREGATE_PREPARED,
                                    request),
                                null))),
                        cancellation)
                    .thenCompose(result -> {
                        if (result instanceof ZLinkStoreWriteApplied) {
                            return completed(
                                new ZLinkAggregatePrepared(fence));
                        }
                        return provider.read(key, cancellation)
                            .thenApply(raced -> {
                                if (!(raced instanceof ZLinkStoreReadFound found)) {
                                    return new ZLinkAggregateConflict();
                                }
                                PreparedAggregate current =
                                    decodeAggregate(found.value().bytes());
                                if (!sameAggregateRequest(current, request)) {
                                    return new ZLinkAggregateConflict();
                                }
                                return current.state() == AGGREGATE_PREPARED
                                    ? new ZLinkAggregateAlreadyPrepared(fence)
                                    : current.state() == AGGREGATE_COMMITTED
                                        ? new ZLinkAggregateStale()
                                        : new ZLinkAggregateConflict();
                            });
                    });
            });
    }

    private CompletionStage<Boolean> abortOwnedAggregateStaging(
        ZLinkAggregateFence fence,
        String expectedStoreVersion,
        systems.zlink.framework.locationprovider.ZLinkStoreCancellation
            cancellation) {
        ZLinkStoreKey key = aggregateKey(fence);
        return provider.read(key, cancellation).thenCompose(read -> {
            if (!(read instanceof ZLinkStoreReadFound found)
                || !found.value().version().value().equals(
                    expectedStoreVersion)) {
                return completed(false);
            }
            PreparedAggregate aggregate = decodeAggregate(found.value().bytes());
            if (aggregate.state() != AGGREGATE_STAGING) {
                return completed(false);
            }
            return provider.write(
                    new ZLinkStoreWriteRequest(
                        List.of(new ZLinkStoreVersionCondition(
                            key, found.value().version())),
                        List.of(new ZLinkStoreDelete(key))),
                    cancellation)
                .thenCompose(result -> {
                    if (!(result instanceof ZLinkStoreWriteApplied)) {
                        return completed(false);
                    }
                    return clearAggregateMarkersByScan(fence, cancellation)
                        .thenCompose(cleaned -> cleaned
                            ? aggregateInventory.delete(fence, cancellation)
                                .thenApply(ignored -> true)
                            : completed(false));
                });
        });
    }

    CompletionStage<ZLinkAggregateCommitResult> commitAggregate(
        ZLinkAggregateFence fence,
        ZLinkStoreCancellation cancellation) {
        Objects.requireNonNull(fence, "fence");
        Objects.requireNonNull(cancellation, "cancellation");
        return commitAggregate(
            fence,
            cancellation,
            0,
            Instant.now().plus(AGGREGATE_COMMIT_RETRY_WINDOW));
    }

    private CompletionStage<ZLinkAggregateCommitResult> commitAggregate(
        ZLinkAggregateFence fence,
        ZLinkStoreCancellation cancellation,
        int retryAttempt,
        Instant retryDeadline) {
        ZLinkStoreKey marker = aggregateKey(fence);
        var opaqueCancellation = adapt(cancellation);
        return provider.read(marker, opaqueCancellation).thenCompose(read -> {
            if (!(read instanceof ZLinkStoreReadFound found)) {
                return completed(ZLinkAggregateCommitResult.STALE);
            }
            PreparedAggregate prepared =
                decodeAggregate(found.value().bytes());
            if (prepared.state() == AGGREGATE_COMMITTED) {
                return aggregateInventory.load(
                        fence,
                        prepared.participantCount(),
                        prepared.inventoryDigest(),
                        opaqueCancellation)
                    .thenCompose(participants -> normalizeAggregateParticipants(
                            fence,
                            prepared.request(participants),
                            opaqueCancellation))
                    .thenApply(ignored ->
                        ZLinkAggregateCommitResult.ALREADY_COMMITTED);
            }
            if (prepared.state() != AGGREGATE_PREPARED) {
                return completed(ZLinkAggregateCommitResult.STALE);
            }
            return aggregateInventory.load(
                    fence,
                    prepared.participantCount(),
                    prepared.inventoryDigest(),
                    opaqueCancellation)
                .thenCompose(participants -> {
                    ZLinkAggregatePrepareRequest request =
                        prepared.request(participants);
                    return loadParticipants(
                            request.participants(),
                            opaqueCancellation)
                        .thenCompose(rows -> {
                            if (rows.size() != request.participants().size()
                                || !aggregateMarkersMatch(
                                    request,
                                    fence,
                                    rows)) {
                                return completed(
                                    ZLinkAggregateCommitResult.STALE);
                            }
                            List<ZLinkStoreCondition> conditions =
                                new ArrayList<>();
                            conditions.add(new ZLinkStoreVersionCondition(
                                marker, found.value().version()));
                            return requireLiveOwner(
                                    request.targetOwner(),
                                    conditions,
                                    opaqueCancellation)
                                .thenCompose(live -> {
                                    if (!live) {
                                        return completed(
                                            ZLinkAggregateCommitResult.STALE);
                                    }
                                    return provider.write(
                                            new ZLinkStoreWriteRequest(
                                                conditions,
                                                List.of(new ZLinkStorePut(
                                                    marker,
                                                    encodeAggregate(
                                                        AGGREGATE_COMMITTED,
                                                        request),
                                                    null))),
                                            opaqueCancellation)
                                        .thenCompose(result -> {
                                            if (!(result
                                                instanceof ZLinkStoreWriteApplied)) {
                                                return retryAggregateCommit(
                                                    fence,
                                                    cancellation,
                                                    retryAttempt,
                                                    retryDeadline);
                                            }
                                            return normalizeAggregateParticipants(
                                                    fence,
                                                    request,
                                                    opaqueCancellation)
                                                .thenApply(ignored ->
                                                    ZLinkAggregateCommitResult
                                                        .COMMITTED);
                                        });
                                });
                        });
                });
        });
    }

    private CompletionStage<ZLinkAggregateCommitResult>
        retryAggregateCommit(
            ZLinkAggregateFence fence,
            ZLinkStoreCancellation cancellation,
            int retryAttempt,
            Instant retryDeadline) {
        ZLinkStoreKey marker = aggregateKey(fence);
        var opaqueCancellation = adapt(cancellation);
        return provider.read(marker, opaqueCancellation).thenCompose(read -> {
            if (!(read instanceof ZLinkStoreReadFound found)) {
                return completed(ZLinkAggregateCommitResult.STALE);
            }
            PreparedAggregate current = decodeAggregate(found.value().bytes());
            if (current.state() == AGGREGATE_COMMITTED) {
                return aggregateInventory.load(
                        fence,
                        current.participantCount(),
                        current.inventoryDigest(),
                        opaqueCancellation)
                    .thenCompose(participants ->
                        normalizeAggregateParticipants(
                            fence,
                            current.request(participants),
                            opaqueCancellation))
                    .thenApply(ignored ->
                        ZLinkAggregateCommitResult.ALREADY_COMMITTED);
            }
            if (current.state() != AGGREGATE_PREPARED
                || retryAttempt >= AGGREGATE_COMMIT_RETRY_LIMIT
                || !Instant.now().isBefore(retryDeadline)
                || cancellation.isCancellationRequested()) {
                return completed(ZLinkAggregateCommitResult.STALE);
            }
            return delayAggregateCommitRetry(
                    retryAttempt,
                    retryDeadline,
                    cancellation)
                .thenCompose(ignored -> commitAggregate(
                    fence,
                    cancellation,
                    retryAttempt + 1,
                    retryDeadline));
        });
    }

    private static CompletionStage<Void> delayAggregateCommitRetry(
        int retryAttempt,
        Instant retryDeadline,
        ZLinkStoreCancellation cancellation) {
        long remainingMillis = Duration.between(
                Instant.now(), retryDeadline).toMillis();
        if (remainingMillis <= 0 || cancellation.isCancellationRequested()) {
            return completed(null);
        }
        long exponentialMillis = Math.min(
            100L,
            2L << Math.min(retryAttempt, 5));
        long jitterMillis = ThreadLocalRandom.current()
            .nextLong(exponentialMillis + 1L);
        long delayMillis = Math.min(
            remainingMillis,
            exponentialMillis + jitterMillis);
        return CompletableFuture.runAsync(
            () -> {},
            CompletableFuture.delayedExecutor(
                delayMillis,
                TimeUnit.MILLISECONDS));
    }

    CompletionStage<Optional<ZLinkAggregateProgressSnapshot>>
        readAggregateProgress(
            ZLinkAggregateFence fence,
            ZLinkStoreCancellation cancellation) {
        Objects.requireNonNull(fence, "fence");
        return provider.read(aggregateKey(fence), adapt(cancellation))
            .thenCompose(read -> {
                if (!(read instanceof ZLinkStoreReadFound found)) {
                    return completed(Optional.empty());
                }
                PreparedAggregate aggregate =
                    decodeAggregate(found.value().bytes());
                if (aggregate.state() != AGGREGATE_COMMITTED) {
                    return completed(Optional.empty());
                }
                return aggregateInventory.load(
                        fence,
                        aggregate.participantCount(),
                        aggregate.inventoryDigest(),
                        adapt(cancellation))
                    .thenApply(participants -> Optional.of(
                        new ZLinkAggregateProgressSnapshot(
                            fence,
                            found.value().version().value(),
                            aggregate.request(participants))));
            });
    }

    CompletionStage<Boolean> removeAggregateProgress(
        ZLinkAggregateFence fence,
        String expectedStoreVersion,
        ZLinkStoreCancellation cancellation) {
        Objects.requireNonNull(fence, "fence");
        Objects.requireNonNull(expectedStoreVersion, "expectedStoreVersion");
        ZLinkStoreKey marker = aggregateKey(fence);
        var opaqueCancellation = adapt(cancellation);
        return provider.read(marker, opaqueCancellation).thenCompose(read -> {
            if (!(read instanceof ZLinkStoreReadFound found)) {
                return aggregateInventory.delete(
                        fence,
                        opaqueCancellation)
                    .thenApply(ignored -> true);
            }
            if (!found.value().version().value().equals(
                    expectedStoreVersion)) {
                return completed(false);
            }
            PreparedAggregate aggregate =
                decodeAggregate(found.value().bytes());
            if (aggregate.state() != AGGREGATE_COMMITTED) {
                return completed(false);
            }
            List<ZLinkStoreCondition> conditions = new ArrayList<>();
            conditions.add(new ZLinkStoreVersionCondition(
                marker, found.value().version()));
            return requireLiveOwner(
                    aggregate.targetOwner(),
                    conditions,
                    opaqueCancellation)
                .thenCompose(live -> !live
                    ? completed(false)
                    : provider.write(
                            new ZLinkStoreWriteRequest(
                                conditions,
                                List.of(new ZLinkStoreDelete(marker))),
                                opaqueCancellation)
                        .thenCompose(result ->
                            result instanceof ZLinkStoreWriteApplied
                                ? aggregateInventory.delete(
                                        fence,
                                        opaqueCancellation)
                                    .thenApply(ignored -> true)
                                : completed(false)));
        });
    }

    CompletionStage<ZLinkAggregateAbortResult> abortAggregate(
        ZLinkAggregateFence fence,
        ZLinkStoreCancellation cancellation) {
        ZLinkStoreKey key = aggregateKey(fence);
        var opaqueCancellation = adapt(cancellation);
        return provider.read(key, opaqueCancellation).thenCompose(read -> {
            if (!(read instanceof ZLinkStoreReadFound found)) {
                return clearAggregateMarkersByScan(
                        fence,
                        opaqueCancellation)
                    .thenCompose(cleaned -> cleaned
                        ? aggregateInventory.delete(
                                fence,
                                opaqueCancellation)
                            .thenApply(ignored ->
                                ZLinkAggregateAbortResult.ALREADY_ABORTED)
                        : completed(ZLinkAggregateAbortResult.STALE));
            }
            PreparedAggregate aggregate = decodeAggregate(found.value().bytes());
            if (aggregate.state() == AGGREGATE_COMMITTED) {
                return completed(ZLinkAggregateAbortResult.STALE);
            }
            return provider.write(
                    new ZLinkStoreWriteRequest(
                        List.of(new ZLinkStoreVersionCondition(
                            key, found.value().version())),
                        List.of(new ZLinkStoreDelete(key))),
                    opaqueCancellation)
                .thenCompose(result -> {
                    if (!(result instanceof ZLinkStoreWriteApplied)) {
                        return completed(ZLinkAggregateAbortResult.STALE);
                    }
                    return clearAggregateMarkersByScan(
                            fence,
                            opaqueCancellation)
                        .thenCompose(cleaned -> cleaned
                            ? aggregateInventory.delete(
                                    fence,
                                    opaqueCancellation)
                                .thenApply(ignored ->
                                    ZLinkAggregateAbortResult.ABORTED)
                            : completed(ZLinkAggregateAbortResult.STALE));
                });
        });
    }

    CompletionStage<Long> removeAllByOwner(
        ZLinkLocationOwnerToken owner) {
        Objects.requireNonNull(owner, "owner");
        return removeAllByOwner(owner, null, 0L, 0);
    }

    private CompletionStage<Long> removeAllByOwner(
        ZLinkLocationOwnerToken owner,
        ZLinkStoreScanCursor cursor,
        long removed,
        int restartCount) {
        return provider.scan(
                new ZLinkStoreScanRequest(AUTHORITY_PREIMAGE_PREFIX, cursor, 1000),
                () -> false)
            .thenCompose(result -> {
                if (result instanceof ZLinkStoreScanExpired) {
                    if (restartCount >= 8) {
                        return failed(new IllegalStateException(
                            "authority cleanup scan repeatedly expired"));
                    }
                    return removeAllByOwner(owner, null, removed,
                        restartCount + 1);
                }
                ZLinkStoreScanPage page =
                    ((ZLinkStoreScanPageResult) result).value();
                List<ZLinkStoreCondition> conditions = new ArrayList<>();
                List<ZLinkStoreMutation> mutations = new ArrayList<>();
                for (ZLinkStoreScanItem item : page.items()) {
                    AuthorityRecord record = decode(item.value().bytes());
                    if (record.aggregate() == null
                        && record.ownerId().equals(owner.ownerId())
                        && record.ownerLeaseGeneration()
                            == owner.leaseGeneration()) {
                        conditions.add(new ZLinkStoreVersionCondition(
                            item.key(), item.value().version()));
                        mutations.add(new ZLinkStoreDelete(item.key()));
                    }
                }
                if (!mutations.isEmpty()) {
                    return provider.write(
                            new ZLinkStoreWriteRequest(
                                conditions, mutations),
                            () -> false)
                        .thenCompose(write -> {
                            if (write instanceof ZLinkStoreWriteApplied) {
                                return continueOwnerCleanupScan(
                                    owner,
                                    page.nextCursor(),
                                    Math.addExact(
                                        removed,
                                        mutations.size()),
                                    restartCount);
                            }
                            if (restartCount >= 8) {
                                return failed(new IllegalStateException(
                                    "authority cleanup write repeatedly conflicted"));
                            }
                            return removeAllByOwner(
                                owner, null, removed, restartCount + 1);
                        });
                }
                return continueOwnerCleanupScan(
                    owner, page.nextCursor(), removed, restartCount);
            });
    }

    private CompletionStage<Long> continueOwnerCleanupScan(
        ZLinkLocationOwnerToken owner,
        ZLinkStoreScanCursor cursor,
        long removed,
        int restartCount) {
        return cursor == null
            ? completed(removed)
            : removeAllByOwner(owner, cursor, removed, restartCount);
    }

    private CompletionStage<List<LoadedParticipant>> loadParticipants(
        List<ZLinkAggregateParticipant> participants,
        systems.zlink.framework.locationprovider.ZLinkStoreCancellation
            cancellation) {
        CompletionStage<List<LoadedParticipant>> loaded =
            completed(new ArrayList<>());
        for (ZLinkAggregateParticipant participant : participants) {
            loaded = loaded.thenCompose(rows -> provider.read(
                    authorityKey(participant.authorityKey()),
                    cancellation)
                .thenApply(read -> {
                    if (read instanceof ZLinkStoreReadFound found) {
                        rows.add(new LoadedParticipant(
                            authorityKey(participant.authorityKey()),
                            found.value()));
                    }
                    return rows;
                }));
        }
        return loaded.thenApply(List::copyOf);
    }

    private static boolean aggregateMarkersMatch(
        ZLinkAggregatePrepareRequest request,
        ZLinkAggregateFence fence,
        List<LoadedParticipant> rows) {
        for (int index = 0; index < rows.size(); index++) {
            ZLinkAggregateParticipant participant =
                request.participants().get(index);
            AuthorityRecord record = decode(rows.get(index).value().bytes());
            AggregateParticipantMarker marker = record.aggregate();
            if (marker == null
                || !marker.aggregateId().equals(fence.aggregateId())
                || marker.aggregateGeneration()
                    != fence.aggregateGeneration()
                || marker.index() != index
                || !marker.expectedStoreVersion().equals(
                    participant.expectedStoreVersion())
                || marker.ownerTransition() != participant.ownerTransition()
                || !Arrays.equals(
                    marker.authorityPayloadSha256(),
                    ZLinkAggregateInventoryStore.sha256(
                        participant.authorityPayload()))
                || !Arrays.equals(
                    marker.membershipMutationSha256(),
                    ZLinkAggregateInventoryStore.sha256(
                        participant.membershipMutation()))) {
                return false;
            }
        }
        return true;
    }

    private static boolean sameAggregateMarker(
        AggregateParticipantMarker marker,
        ZLinkAggregateFence fence,
        int index,
        ZLinkAggregateParticipant participant) {
        return marker != null
            && marker.aggregateId().equals(fence.aggregateId())
            && marker.aggregateGeneration() == fence.aggregateGeneration()
            && marker.index() == index
            && marker.expectedStoreVersion().equals(
                participant.expectedStoreVersion())
            && marker.ownerTransition() == participant.ownerTransition()
            && Arrays.equals(
                marker.authorityPayloadSha256(),
                ZLinkAggregateInventoryStore.sha256(
                    participant.authorityPayload()))
            && Arrays.equals(
                marker.membershipMutationSha256(),
                ZLinkAggregateInventoryStore.sha256(
                    participant.membershipMutation()));
    }

    private CompletionStage<Void> normalizeAggregateParticipants(
        ZLinkAggregateFence fence,
        ZLinkAggregatePrepareRequest request,
        systems.zlink.framework.locationprovider.ZLinkStoreCancellation
            cancellation) {
        CompletionStage<Void> chain = CompletableFuture.completedFuture(null);
        for (int index = 0; index < request.participants().size(); index++) {
            int participantIndex = index;
            chain = chain.thenCompose(ignored -> normalizeAggregateParticipant(
                fence,
                request,
                participantIndex,
                cancellation,
                0));
        }
        return chain;
    }

    private CompletionStage<Void> normalizeAggregateParticipant(
        ZLinkAggregateFence fence,
        ZLinkAggregatePrepareRequest request,
        int index,
        systems.zlink.framework.locationprovider.ZLinkStoreCancellation
            cancellation,
        int retry) {
        ZLinkAggregateParticipant participant = request.participants().get(index);
        ZLinkStoreKey rowKey = authorityKey(participant.authorityKey());
        return provider.read(rowKey, cancellation).thenCompose(read -> {
            if (!(read instanceof ZLinkStoreReadFound found)) {
                return failed(new IllegalStateException(
                    "aggregate participant authority is missing during normalization"));
            }
            AuthorityRecord current = decode(found.value().bytes());
            AggregateParticipantMarker marker = current.aggregate();
            if (marker == null) {
                return aggregateInventory.readParticipantPayload(
                        fence,
                        index,
                        cancellation)
                    .thenCompose(payload ->
                        isNormalizedAggregateParticipant(
                                current,
                                participant,
                                request,
                                payload)
                            ? completed(null)
                            : failed(new IllegalStateException(
                                "aggregate participant was normalized to an unexpected state")));
            }
            if (!marker.aggregateId().equals(fence.aggregateId())
                || marker.aggregateGeneration() != fence.aggregateGeneration()
                || marker.index() != index
                || !marker.expectedStoreVersion().equals(
                    participant.expectedStoreVersion())
                || marker.ownerTransition() != participant.ownerTransition()) {
                return failed(new IllegalStateException(
                    "aggregate participant fence changed during normalization"));
            }
            return aggregateInventory.readParticipantPayload(
                    fence,
                    index,
                    cancellation)
                .thenCompose(payload -> {
                    if (!Arrays.equals(
                            marker.authorityPayloadSha256(),
                            ZLinkAggregateInventoryStore.sha256(payload))) {
                        return failed(new IllegalStateException(
                            "aggregate participant payload checksum is invalid"));
                    }
                    boolean moves = marker.ownerTransition()
                        == ZLinkAuthorityGenerationTransition.NEW_OWNER;
                    ZLinkPlacementAllocation allocation = moves
                        ? new ZLinkPlacementAllocation(
                            ZLinkPlacementAllocationState.ACTIVE,
                            current.allocation().objectKind(),
                            current.allocation().stableType(),
                            request.targetDescriptor(),
                            request.targetDescriptorLifecycleGeneration(),
                            current.allocation().capacityBundle())
                        : current.allocation();
                    AuthorityRecord next = new AuthorityRecord(
                        payload,
                        current.objectGeneration(),
                        moves
                            ? marker.targetAuthorityOwnerGeneration()
                            : current.authorityOwnerGeneration(),
                        moves
                            ? request.targetOwner().ownerId()
                            : current.ownerId(),
                        moves
                            ? request.targetOwner().leaseGeneration()
                            : current.ownerLeaseGeneration(),
                        allocation,
                        Optional.empty());
                    return provider.write(
                            new ZLinkStoreWriteRequest(
                                List.of(new ZLinkStoreVersionCondition(
                                    rowKey,
                                    found.value().version())),
                                List.of(new ZLinkStorePut(
                                    rowKey,
                                    encode(next),
                                    null))),
                            cancellation)
                        .thenCompose(result -> result
                            instanceof ZLinkStoreWriteApplied
                            ? completed(null)
                            : retry >= 8
                                ? failed(new IllegalStateException(
                                    "aggregate participant normalization repeatedly conflicted"))
                                : normalizeAggregateParticipant(
                                    fence,
                                    request,
                                    index,
                                    cancellation,
                                    retry + 1));
                });
        });
    }

    private static boolean isNormalizedAggregateParticipant(
        AuthorityRecord current,
        ZLinkAggregateParticipant participant,
        ZLinkAggregatePrepareRequest request,
        byte[] payload) {
        if (!Arrays.equals(current.payload(), payload)
            || current.objectGeneration() != participant.objectGeneration()
            || current.allocation().state()
                != ZLinkPlacementAllocationState.ACTIVE
            || current.visibleStoreVersion() != null) {
            return false;
        }
        if (participant.ownerTransition()
            == ZLinkAuthorityGenerationTransition.NEW_OWNER) {
            return current.authorityOwnerGeneration()
                    > participant.sourceAuthorityOwnerGeneration()
                && current.ownerId().equals(request.targetOwner().ownerId())
                && current.ownerLeaseGeneration()
                    == request.targetOwner().leaseGeneration()
                && current.allocation().descriptor().equals(
                    request.targetDescriptor())
                && current.allocation().descriptorLifecycleGeneration()
                    == request.targetDescriptorLifecycleGeneration();
        }
        return current.authorityOwnerGeneration()
            == participant.sourceAuthorityOwnerGeneration();
    }

    private CompletionStage<Boolean> installAggregateMarkers(
        ZLinkAggregatePrepareRequest request,
        ZLinkAggregateFence fence,
        systems.zlink.framework.locationprovider.ZLinkStoreCancellation
            cancellation) {
        List<ZLinkStoreCondition> counterConditions = new ArrayList<>();
        int ownerChanges = Math.toIntExact(request.participants().stream()
            .filter(participant -> participant.ownerTransition()
                == ZLinkAuthorityGenerationTransition.NEW_OWNER)
            .count());
        return nextCounterRange(
                "authority-owner",
                ownerChanges,
                counterConditions,
                cancellation)
            .thenCompose(counter -> {
                CompletionStage<Boolean> chain = completed(true);
                List<ZLinkStoreKey> installed = new ArrayList<>();
                long[] nextOwnerGeneration = {counter.value()};
                boolean[] counterApplied = {false};
                for (int index = 0; index < request.participants().size(); index++) {
                    int participantIndex = index;
                    ZLinkAggregateParticipant participant =
                        request.participants().get(index);
                    chain = chain.thenCompose(ok -> {
                        if (!ok) {
                            return completed(false);
                        }
                        ZLinkStoreKey rowKey = authorityKey(
                            participant.authorityKey());
                        return provider.read(rowKey, cancellation)
                            .thenCompose(read -> {
                                if (!(read instanceof ZLinkStoreReadFound found)) {
                                    return completed(false);
                                }
                                AuthorityRecord current =
                                    decode(found.value().bytes());
                                List<ZLinkStoreCondition> conditions =
                                    new ArrayList<>();
                                List<ZLinkStoreMutation> mutations =
                                    new ArrayList<>();
                                ZLinkLocationOwnerToken requiredOwner =
                                    participant.ownerTransition()
                                        == ZLinkAuthorityGenerationTransition.PRESERVE
                                        ? new ZLinkLocationOwnerToken(
                                            current.ownerId(),
                                            current.ownerLeaseGeneration())
                                        : request.targetOwner();
                                return requireLiveOwner(
                                        requiredOwner,
                                        conditions,
                                        cancellation)
                                    .thenCompose(live -> {
                                        if (!live) {
                                            return completed(false);
                                        }
                                        if (current.aggregate() != null) {
                                            if (!sameAggregateMarker(
                                                    current.aggregate(),
                                                    fence,
                                                    participantIndex,
                                                    participant)) {
                                                return completed(false);
                                            }
                                            // This invocation did not write the
                                            // existing marker.  Keep it out of
                                            // the rollback set so a later
                                            // participant conflict cannot erase
                                            // another attempt's prepared work.
                                            return completed(true);
                                        }
                                        if (!found.value().version().value().equals(
                                                participant.expectedStoreVersion())
                                            || current.allocation().state()
                                                != ZLinkPlacementAllocationState.ACTIVE) {
                                            return completed(false);
                                        }
                                        long targetGeneration =
                                            participant.ownerTransition()
                                                == ZLinkAuthorityGenerationTransition.NEW_OWNER
                                                ? nextOwnerGeneration[0]++
                                                : current.authorityOwnerGeneration();
                                        AggregateParticipantMarker marker =
                                            new AggregateParticipantMarker(
                                                fence.aggregateId(),
                                                fence.aggregateGeneration(),
                                                participantIndex,
                                                participant.expectedStoreVersion(),
                                                participant.ownerTransition(),
                                                targetGeneration,
                                                ZLinkAggregateInventoryStore.sha256(
                                                    participant.authorityPayload()),
                                                ZLinkAggregateInventoryStore.sha256(
                                                    participant.membershipMutation()));
                                        if (!counterApplied[0]
                                            && participant.ownerTransition()
                                                == ZLinkAuthorityGenerationTransition.NEW_OWNER) {
                                            conditions.addAll(counterConditions);
                                            mutations.addAll(counter.mutations());
                                            counterApplied[0] = true;
                                        }
                                        conditions.add(new ZLinkStoreVersionCondition(
                                            rowKey,
                                            found.value().version()));
                                        mutations.add(new ZLinkStorePut(
                                            rowKey,
                                            encode(current.withAggregate(
                                                marker,
                                                current.visibleStoreVersion() == null
                                                    ? found.value().version().value()
                                                    : current.visibleStoreVersion())),
                                            null));
                                        return provider.write(
                                                new ZLinkStoreWriteRequest(
                                                    conditions,
                                                    mutations),
                                                cancellation)
                                            .thenApply(result -> {
                                                if (result instanceof ZLinkStoreWriteApplied) {
                                                    installed.add(rowKey);
                                                    return true;
                                                }
                                                return false;
                                            });
                                    });
                            });
                    });
                }
                return chain.thenCompose(ok -> ok
                    ? completed(true)
                    : clearAggregateMarkers(
                            fence,
                            installed,
                            cancellation)
                        .thenApply(ignored -> false));
            });
    }

    private CompletionStage<Boolean> clearAggregateMarkers(
        ZLinkAggregateFence fence,
        List<ZLinkStoreKey> installed,
        systems.zlink.framework.locationprovider.ZLinkStoreCancellation
            cancellation) {
        CompletionStage<Boolean> chain = completed(true);
        for (ZLinkStoreKey rowKey : installed) {
            chain = chain.thenCompose(ok -> provider.read(rowKey, cancellation)
                .thenCompose(read -> {
                    if (!(read instanceof ZLinkStoreReadFound found)) {
                        return completed(ok);
                    }
                    AuthorityRecord current = decode(found.value().bytes());
                    AggregateParticipantMarker marker = current.aggregate();
                    if (marker == null
                        || !marker.aggregateId().equals(fence.aggregateId())
                        || marker.aggregateGeneration()
                            != fence.aggregateGeneration()) {
                        return completed(ok);
                    }
                    return clearAggregateMarker(
                            fence,
                            rowKey,
                            found,
                            current,
                            cancellation)
                        .thenApply(cleared -> ok && cleared);
                }));
        }
        return chain;
    }

    private CompletionStage<Boolean> clearAggregateMarkersByScan(
        ZLinkAggregateFence fence,
        systems.zlink.framework.locationprovider.ZLinkStoreCancellation
            cancellation) {
        return clearAggregateMarkersByScan(
            fence, null, cancellation, 0);
    }

    private CompletionStage<Boolean> clearAggregateMarkersByScan(
        ZLinkAggregateFence fence,
        ZLinkStoreScanCursor cursor,
        systems.zlink.framework.locationprovider.ZLinkStoreCancellation
            cancellation,
        int restartCount) {
        return provider.scan(
                new ZLinkStoreScanRequest(AUTHORITY_PREIMAGE_PREFIX, cursor, 1000),
                cancellation)
            .thenCompose(result -> {
                if (result instanceof ZLinkStoreScanExpired) {
                    if (restartCount >= 8) {
                        return failed(new IllegalStateException(
                            "aggregate marker cleanup scan repeatedly expired"));
                    }
                    return clearAggregateMarkersByScan(
                        fence, null, cancellation, restartCount + 1);
                }
                ZLinkStoreScanPage page =
                    ((ZLinkStoreScanPageResult) result).value();
                CompletionStage<Boolean> cleared = completed(true);
                for (ZLinkStoreScanItem item : page.items()) {
                    AuthorityRecord record = decode(item.value().bytes());
                    AggregateParticipantMarker marker = record.aggregate();
                    if (marker == null
                        || !marker.aggregateId().equals(fence.aggregateId())
                        || marker.aggregateGeneration()
                            != fence.aggregateGeneration()) {
                        continue;
                    }
                    cleared = cleared.thenCompose(ok ->
                        clearAggregateMarker(
                                fence,
                                item.key(),
                                new ZLinkStoreReadFound(item.value()),
                                record,
                                cancellation)
                            .thenApply(value -> ok && value));
                }
                return cleared.thenCompose(ok ->
                    page.nextCursor() == null
                        ? completed(ok)
                        : clearAggregateMarkersByScan(
                            fence,
                            page.nextCursor(),
                            cancellation,
                            restartCount)
                            .thenApply(next -> ok && next));
            });
    }

    private CompletionStage<Boolean> clearAggregateMarker(
        ZLinkAggregateFence fence,
        ZLinkStoreKey rowKey,
        ZLinkStoreReadFound found,
        AuthorityRecord current,
        systems.zlink.framework.locationprovider.ZLinkStoreCancellation
            cancellation) {
        AggregateParticipantMarker marker = current.aggregate();
        if (marker == null
            || !marker.aggregateId().equals(fence.aggregateId())
            || marker.aggregateGeneration() != fence.aggregateGeneration()) {
            return completed(true);
        }
        return provider.write(
                new ZLinkStoreWriteRequest(
                    List.of(new ZLinkStoreVersionCondition(
                        rowKey,
                        found.value().version())),
                    List.of(new ZLinkStorePut(
                        rowKey,
                        encode(current.withAggregate(null, null)),
                        null))),
                cancellation)
            .thenApply(result -> result instanceof ZLinkStoreWriteApplied);
    }

    private CompletionStage<ZLinkAuthorityWriteResult> put(
        ZLinkStoreKey key,
        ZLinkStoreReadFound found,
        AuthorityRecord next,
        List<ZLinkStoreCondition> conditions,
        systems.zlink.framework.locationprovider.ZLinkStoreCancellation
            cancellation,
        ZLinkStoreReadResult current) {
        return writeAuthority(
            key,
            found,
            next,
            conditions,
            List.of(new ZLinkStorePut(key, encode(next), null)),
            cancellation,
            current);
    }

    private CompletionStage<ZLinkAuthorityWriteResult> writeAuthority(
        ZLinkStoreKey key,
        ZLinkStoreReadFound found,
        AuthorityRecord next,
        List<ZLinkStoreCondition> conditions,
        List<ZLinkStoreMutation> mutations,
        systems.zlink.framework.locationprovider.ZLinkStoreCancellation
            cancellation,
        ZLinkStoreReadResult current) {
        return provider.write(
                new ZLinkStoreWriteRequest(conditions, mutations),
                cancellation)
            .thenApply(result -> {
                if (!(result instanceof ZLinkStoreWriteApplied applied)) {
                    return new ZLinkAuthorityConflict(toRead(current));
                }
                ZLinkStoreVersion version =
                    applied.putVersions().get(key);
                return stored(next, version.value(), applied.storeNow());
            });
    }

    private CompletionStage<Boolean> requireLiveOwner(
        AuthorityRecord record,
        List<ZLinkStoreCondition> conditions,
        systems.zlink.framework.locationprovider.ZLinkStoreCancellation
            cancellation) {
        return requireLiveOwner(
            new ZLinkLocationOwnerToken(
                record.ownerId(), record.ownerLeaseGeneration()),
            conditions,
            cancellation);
    }

    private CompletionStage<Boolean> requireLiveOwner(
        ZLinkLocationOwnerToken owner,
        List<ZLinkStoreCondition> conditions,
        systems.zlink.framework.locationprovider.ZLinkStoreCancellation
            cancellation) {
        ZLinkStoreKey key = ownerKey(owner.ownerId());
        return provider.read(key, cancellation).thenApply(read -> {
            if (!(read instanceof ZLinkStoreReadFound found)
                || ownerGeneration(found.value().bytes())
                    != owner.leaseGeneration()) {
                return false;
            }
            conditions.add(new ZLinkStoreVersionCondition(
                key, found.value().version()));
            return true;
        });
    }

    private CompletionStage<Counters> nextPair(
        List<ZLinkStoreCondition> conditions,
        systems.zlink.framework.locationprovider.ZLinkStoreCancellation
            cancellation) {
        return nextCounter("object", conditions, cancellation)
            .thenCompose(object -> nextCounter(
                    "authority-owner", conditions, cancellation)
                .thenApply(owner -> new Counters(
                    object.value(),
                    owner.value(),
                    concat(object.mutations(), owner.mutations()))));
    }

    private CompletionStage<Counter> nextCounter(
        String name,
        List<ZLinkStoreCondition> conditions,
        systems.zlink.framework.locationprovider.ZLinkStoreCancellation
            cancellation) {
        ZLinkStoreKey key = new ZLinkStoreKey(COUNTER_PREFIX + name);
        return provider.read(key, cancellation).thenApply(read -> {
            long value = read instanceof ZLinkStoreReadFound found
                ? decodeLong(found.value().bytes())
                : 1L;
            if (value == Long.MAX_VALUE) {
                throw new IllegalStateException(
                    "Location Store generation is exhausted");
            }
            conditions.add(read instanceof ZLinkStoreReadFound found
                ? new ZLinkStoreVersionCondition(
                    key, found.value().version())
                : new ZLinkStoreMissingCondition(key));
            return new Counter(
                value,
                List.of(new ZLinkStorePut(
                    key, encodeLong(value + 1), null)));
        });
    }

    private CompletionStage<Counter> nextCounterRange(
        String name,
        int count,
        List<ZLinkStoreCondition> conditions,
        systems.zlink.framework.locationprovider.ZLinkStoreCancellation
            cancellation) {
        if (count == 0) {
            return completed(new Counter(0, List.of()));
        }
        ZLinkStoreKey key = new ZLinkStoreKey(COUNTER_PREFIX + name);
        return provider.read(key, cancellation).thenApply(read -> {
            long value = read instanceof ZLinkStoreReadFound found
                ? decodeLong(found.value().bytes())
                : 1L;
            long next = Math.addExact(value, count);
            conditions.add(read instanceof ZLinkStoreReadFound found
                ? new ZLinkStoreVersionCondition(
                    key, found.value().version())
                : new ZLinkStoreMissingCondition(key));
            return new Counter(
                value,
                List.of(new ZLinkStorePut(
                    key, encodeLong(next), null)));
        });
    }

    private static ZLinkAuthorityReadResult toRead(
        ZLinkStoreReadResult result) {
        return result instanceof ZLinkStoreReadMissing missing
            ? new ZLinkAuthorityMissing(missing.storeNow())
            : snapshot(((ZLinkStoreReadFound) result).value());
    }

    private CompletionStage<ZLinkAuthorityReadResult> projectRead(
        ZLinkStoreReadResult result,
        systems.zlink.framework.locationprovider.ZLinkStoreCancellation
            cancellation) {
        if (!(result instanceof ZLinkStoreReadFound found)) {
            return completed(toRead(result));
        }
        AuthorityRecord record = decode(found.value().bytes());
        AggregateParticipantMarker marker = record.aggregate();
        if (marker == null) {
            return completed(snapshot(found.value()));
        }
        ZLinkAggregateFence fence = new ZLinkAggregateFence(
            marker.aggregateId(),
            marker.aggregateGeneration());
        return provider.read(aggregateKey(fence), cancellation)
            .thenCompose(aggregateRead -> {
                if (!(aggregateRead instanceof ZLinkStoreReadFound aggregateFound)) {
                    return failed(new IllegalStateException(
                        "aggregate participant references a missing aggregate marker"));
                }
                PreparedAggregate aggregate =
                    decodeAggregate(aggregateFound.value().bytes());
                if (aggregate.state() != AGGREGATE_COMMITTED) {
                    return completed(snapshot(
                        record,
                        marker.expectedStoreVersion(),
                        found.value().storeNow()));
                }
                return aggregateInventory.readParticipantPayload(
                        fence,
                        marker.index(),
                        cancellation)
                    .thenApply(payload -> {
                        if (!Arrays.equals(
                                marker.authorityPayloadSha256(),
                                ZLinkAggregateInventoryStore.sha256(payload))) {
                            throw new IllegalStateException(
                                "committed aggregate participant payload checksum is invalid");
                        }
                        AuthorityRecord projected = marker.ownerTransition()
                            == ZLinkAuthorityGenerationTransition.NEW_OWNER
                            ? new AuthorityRecord(
                                payload,
                                record.objectGeneration(),
                                marker.targetAuthorityOwnerGeneration(),
                                aggregate.targetOwner().ownerId(),
                                aggregate.targetOwner().leaseGeneration(),
                                new ZLinkPlacementAllocation(
                                    ZLinkPlacementAllocationState.ACTIVE,
                                    record.allocation().objectKind(),
                                    record.allocation().stableType(),
                                    aggregate.targetDescriptor(),
                                    aggregate.targetDescriptorLifecycleGeneration(),
                                    record.allocation().capacityBundle()),
                                Optional.empty())
                            : new AuthorityRecord(
                                payload,
                                record.objectGeneration(),
                                record.authorityOwnerGeneration(),
                                record.ownerId(),
                                record.ownerLeaseGeneration(),
                                record.allocation(),
                                record.pendingCreation());
                        return snapshot(
                            projected,
                            found.value().version().value(),
                            found.value().storeNow());
                    });
            });
    }

    private static ZLinkAuthoritySnapshot snapshot(ZLinkStoreValue value) {
        AuthorityRecord record = decode(value.bytes());
        return snapshot(record, value.version().value(), value.storeNow());
    }

    private static ZLinkAuthoritySnapshot snapshot(
        AuthorityRecord record,
        String version,
        Instant storeNow) {
        return new ZLinkAuthoritySnapshot(
            version,
            record.payload(),
            record.objectGeneration(),
            record.authorityOwnerGeneration(),
            record.ownerId(),
            record.ownerLeaseGeneration(),
            record.allocation(),
            record.pendingCreation(),
            storeNow);
    }

    private static ZLinkAuthorityStored stored(
        AuthorityRecord record,
        String version,
        Instant storeNow) {
        return new ZLinkAuthorityStored(
            version,
            record.payload(),
            record.objectGeneration(),
            record.authorityOwnerGeneration(),
            record.ownerId(),
            record.ownerLeaseGeneration(),
            record.allocation(),
            storeNow);
    }

    private static boolean matches(
        AuthorityRecord current,
        ZLinkObjectReservation reservation) {
        return current.objectGeneration() == reservation.objectGeneration()
            && current.authorityOwnerGeneration()
                == reservation.authorityOwnerGeneration()
            && current.ownerId().equals(
                reservation.targetOwner().ownerId())
            && current.ownerLeaseGeneration()
                == reservation.targetOwner().leaseGeneration()
            && current.pendingCreation()
                .map(ZLinkPendingObjectCreation::reservationId)
                .filter(reservation.reservationVersion()::equals)
                .isPresent();
    }

    private static ZLinkPlacementAllocation withState(
        ZLinkPlacementAllocation value,
        ZLinkPlacementAllocationState state) {
        return new ZLinkPlacementAllocation(
            state,
            value.objectKind(),
            value.stableType(),
            value.descriptor(),
            value.descriptorLifecycleGeneration(),
            value.capacityBundle());
    }

    // --- Authority record canonical JSON (21-location-runtime.md#2.4) ---
    //
    // Top-level: {recordVersion:1, payload(base64), objectGeneration,
    // authorityOwnerGeneration, ownerId, ownerLeaseGeneration, allocation,
    // pendingCreation}. Except for `payload`, integer fields are JSON
    // strings (64-bit values). `providerExtension` is a java-private,
    // non-normative addition (permitted -- the spec's field table is "at
    // least" the listed fields) that carries the aggregate-transaction
    // marker and visible-store-version bookkeeping; it is present only
    // while an aggregate transaction is in flight, so a plain reserve/
    // commit record matches the golden fixture's field set exactly.

    private static byte[] encode(AuthorityRecord value) {
        ObjectNode root = CANONICAL_JSON.createObjectNode();
        root.put("recordVersion", 1);
        root.put(
            "payload",
            Base64.getEncoder().encodeToString(value.payload()));
        root.put(
            "objectGeneration", Long.toUnsignedString(value.objectGeneration()));
        root.put(
            "authorityOwnerGeneration",
            Long.toUnsignedString(value.authorityOwnerGeneration()));
        root.put("ownerId", value.ownerId());
        root.put(
            "ownerLeaseGeneration",
            Long.toUnsignedString(value.ownerLeaseGeneration()));
        root.set("allocation", encodeAllocation(value.allocation()));
        if (value.pendingCreation().isPresent()) {
            root.set(
                "pendingCreation",
                encodePendingCreation(value.pendingCreation().orElseThrow()));
        } else {
            root.putNull("pendingCreation");
        }
        if (value.aggregate() != null || value.visibleStoreVersion() != null) {
            root.set(
                "providerExtension",
                encodeExtension(value.aggregate(), value.visibleStoreVersion()));
        }
        try {
            return CANONICAL_JSON.writeValueAsBytes(root);
        } catch (JsonProcessingException error) {
            throw new IllegalStateException(
                "Failed to encode authority record", error);
        }
    }

    private static ObjectNode encodeAllocation(
        ZLinkPlacementAllocation allocation) {
        ObjectNode node = CANONICAL_JSON.createObjectNode();
        node.put(
            "state", allocationStateWire(allocation.state()));
        node.put(
            "objectKind", authorityObjectKindWire(allocation.objectKind()));
        node.put("stableType", allocation.stableType());
        ObjectNode descriptor = CANONICAL_JSON.createObjectNode();
        descriptor.put("meshName", allocation.descriptor().meshName());
        descriptor.put(
            "routingIdHex", allocation.descriptor().rid().toHex());
        node.set("descriptor", descriptor);
        node.put(
            "descriptorLifecycleGeneration",
            Long.toUnsignedString(allocation.descriptorLifecycleGeneration()));
        node.set(
            "capacity", encodeCapacityBundle(allocation.capacityBundle()));
        return node;
    }

    private static ObjectNode encodeCapacityBundle(
        ZLinkPlacementCapacityBundle bundle) {
        ObjectNode node = CANONICAL_JSON.createObjectNode();
        node.put("actors", bundle.actorSlots());
        node.put("spots", bundle.spotSlots());
        if (bundle.spotType().isPresent()) {
            ZLinkSpotTypeCapacityDelta delta = bundle.spotType().orElseThrow();
            ObjectNode spotType = CANONICAL_JSON.createObjectNode();
            spotType.put(
                "objectKind", authorityObjectKindWire(delta.objectKind()));
            spotType.put("stableType", delta.stableType());
            spotType.put("count", delta.slots());
            node.set("spotType", spotType);
        } else {
            node.putNull("spotType");
        }
        return node;
    }

    private static ObjectNode encodePendingCreation(
        ZLinkPendingObjectCreation pending) {
        ObjectNode node = CANONICAL_JSON.createObjectNode();
        node.put("reservationId", pending.reservationId());
        node.put(
            "requestContentReference", pending.requestContentReference());
        node.put(
            "requestSha256",
            HexFormat.of().formatHex(pending.requestSha256()));
        node.put("requestEncodedSize", pending.requestEncodedSize());
        return node;
    }

    private static ObjectNode encodeExtension(
        AggregateParticipantMarker aggregate,
        String visibleStoreVersion) {
        ObjectNode node = CANONICAL_JSON.createObjectNode();
        if (aggregate != null) {
            ObjectNode marker = CANONICAL_JSON.createObjectNode();
            marker.put(
                "aggregateIdMostSigBits",
                aggregate.aggregateId().getMostSignificantBits());
            marker.put(
                "aggregateIdLeastSigBits",
                aggregate.aggregateId().getLeastSignificantBits());
            marker.put(
                "aggregateGeneration",
                Long.toUnsignedString(aggregate.aggregateGeneration()));
            marker.put("index", aggregate.index());
            marker.put(
                "expectedStoreVersion", aggregate.expectedStoreVersion());
            marker.put(
                "ownerTransition", aggregate.ownerTransition().name());
            marker.put(
                "targetAuthorityOwnerGeneration",
                Long.toUnsignedString(aggregate.targetAuthorityOwnerGeneration()));
            marker.put(
                "authorityPayloadSha256",
                HexFormat.of().formatHex(
                    aggregate.authorityPayloadSha256()));
            marker.put(
                "membershipMutationSha256",
                HexFormat.of().formatHex(
                    aggregate.membershipMutationSha256()));
            node.set("aggregate", marker);
        } else {
            node.putNull("aggregate");
        }
        if (visibleStoreVersion != null) {
            node.put("visibleStoreVersion", visibleStoreVersion);
        } else {
            node.putNull("visibleStoreVersion");
        }
        return node;
    }

    private static String allocationStateWire(
        ZLinkPlacementAllocationState state) {
        return switch (state) {
            case PENDING -> "reserved";
            case ACTIVE -> "active";
        };
    }

    private static ZLinkPlacementAllocationState allocationStateFromWire(
        String value) {
        return switch (value) {
            case "reserved" -> ZLinkPlacementAllocationState.PENDING;
            case "active" -> ZLinkPlacementAllocationState.ACTIVE;
            default -> throw new IllegalStateException(
                "Unrecognized allocation state: " + value);
        };
    }

    private static String authorityObjectKindWire(
        ZLinkPlacementObjectKind kind) {
        return switch (kind) {
            case ACTOR -> "actor";
            case USER_SPOT -> "userSpot";
            case INSTANCE_SPOT -> "instanceSpot";
        };
    }

    private static ZLinkPlacementObjectKind authorityObjectKindFromWire(
        String value) {
        return switch (value) {
            case "actor" -> ZLinkPlacementObjectKind.ACTOR;
            case "userSpot" -> ZLinkPlacementObjectKind.USER_SPOT;
            case "instanceSpot" -> ZLinkPlacementObjectKind.INSTANCE_SPOT;
            default -> throw new IllegalStateException(
                "Unrecognized objectKind: " + value);
        };
    }

    private static byte[] encodeCapacity(CapacityRecord value) {
        try {
            var bytes = new ByteArrayOutputStream();
            var out = new DataOutputStream(bytes);
            out.writeInt(1);
            out.writeInt(value.actorActive());
            out.writeInt(value.actorPending());
            out.writeInt(value.spotActive());
            out.writeInt(value.spotPending());
            out.writeInt(value.types().size());
            for (Map.Entry<String, TypeCounter> entry : value.types()
                .entrySet()) {
                out.writeUTF(entry.getKey());
                out.writeInt(entry.getValue().active());
                out.writeInt(entry.getValue().pending());
            }
            out.flush();
            return bytes.toByteArray();
        } catch (IOException failure) {
            throw new IllegalStateException(failure);
        }
    }

    private static CapacityRecord decodeCapacity(byte[] bytes) {
        try {
            var in = new DataInputStream(new ByteArrayInputStream(bytes));
            if (in.readInt() != 1) {
                throw new IOException("unsupported capacity version");
            }
            int actorActive = in.readInt();
            int actorPending = in.readInt();
            int spotActive = in.readInt();
            int spotPending = in.readInt();
            int count = in.readInt();
            if (count < 0 || count > 4096) {
                throw new IOException("invalid capacity type count");
            }
            Map<String, TypeCounter> types = new HashMap<>();
            for (int index = 0; index < count; index++) {
                String key = in.readUTF();
                TypeCounter previous = types.put(
                    key,
                    new TypeCounter(in.readInt(), in.readInt()));
                if (previous != null) {
                    throw new IOException("duplicate capacity type");
                }
            }
            if (in.available() != 0) {
                throw new IOException("capacity record has trailing bytes");
            }
            return new CapacityRecord(
                actorActive,
                actorPending,
                spotActive,
                spotPending,
                types);
        } catch (IOException | RuntimeException failure) {
            throw new IllegalStateException(
                "Location Store capacity record is invalid", failure);
        }
    }

    private static AuthorityRecord decode(byte[] bytes) {
        try {
            JsonNode root = CANONICAL_JSON.readTree(bytes);
            if (root.path("recordVersion").asInt(-1) != 1) {
                throw new IllegalStateException(
                    "Location Store authority record has an unrecognized"
                        + " recordVersion");
            }
            byte[] payload = Base64.getDecoder().decode(
                root.path("payload").asText());
            long objectGeneration = Long.parseUnsignedLong(
                root.path("objectGeneration").asText());
            long ownerGeneration = Long.parseUnsignedLong(
                root.path("authorityOwnerGeneration").asText());
            String ownerId = root.path("ownerId").asText();
            long ownerLeaseGeneration = Long.parseUnsignedLong(
                root.path("ownerLeaseGeneration").asText());
            ZLinkPlacementAllocation allocation =
                decodeAllocation(root.path("allocation"));
            JsonNode pendingNode = root.path("pendingCreation");
            Optional<ZLinkPendingObjectCreation> pending =
                pendingNode.isMissingNode() || pendingNode.isNull()
                    ? Optional.empty()
                    : Optional.of(decodePendingCreation(pendingNode));
            AggregateParticipantMarker aggregate = null;
            String visibleStoreVersion = null;
            JsonNode extension = root.path("providerExtension");
            if (!extension.isMissingNode() && !extension.isNull()) {
                JsonNode markerNode = extension.path("aggregate");
                aggregate = markerNode.isMissingNode()
                        || markerNode.isNull()
                    ? null
                    : decodeAggregateMarker(markerNode);
                JsonNode visible = extension.path("visibleStoreVersion");
                visibleStoreVersion = visible.isMissingNode()
                        || visible.isNull()
                    ? null
                    : visible.asText();
            }
            return new AuthorityRecord(
                payload,
                objectGeneration,
                ownerGeneration,
                ownerId,
                ownerLeaseGeneration,
                allocation,
                pending,
                aggregate,
                visibleStoreVersion);
        } catch (IOException | RuntimeException failure) {
            throw new IllegalStateException(
                "Location Store authority record is invalid",
                failure);
        }
    }

    private static ZLinkPlacementAllocation decodeAllocation(JsonNode node) {
        JsonNode descriptor = node.path("descriptor");
        return new ZLinkPlacementAllocation(
            allocationStateFromWire(node.path("state").asText()),
            authorityObjectKindFromWire(node.path("objectKind").asText()),
            node.path("stableType").asText(),
            new ZLinkMeshNodeDescriptorKey(
                descriptor.path("meshName").asText(),
                RoutingId.fromHex(
                    descriptor.path("routingIdHex").asText())),
            Long.parseUnsignedLong(
                node.path("descriptorLifecycleGeneration").asText()),
            decodeCapacityBundle(node.path("capacity")));
    }

    private static ZLinkPlacementCapacityBundle decodeCapacityBundle(
        JsonNode node) {
        JsonNode spotTypeNode = node.path("spotType");
        Optional<ZLinkSpotTypeCapacityDelta> spotType =
            spotTypeNode.isMissingNode() || spotTypeNode.isNull()
                ? Optional.empty()
                : Optional.of(new ZLinkSpotTypeCapacityDelta(
                    authorityObjectKindFromWire(
                        spotTypeNode.path("objectKind").asText()),
                    spotTypeNode.path("stableType").asText(),
                    spotTypeNode.path("count").asInt()));
        return new ZLinkPlacementCapacityBundle(
            node.path("actors").asInt(),
            node.path("spots").asInt(),
            spotType);
    }

    private static ZLinkPendingObjectCreation decodePendingCreation(
        JsonNode node) {
        return new ZLinkPendingObjectCreation(
            node.path("reservationId").asText(),
            node.path("requestContentReference").asText(),
            HexFormat.of().parseHex(
                node.path("requestSha256").asText()),
            node.path("requestEncodedSize").asInt());
    }

    private static AggregateParticipantMarker decodeAggregateMarker(
        JsonNode node) {
        return new AggregateParticipantMarker(
            new UUID(
                node.path("aggregateIdMostSigBits").asLong(),
                node.path("aggregateIdLeastSigBits").asLong()),
            Long.parseUnsignedLong(node.path("aggregateGeneration").asText()),
            node.path("index").asInt(),
            node.path("expectedStoreVersion").asText(),
            ZLinkAuthorityGenerationTransition.valueOf(
                node.path("ownerTransition").asText()),
            Long.parseUnsignedLong(
                node.path("targetAuthorityOwnerGeneration").asText()),
            HexFormat.of().parseHex(
                node.path("authorityPayloadSha256").asText()),
            HexFormat.of().parseHex(
                node.path("membershipMutationSha256").asText()));
    }

    private static byte[] encodeAggregate(
        byte state,
        ZLinkAggregatePrepareRequest request) {
        try {
            var bytes = new ByteArrayOutputStream();
            var out = new DataOutputStream(bytes);
            out.writeByte(state);
            out.writeLong(request.aggregateId().getMostSignificantBits());
            out.writeLong(request.aggregateId().getLeastSignificantBits());
            out.writeLong(request.aggregateGeneration());
            out.writeUTF(request.targetDescriptor().meshName());
            out.writeUTF(request.targetDescriptor().rid().toHex());
            out.writeLong(request.targetDescriptorLifecycleGeneration());
            out.writeUTF(request.targetOwner().ownerId());
            out.writeLong(request.targetOwner().leaseGeneration());
            writeBytes(out, request.inventoryDigest());
            out.writeInt(request.capacityBundle().actorSlots());
            out.writeInt(request.capacityBundle().spotSlots());
            out.writeBoolean(request.capacityBundle().spotType().isPresent());
            if (request.capacityBundle().spotType().isPresent()) {
                ZLinkSpotTypeCapacityDelta delta =
                    request.capacityBundle().spotType().orElseThrow();
                out.writeInt(delta.objectKind().ordinal());
                out.writeUTF(delta.stableType());
                out.writeInt(delta.slots());
            }
            out.writeInt(request.participants().size());
            writeBytes(out, ZLinkAggregateInventoryStore.fingerprint(request));
            out.flush();
            return bytes.toByteArray();
        } catch (IOException failure) {
            throw new IllegalStateException(failure);
        }
    }

    private static PreparedAggregate decodeAggregate(byte[] bytes) {
        try {
            var in = new DataInputStream(new ByteArrayInputStream(bytes));
            byte state = in.readByte();
            if (state != AGGREGATE_STAGING
                && state != AGGREGATE_PREPARED
                && state != AGGREGATE_COMMITTED) {
                throw new IOException("aggregate marker state is invalid");
            }
            var id = new UUID(in.readLong(), in.readLong());
            long generation = in.readLong();
            var descriptor = new ZLinkMeshNodeDescriptorKey(
                in.readUTF(), RoutingId.fromHex(in.readUTF()));
            long descriptorGeneration = in.readLong();
            var owner = new ZLinkLocationOwnerToken(
                in.readUTF(), in.readLong());
            byte[] digest = readBytes(in);
            int actors = in.readInt();
            int spots = in.readInt();
            Optional<ZLinkSpotTypeCapacityDelta> spotType =
                in.readBoolean()
                    ? Optional.of(new ZLinkSpotTypeCapacityDelta(
                        ZLinkPlacementObjectKind.values()[in.readInt()],
                        in.readUTF(),
                        in.readInt()))
                    : Optional.empty();
            int count = in.readInt();
            if (count < 1) {
                throw new IOException("aggregate participant count is invalid");
            }
            byte[] fingerprint = readBytes(in);
            if (fingerprint.length != 32) {
                throw new IOException("aggregate request fingerprint is invalid");
            }
            if (in.available() != 0) {
                throw new IOException("aggregate marker has trailing bytes");
            }
            return new PreparedAggregate(
                state,
                id,
                generation,
                descriptor,
                descriptorGeneration,
                owner,
                digest,
                actors,
                spots,
                spotType,
                count,
                fingerprint);
        } catch (IOException | RuntimeException failure) {
            throw new IllegalStateException(
                "Location Store aggregate record is invalid",
                failure);
        }
    }

    private static void writeBytes(DataOutputStream out, byte[] bytes)
        throws IOException {
        out.writeInt(bytes.length);
        out.write(bytes);
    }

    private static byte[] readBytes(DataInputStream in) throws IOException {
        int length = in.readInt();
        if (length < 0 || length > 64 * 1024 * 1024) {
            throw new IOException("invalid byte length");
        }
        byte[] bytes = in.readNBytes(length);
        if (bytes.length != length) {
            throw new IOException("truncated byte value");
        }
        return bytes;
    }

    // Canonical cross-language logical key preimage
    // (21-location-runtime.md#2.4): "authority\0{actor|spot}\0{Id}". The
    // second segment collapses every Spot kind onto one "spot" string --
    // one Id has exactly one authority row regardless of Spot kind -- and
    // {Id} is the raw ActorId/SpotId, not the "zla1:..." wire form the rest
    // of this class carries as its authority-key contract value.
    private static final String AUTHORITY_PREIMAGE_PREFIX = "authority\0";

    private static ZLinkStoreKey authorityKey(String key) {
        ZLinkAuthorityKeyCodec.AuthorityIdentity identity =
            ZLinkAuthorityKeyCodec.decode(key);
        return new ZLinkStoreKey(
            AUTHORITY_PREIMAGE_PREFIX + identity.kind() + "\0"
                + identity.id());
    }

    private static ZLinkStoreKey capacityKey(
        ZLinkMeshNodeDescriptorKey descriptor,
        long lifecycleGeneration) {
        String identity = descriptor.meshName()
            + "\0" + descriptor.rid().toHex()
            + "\0" + lifecycleGeneration;
        return new ZLinkStoreKey(
            CAPACITY_PREFIX + HexFormat.of().formatHex(
                identity.getBytes(StandardCharsets.UTF_8)));
    }

    // Reverses authorityKey(): recovers the "zla1:..." authority-key
    // contract value from a scanned opaque record's logical key preimage.
    private static String decodeAuthorityKey(ZLinkStoreKey key) {
        String preimage = key.value();
        if (!preimage.startsWith(AUTHORITY_PREIMAGE_PREFIX)) {
            throw new IllegalStateException(
                "authority scan returned a non-authority logical key");
        }
        String rest = preimage.substring(AUTHORITY_PREIMAGE_PREFIX.length());
        int separator = rest.indexOf('\0');
        if (separator < 0) {
            throw new IllegalStateException(
                "authority logical key preimage is malformed");
        }
        String kind = rest.substring(0, separator);
        String id = rest.substring(separator + 1);
        return switch (kind) {
            case "actor" -> ZLinkAuthorityKeyCodec.actor(id);
            case "spot" -> ZLinkAuthorityKeyCodec.spot(id);
            default -> throw new IllegalStateException(
                "unrecognized authority key kind: " + kind);
        };
    }

    // Canonical cross-language logical key preimage
    // (21-location-runtime.md#2.4): "owner-lease\0{OwnerId}".
    private static ZLinkStoreKey ownerKey(String ownerId) {
        return ZLinkOwnerLeaseRecordCodec.key(ownerId);
    }

    private static ZLinkStoreKey aggregateKey(ZLinkAggregateFence fence) {
        return new ZLinkStoreKey(
            "zlink:v11:aggregate:" + fence.aggregateId()
                + ":" + fence.aggregateGeneration());
    }

    private static long ownerGeneration(byte[] bytes) {
        return ZLinkOwnerLeaseRecordCodec.decode(bytes).leaseGeneration();
    }

    // Counter rows are stored as UTF-8 decimal strings, matching the
    // cross-language owner-counter convention (dotnet/cpp/node parse
    // counters with a decimal-string reader; raw big-endian bytes are
    // unreadable to them).
    private static byte[] encodeLong(long value) {
        return Long.toString(value).getBytes(StandardCharsets.UTF_8);
    }

    private static long decodeLong(byte[] bytes) {
        try {
            return Long.parseUnsignedLong(
                new String(bytes, StandardCharsets.UTF_8));
        } catch (NumberFormatException failure) {
            throw new IllegalStateException(
                "Location Store counter is invalid", failure);
        }
    }

    private static systems.zlink.framework.locationprovider
        .ZLinkStoreCancellation adapt(ZLinkStoreCancellation cancellation) {
        Objects.requireNonNull(cancellation, "cancellation");
        return cancellation::isCancellationRequested;
    }

    private static void requireKey(String key) {
        if (key == null || key.isBlank()) {
            throw new IllegalArgumentException(
                "authority key must not be blank");
        }
    }

    private static <T> CompletionStage<T> completed(T value) {
        return CompletableFuture.completedFuture(value);
    }

    private static <T> CompletionStage<T> failed(Throwable failure) {
        return CompletableFuture.failedFuture(failure);
    }

    private static boolean sameAggregateRequest(
        PreparedAggregate prepared,
        ZLinkAggregatePrepareRequest request) {
        return prepared.aggregateId().equals(request.aggregateId())
            && prepared.aggregateGeneration() == request.aggregateGeneration()
            && prepared.targetDescriptor().equals(request.targetDescriptor())
            && prepared.targetDescriptorLifecycleGeneration()
                == request.targetDescriptorLifecycleGeneration()
            && prepared.targetOwner().equals(request.targetOwner())
            && prepared.capacityBundle().equals(request.capacityBundle())
            && prepared.participantCount() == request.participants().size()
            && Arrays.equals(
                prepared.inventoryDigest(),
                request.inventoryDigest())
            && Arrays.equals(
                prepared.requestFingerprint(),
                ZLinkAggregateInventoryStore.fingerprint(request));
    }

    private static List<ZLinkStoreMutation> concat(
        List<ZLinkStoreMutation> left,
        List<ZLinkStoreMutation> right) {
        List<ZLinkStoreMutation> result = new ArrayList<>(left);
        result.addAll(right);
        return result;
    }

    private record AuthorityRecord(
        byte[] payload,
        long objectGeneration,
        long authorityOwnerGeneration,
        String ownerId,
        long ownerLeaseGeneration,
        ZLinkPlacementAllocation allocation,
        Optional<ZLinkPendingObjectCreation> pendingCreation,
        AggregateParticipantMarker aggregate,
        String visibleStoreVersion) {
        AuthorityRecord(
            byte[] payload,
            long objectGeneration,
            long authorityOwnerGeneration,
            String ownerId,
            long ownerLeaseGeneration,
            ZLinkPlacementAllocation allocation,
            Optional<ZLinkPendingObjectCreation> pendingCreation) {
            this(
                payload,
                objectGeneration,
                authorityOwnerGeneration,
                ownerId,
                ownerLeaseGeneration,
                allocation,
                pendingCreation,
                null,
                null);
        }

        AuthorityRecord {
            payload = payload.clone();
        }

        AuthorityRecord withPayload(byte[] next) {
            return new AuthorityRecord(
                next,
                objectGeneration,
                authorityOwnerGeneration,
                ownerId,
                ownerLeaseGeneration,
                allocation,
                pendingCreation,
                aggregate,
                visibleStoreVersion);
        }

        AuthorityRecord withOwner(
            byte[] next,
            long generation,
            ZLinkLocationOwnerToken owner) {
            return new AuthorityRecord(
                next,
                objectGeneration,
                generation,
                owner.ownerId(),
                owner.leaseGeneration(),
                allocation,
                pendingCreation,
                aggregate,
                visibleStoreVersion);
        }

        AuthorityRecord withAggregate(
            AggregateParticipantMarker next,
            String nextVisibleStoreVersion) {
            return new AuthorityRecord(
                payload,
                objectGeneration,
                authorityOwnerGeneration,
                ownerId,
                ownerLeaseGeneration,
                allocation,
                pendingCreation,
                next,
                nextVisibleStoreVersion);
        }
    }

    private record AggregateParticipantMarker(
        UUID aggregateId,
        long aggregateGeneration,
        int index,
        String expectedStoreVersion,
        ZLinkAuthorityGenerationTransition ownerTransition,
        long targetAuthorityOwnerGeneration,
        byte[] authorityPayloadSha256,
        byte[] membershipMutationSha256) {
        AggregateParticipantMarker {
            authorityPayloadSha256 = authorityPayloadSha256.clone();
            membershipMutationSha256 = membershipMutationSha256.clone();
        }

        @Override public byte[] authorityPayloadSha256() {
            return authorityPayloadSha256.clone();
        }

        @Override public byte[] membershipMutationSha256() {
            return membershipMutationSha256.clone();
        }
    }

    private enum CapacityAdmission {
        UNAVAILABLE,
        EXHAUSTED,
        ACCEPTED
    }

    private record CapacityPlan(
        CapacityAdmission admission,
        ZLinkStoreKey key,
        CapacityRecord next) {}

    private record CapacitySnapshot(
        ZLinkStoreKey key,
        ZLinkStoreValue value,
        CapacityRecord record) {}

    private record TypeCounter(int active, int pending) {
        TypeCounter {
            if (active < 0 || pending < 0) {
                throw new IllegalArgumentException(
                    "capacity type counters must be non-negative");
            }
        }
    }

    private record CapacityRecord(
        int actorActive,
        int actorPending,
        int spotActive,
        int spotPending,
        Map<String, TypeCounter> types) {
        CapacityRecord {
            if (actorActive < 0
                || actorPending < 0
                || spotActive < 0
                || spotPending < 0) {
                throw new IllegalArgumentException(
                    "capacity counters must be non-negative");
            }
            types = Map.copyOf(Objects.requireNonNull(types, "types"));
        }

        static CapacityRecord empty() {
            return new CapacityRecord(0, 0, 0, 0, Map.of());
        }

        int typeCount(ZLinkSpotTypeCapacityDelta delta) {
            TypeCounter counter = types.get(typeKey(delta));
            return counter == null
                ? 0
                : counter.active() + counter.pending();
        }

        CapacityRecord adjustPending(
            ZLinkPlacementCapacityBundle bundle,
            int delta) {
            return adjust(bundle, delta, 0);
        }

        CapacityRecord adjustActive(
            ZLinkPlacementCapacityBundle bundle,
            int delta) {
            return adjust(bundle, 0, delta);
        }

        CapacityRecord transition(ZLinkPlacementCapacityBundle bundle) {
            CapacityRecord pending = adjustPending(bundle, -1);
            return pending == null
                ? null
                : pending.adjustActive(bundle, 1);
        }

        private CapacityRecord adjust(
            ZLinkPlacementCapacityBundle bundle,
            int pendingDelta,
            int activeDelta) {
            int nextActorActive = add(actorActive,
                bundle.actorSlots() * activeDelta);
            int nextActorPending = add(actorPending,
                bundle.actorSlots() * pendingDelta);
            int nextSpotActive = add(spotActive,
                bundle.spotSlots() * activeDelta);
            int nextSpotPending = add(spotPending,
                bundle.spotSlots() * pendingDelta);
            if (nextActorActive < 0
                || nextActorPending < 0
                || nextSpotActive < 0
                || nextSpotPending < 0) {
                return null;
            }
            Map<String, TypeCounter> nextTypes = new HashMap<>(types);
            if (bundle.spotType().isPresent()) {
                ZLinkSpotTypeCapacityDelta delta =
                    bundle.spotType().orElseThrow();
                String key = typeKey(delta);
                TypeCounter current = nextTypes.getOrDefault(
                    key, new TypeCounter(0, 0));
                int nextActive = add(
                    current.active(), delta.slots() * activeDelta);
                int nextPending = add(
                    current.pending(), delta.slots() * pendingDelta);
                if (nextActive < 0 || nextPending < 0) {
                    return null;
                }
                if (nextActive == 0 && nextPending == 0) {
                    nextTypes.remove(key);
                } else {
                    nextTypes.put(key, new TypeCounter(
                        nextActive, nextPending));
                }
            }
            return new CapacityRecord(
                nextActorActive,
                nextActorPending,
                nextSpotActive,
                nextSpotPending,
                nextTypes);
        }

        private static int add(int value, int delta) {
            try {
                return Math.addExact(value, delta);
            } catch (ArithmeticException overflow) {
                return -1;
            }
        }

        private static String typeKey(ZLinkSpotTypeCapacityDelta delta) {
            return delta.objectKind().value() + "\0" + delta.stableType();
        }
    }

    private record Counter(long value, List<ZLinkStoreMutation> mutations) {}
    private record Counters(
        long objectGeneration,
        long ownerGeneration,
        List<ZLinkStoreMutation> mutations) {}
    private record DecodedItem(String key, ZLinkStoreValue value) {}
    private record LoadedParticipant(
        ZLinkStoreKey key,
        ZLinkStoreValue value) {}
    private record PreparedAggregate(
        byte state,
        UUID aggregateId,
        long aggregateGeneration,
        ZLinkMeshNodeDescriptorKey targetDescriptor,
        long targetDescriptorLifecycleGeneration,
        ZLinkLocationOwnerToken targetOwner,
        byte[] inventoryDigest,
        int actors,
        int spots,
        Optional<ZLinkSpotTypeCapacityDelta> spotType,
        int participantCount,
        byte[] requestFingerprint) {
        PreparedAggregate {
            inventoryDigest = inventoryDigest.clone();
            requestFingerprint = requestFingerprint.clone();
        }

        ZLinkPlacementCapacityBundle capacityBundle() {
            return new ZLinkPlacementCapacityBundle(
                actors,
                spots,
                spotType);
        }

        ZLinkAggregatePrepareRequest request(
            List<ZLinkAggregateParticipant> participants) {
            if (participants.size() != participantCount) {
                throw new IllegalStateException(
                    "aggregate inventory participant count differs from marker");
            }
            ZLinkAggregatePrepareRequest request =
                new ZLinkAggregatePrepareRequest(
                    aggregateId,
                    aggregateGeneration,
                    participants,
                    inventoryDigest,
                    targetDescriptor,
                    targetDescriptorLifecycleGeneration,
                    capacityBundle(),
                    targetOwner);
            if (!Arrays.equals(
                    requestFingerprint,
                    ZLinkAggregateInventoryStore.fingerprint(request))) {
                throw new IllegalStateException(
                    "aggregate inventory request fingerprint differs from marker");
            }
            return request;
        }

        @Override public byte[] inventoryDigest() {
            return inventoryDigest.clone();
        }

        @Override public byte[] requestFingerprint() {
            return requestFingerprint.clone();
        }
    }

}
