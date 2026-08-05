package systems.zlink.framework.runtime.locations;

import java.nio.charset.StandardCharsets;
import java.time.Clock;
import java.time.Instant;
import java.util.ArrayList;
import java.util.Comparator;
import java.util.HashMap;
import java.util.HashSet;
import java.util.List;
import java.util.Map;
import java.util.Objects;
import java.util.Optional;
import java.util.Set;
import java.util.UUID;
import java.util.Arrays;
import java.util.function.Predicate;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import systems.zlink.framework.errors.ZLinkConfigurationException;
import systems.zlink.framework.locations.*;
import systems.zlink.framework.runtime.internal.locations.*;

final class ZLinkInMemoryAuthorityStore {
    private final Object gate;
    private final Clock clock;
    private final Predicate<ZLinkLocationOwnerToken> ownerLeaseIsLive;
    private final DescriptorLookup descriptorLookup;
    private final Predicate<String> spotIdentityClaimed;
    private final Map<String, Row> rows = new HashMap<>();
    private final Map<String, ReservationState> reservations = new HashMap<>();
    private final Map<ZLinkCreationOperationIdentity,
        ZLinkCreationOperationTerminal> creationTerminals = new HashMap<>();
    private final Map<String, CapacityState> capacityReservations =
        new HashMap<>();
    private final Map<UUID, AggregateState> aggregates = new HashMap<>();
    private final Map<String, byte[]> membershipMutations =
        new HashMap<>();
    private final Map<AllocationCounterKey, CapacityCounter>
        actorAllocationCounters = new HashMap<>();
    private final Map<AllocationCounterKey, CapacityCounter>
        spotAllocationCounters = new HashMap<>();
    private final Map<TypeAllocationCounterKey, CapacityCounter>
        typeAllocationCounters = new HashMap<>();
    private long revision;
    private long objectGeneration;
    private long authorityOwnerGeneration;

    ZLinkInMemoryAuthorityStore(
        Clock clock,
        Predicate<ZLinkLocationOwnerToken> ownerLeaseIsLive) {
        this(
            new Object(),
            clock,
            ownerLeaseIsLive,
            (key, generation, owner) -> null,
            ignored -> false);
    }

    ZLinkInMemoryAuthorityStore(
        Clock clock,
        Predicate<ZLinkLocationOwnerToken> ownerLeaseIsLive,
        DescriptorLookup descriptorLookup) {
        this(
            new Object(),
            clock,
            ownerLeaseIsLive,
            descriptorLookup,
            ignored -> false);
    }

    ZLinkInMemoryAuthorityStore(
        Object gate,
        Clock clock,
        Predicate<ZLinkLocationOwnerToken> ownerLeaseIsLive,
        DescriptorLookup descriptorLookup,
        Predicate<String> spotIdentityClaimed) {
        this.gate = gate;
        this.clock = clock;
        this.ownerLeaseIsLive = ownerLeaseIsLive;
        this.descriptorLookup = descriptorLookup;
        this.spotIdentityClaimed = spotIdentityClaimed;
    }

    boolean containsAuthority(String key) {
        synchronized (gate) {
            return rows.containsKey(key);
        }
    }

    public CompletionStage<ZLinkAuthorityReadResult> read(
        String key,
        ZLinkStoreCancellation cancellation) {
        synchronized (gate) {
            Instant now = clock.instant();
            Row row = rows.get(key);
            return completed(row == null
                ? new ZLinkAuthorityMissing(now)
                : snapshot(row, now));
        }
    }

    public CompletionStage<ZLinkAuthorityWriteResult> compareExchange(
        String key,
        ZLinkAuthorityExpectation expectation,
        ZLinkAuthorityMutation mutation,
        ZLinkStoreCancellation cancellation) {
        synchronized (gate) {
            Instant now = clock.instant();
            Row current = rows.get(key);
            if (!matches(current, expectation)) {
                return completed(new ZLinkAuthorityConflict(
                    current == null
                        ? new ZLinkAuthorityMissing(now)
                        : snapshot(current, now)));
            }
            if (mutation instanceof ZLinkAuthorityDelete) {
                if (current == null
                    || current.allocation.state()
                        != ZLinkPlacementAllocationState.ACTIVE) {
                    return completed(new ZLinkAuthorityConflict(
                        current == null
                            ? new ZLinkAuthorityMissing(now)
                            : snapshot(current, now)));
                }
                if (!ownerLeaseIsLive.test(current.owner)) {
                    return completed(new ZLinkAuthorityConflict(
                        snapshot(current, now)));
                }
                adjustActive(current.allocation, current.allocation.capacityBundle(), -1);
                rows.remove(key);
                return completed(new ZLinkAuthorityDeleted(
                    nextVersion(),
                    now));
            }
            if (mutation instanceof systems.zlink.framework.runtime.internal.locations.ZLinkAuthorityRestore restore) {
                if (current == null
                    || current.allocation.state()
                        != ZLinkPlacementAllocationState.ACTIVE
                    || !current.owner.equals(restore.expectedOwner())) {
                    return completed(new ZLinkAuthorityConflict(
                        current == null
                            ? new ZLinkAuthorityMissing(now)
                            : snapshot(current, now)));
                }
                if (revision == Long.MAX_VALUE) {
                    return completed(new ZLinkAuthorityGenerationExhausted());
                }
                Row stored = new Row(
                    nextVersion(),
                    restore.payload(),
                    current.objectGeneration,
                    current.authorityOwnerGeneration,
                    current.owner,
                    current.allocation);
                rows.put(key, stored);
                return completed(stored(stored, now));
            }
            ZLinkAuthorityPut put = (ZLinkAuthorityPut) mutation;
            if (current == null
                || current.allocation.state()
                    != ZLinkPlacementAllocationState.ACTIVE) {
                return completed(new ZLinkAuthorityConflict(
                    current == null
                        ? new ZLinkAuthorityMissing(now)
                        : snapshot(current, now)));
            }
            ZLinkLocationOwnerToken targetOwner = put.targetOwner()
                .orElse(current.owner);
            if (targetOwner == null
                || !ownerLeaseIsLive.test(targetOwner)) {
                return completed(new ZLinkAuthorityConflict(
                    snapshot(current, now)));
            }
            boolean changesOwner = put.generationTransition()
                == ZLinkAuthorityGenerationTransition.NEW_OWNER;
            boolean relocatesOwner = put.generationTransition()
                == ZLinkAuthorityGenerationTransition.NEW_OWNER;
            CapacityState capacity = put.relocationCapacityFence()
                .map(fence -> capacityReservations.get(fence.value()))
                .orElse(null);
            if (relocatesOwner
                && (capacity == null
                    || capacity.state != State.RESERVED
                    || capacity.boundAggregateId != null
                    || !capacity.request.authorityKey().equals(key)
                    || !capacity.request.expectedStoreVersion().equals(
                        current.storeVersion)
                    || !capacity.request.sourceOwner().equals(current.owner)
                    || !sourceAllocationMatches(
                        current.allocation,
                        capacity.request)
                    || !targetDescriptorIsCurrent(
                        capacity.request)
                    || !capacity.request.targetOwner().equals(targetOwner))) {
                return completed(new ZLinkAuthorityConflict(
                    snapshot(current, now)));
            }
            if ((changesOwner
                    && authorityOwnerGeneration == Long.MAX_VALUE)
                || revision == Long.MAX_VALUE) {
                return completed(new ZLinkAuthorityGenerationExhausted());
            }
            long storedOwnerGeneration = changesOwner
                ? ++authorityOwnerGeneration
                : current.authorityOwnerGeneration;
            ZLinkPlacementAllocation storedAllocation = capacity == null
                ? current.allocation
                : activeTargetAllocation(capacity.request);
            Row stored = new Row(
                nextVersion(),
                put.payload(),
                current.objectGeneration,
                storedOwnerGeneration,
                targetOwner,
                storedAllocation);
            rows.put(key, stored);
            if (capacity != null) {
                relocateAllocation(
                    current.allocation,
                    storedAllocation);
                capacity.state = State.COMMITTED;
            }
            return completed(stored(stored, now));
        }
    }

    public CompletionStage<ZLinkAuthorityScanResult> list(
        String prefix,
        Optional<ZLinkAuthorityScanCursor> cursor,
        int limit,
        ZLinkStoreCancellation cancellation) {
        synchronized (gate) {
            if (limit <= 0) {
                throw new IllegalArgumentException(
                    "authority scan limit must be positive");
            }
            int offset = cursor.map(value -> Integer.parseInt(value.encoded()))
                .orElse(0);
            List<Map.Entry<String, Row>> ordered = rows.entrySet().stream()
                .filter(entry -> entry.getKey().startsWith(prefix))
                .sorted(Map.Entry.comparingByKey())
                .toList();
            List<ZLinkAuthorityEntry> items = new ArrayList<>();
            Instant now = clock.instant();
            for (int index = offset;
                 index < ordered.size() && items.size() < limit;
                 index++) {
                var entry = ordered.get(index);
                items.add(new ZLinkAuthorityEntry(
                    entry.getKey(),
                    snapshot(entry.getValue(), now)));
            }
            int next = offset + items.size();
            return completed(new ZLinkAuthorityPage(
                items,
                next < ordered.size()
                    ? Optional.of(new ZLinkAuthorityScanCursor(
                        Integer.toString(next)))
                    : Optional.empty()));
        }
    }

    public CompletionStage<ZLinkObjectReserveResult> reserve(
        ZLinkObjectReservationRequest request,
        ZLinkStoreCancellation cancellation) {
        synchronized (gate) {
            if (request.objectKind()
                    != ZLinkPlacementObjectKind.ACTOR
                && spotIdentityClaimed.test(
                    request.authorityKey())) {
                return completed(new ZLinkObjectConflict(
                    new ZLinkAuthorityMissing(clock.instant())));
            }
            Instant now = clock.instant();
            Row current = rows.get(request.authorityKey());
            if (current != null) {
                if (!current.allocation.stableType()
                    .equals(request.stableType())) {
                    return completed(new ZLinkObjectTypeMismatch(
                        snapshot(current, now)));
                }
                if (current.allocation.state()
                    == ZLinkPlacementAllocationState.PENDING) {
                    return completed(new ZLinkObjectConflict(
                        snapshot(current, now)));
                }
                return completed(new ZLinkObjectAlreadyExists(
                    snapshot(current, now)));
            }
            if (!ownerLeaseIsLive.test(request.targetOwner())) {
                return completed(new ZLinkObjectConflict(
                    new ZLinkAuthorityMissing(now)));
            }
            DescriptorAdmission admission = descriptorAdmission(
                request.targetDescriptor(),
                request.targetDescriptorLifecycleGeneration(),
                request.targetOwner(),
                request.objectKind(),
                request.stableType(),
                request.capacityBundle());
            if (admission == DescriptorAdmission.UNAVAILABLE) {
                return completed(new ZLinkObjectConflict(
                    new ZLinkAuthorityMissing(now)));
            }
            if (admission == DescriptorAdmission.CAPACITY_EXHAUSTED) {
                return completed(new ZLinkPlacementCapacityExhausted());
            }
            if (objectGeneration == Long.MAX_VALUE
                || authorityOwnerGeneration == Long.MAX_VALUE
                || revision == Long.MAX_VALUE) {
                return completed(new ZLinkObjectGenerationExhausted());
            }
            long nextGeneration = ++objectGeneration;
            long nextOwnerGeneration = ++authorityOwnerGeneration;
            String storeVersion = nextVersion();
            String reservationVersion = UUID.randomUUID().toString();
            ZLinkObjectReservation reservation = new ZLinkObjectReservation(
                request.authorityKey(),
                storeVersion,
                nextGeneration,
                nextOwnerGeneration,
                reservationVersion,
                request.targetDescriptor(),
                request.targetDescriptorLifecycleGeneration(),
                request.targetOwner());
            rows.put(
                request.authorityKey(),
                new Row(
                    storeVersion,
                    request.creatingPayload(),
                    nextGeneration,
                    nextOwnerGeneration,
                    request.targetOwner(),
                    new ZLinkPlacementAllocation(
                        ZLinkPlacementAllocationState.PENDING,
                        request.objectKind(),
                        request.stableType(),
                        request.targetDescriptor(),
                        request.targetDescriptorLifecycleGeneration(),
                        request.capacityBundle())));
            adjustPending(
                rows.get(request.authorityKey()).allocation,
                request.capacityBundle(),
                1);
            reservations.put(
                request.authorityKey(),
                new ReservationState(
                    reservation,
                    request,
                    State.PREPARED));
            return completed(new ZLinkObjectReserved(reservation));
        }
    }

    public CompletionStage<ZLinkObjectCommitResult> commit(
        ZLinkObjectReservation reservation,
        byte[] readyPayload,
        ZLinkStoreCancellation cancellation) {
        synchronized (gate) {
            return completed(commitLocked(reservation, readyPayload, null));
        }
    }

    public CompletionStage<ZLinkObjectCommitResult> commit(
        ZLinkObjectReservation reservation,
        byte[] readyPayload,
        ZLinkCreationOperationTerminal terminal,
        ZLinkStoreCancellation cancellation) {
        requireTerminal(
            reservation,
            terminal,
            ZLinkCreationTerminalState.CREATED);
        synchronized (gate) {
            return completed(commitLocked(reservation, readyPayload, terminal));
        }
    }

    private ZLinkObjectCommitResult commitLocked(
        ZLinkObjectReservation reservation,
        byte[] readyPayload,
        ZLinkCreationOperationTerminal terminal) {
            if (terminal != null
                && activeTerminal(terminal.operation()) != null) {
                return terminalMatches(terminal)
                    ? ZLinkObjectCommitResult.ALREADY_COMMITTED
                    : ZLinkObjectCommitResult.STALE;
            }
            ReservationState state = reservations.get(
                reservation.authorityKey());
            if (!sameReservation(state, reservation)) {
                return terminalMatches(terminal)
                    ? ZLinkObjectCommitResult.ALREADY_COMMITTED
                    : ZLinkObjectCommitResult.STALE;
            }
            if (state.state == State.COMMITTED) {
                return terminal == null || terminalMatches(terminal)
                    ? ZLinkObjectCommitResult.ALREADY_COMMITTED
                    : ZLinkObjectCommitResult.STALE;
            }
            if (state.state == State.ABORTED) {
                return ZLinkObjectCommitResult.STALE;
            }
            if (!ownerLeaseIsLive.test(reservation.targetOwner())) {
                return ZLinkObjectCommitResult.STALE;
            }
            Row current = rows.get(reservation.authorityKey());
            if (!pendingReservationMatches(current, state.reservation)) {
                return ZLinkObjectCommitResult.STALE;
            }
            if (!descriptorIsCurrent(
                    reservation.targetDescriptor(),
                    reservation.targetDescriptorLifecycleGeneration(),
                    reservation.targetOwner(),
                    current.allocation.objectKind(),
                    current.allocation.stableType())) {
                return ZLinkObjectCommitResult.STALE;
            }
            if (!hasCounterRoom(revision, 1)) {
                return ZLinkObjectCommitResult.GENERATION_EXHAUSTED;
            }
            rows.put(
                reservation.authorityKey(),
                current.withPayloadAndAllocation(
                    nextVersion(),
                    readyPayload,
                    withAllocationState(
                        current.allocation,
                        ZLinkPlacementAllocationState.ACTIVE)));
            activateAllocation(current.allocation);
            state.state = State.COMMITTED;
            if (terminal != null) {
                creationTerminals.put(terminal.operation(), terminal);
            }
            return ZLinkObjectCommitResult.COMMITTED;
    }

    public CompletionStage<ZLinkObjectRejectResult> reject(
        ZLinkObjectReservation reservation,
        ZLinkCreationOperationTerminal terminal,
        ZLinkStoreCancellation cancellation) {
        requireTerminal(
            reservation,
            terminal,
            ZLinkCreationTerminalState.REJECTED);
        synchronized (gate) {
            if (activeTerminal(terminal.operation()) != null) {
                return completed(terminalMatches(terminal)
                    ? ZLinkObjectRejectResult.ALREADY_REJECTED
                    : ZLinkObjectRejectResult.STALE);
            }
            ReservationState state = reservations.get(
                reservation.authorityKey());
            if (!sameReservation(state, reservation)) {
                return completed(terminalMatches(terminal)
                    ? ZLinkObjectRejectResult.ALREADY_REJECTED
                    : ZLinkObjectRejectResult.STALE);
            }
            if (state.state != State.PREPARED) {
                return completed(ZLinkObjectRejectResult.STALE);
            }
            Row current = rows.get(reservation.authorityKey());
            if (!pendingReservationMatches(current, state.reservation)) {
                return completed(ZLinkObjectRejectResult.STALE);
            }
            if (!hasCounterRoom(revision, 1)) {
                return completed(
                    ZLinkObjectRejectResult.GENERATION_EXHAUSTED);
            }
            rows.remove(reservation.authorityKey());
            adjustPending(
                current.allocation,
                current.allocation.capacityBundle(),
                -1);
            state.state = State.ABORTED;
            revision++;
            creationTerminals.put(terminal.operation(), terminal);
            return completed(ZLinkObjectRejectResult.REJECTED);
        }
    }

    public CompletionStage<ZLinkObjectAbortResult> abort(
        ZLinkObjectReservation reservation,
        ZLinkStoreCancellation cancellation) {
        synchronized (gate) {
            ReservationState state = reservations.get(
                reservation.authorityKey());
            if (!sameReservation(state, reservation)) {
                return completed(ZLinkObjectAbortResult.STALE);
            }
            if (state.state == State.ABORTED) {
                return completed(ZLinkObjectAbortResult.ALREADY_ABORTED);
            }
            if (state.state == State.COMMITTED) {
                return completed(ZLinkObjectAbortResult.STALE);
            }
            Row current = rows.get(reservation.authorityKey());
            if (!pendingReservationMatches(current, state.reservation)) {
                return completed(ZLinkObjectAbortResult.STALE);
            }
            rows.remove(reservation.authorityKey());
            adjustPending(
                current.allocation,
                current.allocation.capacityBundle(),
                -1);
            state.state = State.ABORTED;
            return completed(ZLinkObjectAbortResult.ABORTED);
        }
    }

    public CompletionStage<ZLinkObjectAbortResult> abort(
        ZLinkObjectReservation reservation,
        ZLinkCreationOperationTerminal terminal,
        ZLinkStoreCancellation cancellation) {
        requireTerminal(
            reservation,
            terminal,
            ZLinkCreationTerminalState.FAILED);
        synchronized (gate) {
            if (activeTerminal(terminal.operation()) != null) {
                return completed(terminalMatches(terminal)
                    ? ZLinkObjectAbortResult.ALREADY_ABORTED
                    : ZLinkObjectAbortResult.STALE);
            }
            ReservationState state = reservations.get(
                reservation.authorityKey());
            if (!sameReservation(state, reservation)) {
                return completed(terminalMatches(terminal)
                    ? ZLinkObjectAbortResult.ALREADY_ABORTED
                    : ZLinkObjectAbortResult.STALE);
            }
            if (state.state == State.ABORTED) {
                return completed(ZLinkObjectAbortResult.ALREADY_ABORTED);
            }
            if (state.state == State.COMMITTED) {
                return completed(ZLinkObjectAbortResult.STALE);
            }
            Row current = rows.get(reservation.authorityKey());
            if (!pendingReservationMatches(current, state.reservation)) {
                return completed(ZLinkObjectAbortResult.STALE);
            }
            rows.remove(reservation.authorityKey());
            adjustPending(
                current.allocation,
                current.allocation.capacityBundle(),
                -1);
            state.state = State.ABORTED;
            creationTerminals.put(terminal.operation(), terminal);
            return completed(ZLinkObjectAbortResult.ABORTED);
        }
    }

    public CompletionStage<ZLinkCreationTerminalReadResult>
        readCreationTerminal(
            ZLinkCreationOperationIdentity operation,
            ZLinkStoreCancellation cancellation) {
        synchronized (gate) {
            ZLinkCreationOperationTerminal terminal =
                activeTerminal(operation);
            if (terminal == null) {
                return completed(new ZLinkCreationTerminalMissing());
            }
            return completed(new ZLinkCreationTerminalFound(terminal));
        }
    }

    private boolean terminalMatches(
        ZLinkCreationOperationTerminal terminal) {
        if (terminal == null) {
            return false;
        }
        ZLinkCreationOperationTerminal stored =
            activeTerminal(terminal.operation());
        return stored != null
            && stored.operation().equals(terminal.operation())
            && stored.reservation().equals(terminal.reservation())
            && stored.state() == terminal.state()
            && Arrays.equals(
                stored.terminalEnvelope(),
                terminal.terminalEnvelope())
            && Arrays.equals(
                stored.terminalSha256(),
                terminal.terminalSha256())
            && stored.expiresAt().equals(terminal.expiresAt());
    }

    private ZLinkCreationOperationTerminal activeTerminal(
        ZLinkCreationOperationIdentity operation) {
        ZLinkCreationOperationTerminal terminal =
            creationTerminals.get(operation);
        if (terminal != null
            && !terminal.expiresAt().isAfter(clock.instant())) {
            creationTerminals.remove(operation);
            return null;
        }
        return terminal;
    }

    private void requireTerminal(
        ZLinkObjectReservation reservation,
        ZLinkCreationOperationTerminal terminal,
        ZLinkCreationTerminalState expectedState) {
        java.util.Objects.requireNonNull(terminal, "terminal");
        if (!reservation.equals(terminal.reservation())) {
            throw new IllegalArgumentException(
                "terminal reservation must match the exact reservation");
        }
        if (terminal.state() != expectedState) {
            throw new IllegalArgumentException(
                "terminal state must be " + expectedState);
        }
        if (!terminal.expiresAt().isAfter(clock.instant())) {
            throw new IllegalArgumentException(
                "terminal expiresAt must be later than provider store time");
        }
        byte[] computed;
        try {
            computed = java.security.MessageDigest
                .getInstance("SHA-256")
                .digest(terminal.terminalEnvelope());
        } catch (java.security.NoSuchAlgorithmException impossible) {
            throw new IllegalStateException(impossible);
        }
        if (!Arrays.equals(computed, terminal.terminalSha256())) {
            throw new IllegalArgumentException(
                "terminalSha256 does not match terminalEnvelope");
        }
    }

    public CompletionStage<ZLinkRelocationCapacityReserveResult>
        reserveRelocationCapacity(
            ZLinkRelocationCapacityReservationRequest request,
            ZLinkStoreCancellation cancellation) {
        synchronized (gate) {
            Instant now = clock.instant();
            Row current = rows.get(request.authorityKey());
            String fenceValue = request.reservationId().toString();
            CapacityState existing = capacityReservations.get(fenceValue);
            if (existing != null) {
                return completed(
                    existing.request.equals(request)
                        ? new ZLinkRelocationCapacityAlreadyReserved(
                            new ZLinkRelocationCapacityFence(fenceValue))
                        : new ZLinkRelocationCapacityConflict(
                            current == null
                                ? new ZLinkAuthorityMissing(now)
                                : snapshot(current, now)));
            }
            if (current == null
                || !current.storeVersion.equals(
                    request.expectedStoreVersion())) {
                return completed(new ZLinkRelocationCapacityConflict(
                    current == null
                        ? new ZLinkAuthorityMissing(now)
                        : snapshot(current, now)));
            }
            if (current.allocation.state()
                    != ZLinkPlacementAllocationState.ACTIVE
                || !current.owner.equals(request.sourceOwner())
                || !sourceAllocationMatches(
                    current.allocation,
                    request)) {
                return completed(new ZLinkRelocationCapacityConflict(
                    snapshot(current, now)));
            }
            if (!ownerLeaseIsLive.test(request.targetOwner())) {
                return completed(
                    new ZLinkRelocationCapacityTargetUnavailable());
            }
            DescriptorAdmission admission = descriptorAdmission(
                request.targetDescriptor(),
                request.targetDescriptorLifecycleGeneration(),
                request.targetOwner(),
                request.objectKind(),
                request.stableType(),
                request.capacityBundle());
            if (admission == DescriptorAdmission.UNAVAILABLE) {
                return completed(
                    new ZLinkRelocationCapacityTargetUnavailable());
            }
            if (admission == DescriptorAdmission.CAPACITY_EXHAUSTED) {
                return completed(
                    new ZLinkRelocationCapacityExhausted());
            }
            capacityReservations.put(
                fenceValue,
                new CapacityState(request, State.RESERVED));
            adjustPending(
                activeTargetAllocation(request),
                request.capacityBundle(),
                1);
            return completed(new ZLinkRelocationCapacityReserved(
                new ZLinkRelocationCapacityFence(fenceValue)));
        }
    }

    public CompletionStage<ZLinkRelocationCapacityAbortResult>
        abortRelocationCapacity(
            ZLinkRelocationCapacityFence fence,
            ZLinkStoreCancellation cancellation) {
        synchronized (gate) {
            CapacityState state = capacityReservations.get(fence.value());
            if (state == null) {
                return completed(
                    ZLinkRelocationCapacityAbortResult.STALE);
            }
            if (state.boundAggregateId != null) {
                return completed(
                    ZLinkRelocationCapacityAbortResult.STALE);
            }
            if (state.state == State.COMMITTED) {
                return completed(
                    ZLinkRelocationCapacityAbortResult.ALREADY_COMMITTED);
            }
            if (state.state == State.ABORTED) {
                return completed(
                    ZLinkRelocationCapacityAbortResult.ALREADY_ABORTED);
            }
            adjustPending(
                activeTargetAllocation(state.request),
                state.request.capacityBundle(),
                -1);
            state.state = State.ABORTED;
            return completed(
                ZLinkRelocationCapacityAbortResult.ABORTED);
        }
    }

    public CompletionStage<ZLinkAggregatePrepareResult> prepareAggregate(
        ZLinkAggregatePrepareRequest request,
        ZLinkStoreCancellation cancellation) {
        synchronized (gate) {
            AggregateState existing = aggregates.get(request.aggregateId());
            ZLinkAggregateFence fence = new ZLinkAggregateFence(
                request.aggregateId(),
                request.aggregateGeneration());
            if (existing != null) {
                if (exactAggregateRequest(existing.request, request)) {
                    return completed(new ZLinkAggregateAlreadyPrepared(fence));
                }
                boolean nextCommittedGeneration =
                    existing.state == State.COMMITTED
                        && existing.request.aggregateGeneration()
                            != Long.MAX_VALUE
                        && request.aggregateGeneration()
                            == existing.request.aggregateGeneration() + 1;
                if (!nextCommittedGeneration) {
                    return completed(
                        existing.request.aggregateGeneration()
                                == request.aggregateGeneration()
                            ? new ZLinkAggregateConflict()
                            : new ZLinkAggregateStale());
                }
            }
            if (!ownerLeaseIsLive.test(request.targetOwner())) {
                return completed(new ZLinkAggregateConflict());
            }
            if (request.participants().isEmpty()
                || request.inventoryDigest().length != 32
                || !aggregateParticipantsAreCanonical(
                    request.participants())) {
                return completed(new ZLinkAggregateConflict());
            }
            int ownerGenerationCount = Math.toIntExact(
                request.participants().stream()
                    .filter(participant ->
                        participant.ownerTransition()
                            == ZLinkAuthorityGenerationTransition.NEW_OWNER)
                    .count());
            if (ownerGenerationCount == 0
                && request.participants().stream().anyMatch(participant ->
                    participant.membershipMutation().length != 0)) {
                return completed(new ZLinkAggregateConflict());
            }
            if (!hasCounterRoom(
                    authorityOwnerGeneration,
                    ownerGenerationCount)
                || !hasCounterRoom(
                    revision,
                    request.participants().size())) {
                return completed(
                    new ZLinkAggregateGenerationExhausted());
            }
            for (ZLinkAggregateParticipant participant :
                request.participants()) {
                Row row = rows.get(participant.authorityKey());
                if (row == null
                    || participantIsPrepared(
                        participant.authorityKey())
                    || row.allocation.state()
                        != ZLinkPlacementAllocationState.ACTIVE
                    || !row.storeVersion.equals(
                        participant.expectedStoreVersion())
                    || (participant.ownerTransition()
                        == ZLinkAuthorityGenerationTransition.NEW_OWNER
                        && !targetSupportsAllocation(
                            request,
                            row.allocation))) {
                    return completed(new ZLinkAggregateConflict());
                }
            }
            if (!aggregateBundleMatchesParticipants(request)
                || descriptorAdmission(
                    request.targetDescriptor(),
                    request.targetDescriptorLifecycleGeneration(),
                    request.targetOwner(),
                    request.capacityBundle())
                    != DescriptorAdmission.ACCEPTED) {
                return completed(new ZLinkAggregateConflict());
            }
            adjustPending(
                aggregateTargetAllocation(request),
                request.capacityBundle(),
                1);
            aggregates.put(
                request.aggregateId(),
                new AggregateState(request, State.PREPARED));
            return completed(new ZLinkAggregatePrepared(fence));
        }
    }

    private boolean participantIsPrepared(String authorityKey) {
        return aggregates.values().stream().anyMatch(aggregate ->
            aggregate.state == State.PREPARED
                && aggregate.request.participants().stream().anyMatch(
                    participant ->
                        participant.authorityKey().equals(authorityKey)));
    }

    public CompletionStage<ZLinkAggregateCommitResult> commitAggregate(
        ZLinkAggregateFence fence,
        ZLinkStoreCancellation cancellation) {
        synchronized (gate) {
            AggregateState state = aggregates.get(fence.aggregateId());
            if (!sameAggregate(state, fence)) {
                return completed(ZLinkAggregateCommitResult.STALE);
            }
            if (state.state == State.COMMITTED) {
                return completed(
                    ZLinkAggregateCommitResult.ALREADY_COMMITTED);
            }
            if (state.state == State.ABORTED) {
                return completed(ZLinkAggregateCommitResult.STALE);
            }
            if (!ownerLeaseIsLive.test(state.request.targetOwner())) {
                return completed(ZLinkAggregateCommitResult.STALE);
            }
            if (!aggregateStateIsCurrent(state.request, fence)) {
                return completed(ZLinkAggregateCommitResult.STALE);
            }
            int ownerGenerationCount = Math.toIntExact(
                state.request.participants().stream()
                    .filter(participant ->
                        participant.ownerTransition()
                            == ZLinkAuthorityGenerationTransition.NEW_OWNER)
                    .count());
            if (!hasCounterRoom(
                    authorityOwnerGeneration,
                    ownerGenerationCount)
                || !hasCounterRoom(
                    revision,
                    state.request.participants().size())) {
                return completed(
                    ZLinkAggregateCommitResult.GENERATION_EXHAUSTED);
            }
            for (ZLinkAggregateParticipant participant :
                state.request.participants()) {
                Row current = rows.get(participant.authorityKey());
                boolean changesOwner = participant.ownerTransition()
                    == ZLinkAuthorityGenerationTransition.NEW_OWNER;
                long ownerGeneration = changesOwner
                    ? ++authorityOwnerGeneration
                    : current.authorityOwnerGeneration;
                ZLinkPlacementAllocation targetAllocation = changesOwner
                    ? aggregateTargetAllocation(
                        state.request,
                        current.allocation)
                    : current.allocation;
                rows.put(
                    participant.authorityKey(),
                    new Row(
                        nextVersion(),
                        participant.authorityPayload(),
                        current.objectGeneration,
                        ownerGeneration,
                        changesOwner
                            ? state.request.targetOwner()
                            : current.owner,
                        targetAllocation));
                membershipMutations.put(
                    participant.authorityKey(),
                    participant.membershipMutation());
                if (changesOwner) {
                    adjustActive(
                        current.allocation,
                        current.allocation.capacityBundle(),
                        -1);
                }
            }
            adjustPending(
                aggregateTargetAllocation(state.request),
                state.request.capacityBundle(),
                -1);
            adjustActive(
                aggregateTargetAllocation(state.request),
                state.request.capacityBundle(),
                1);
            state.state = State.COMMITTED;
            state.storeVersion = nextVersion();
            state.progress = tryInitialProgress(state.request);
            return completed(ZLinkAggregateCommitResult.COMMITTED);
        }
    }

    public CompletionStage<ZLinkAggregateAbortResult> abortAggregate(
        ZLinkAggregateFence fence,
        ZLinkStoreCancellation cancellation) {
        synchronized (gate) {
            AggregateState state = aggregates.get(fence.aggregateId());
            if (!sameAggregate(state, fence)) {
                return completed(ZLinkAggregateAbortResult.STALE);
            }
            if (state.state == State.ABORTED) {
                return completed(
                    ZLinkAggregateAbortResult.ALREADY_ABORTED);
            }
            if (state.state == State.COMMITTED) {
                return completed(ZLinkAggregateAbortResult.STALE);
            }
            state.state = State.ABORTED;
            adjustPending(
                aggregateTargetAllocation(state.request),
                state.request.capacityBundle(),
                -1);
            return completed(ZLinkAggregateAbortResult.ABORTED);
        }
    }

    public CompletionStage<Optional<ZLinkAggregateProgressSnapshot>>
        readAggregateProgress(
            ZLinkAggregateFence fence,
            ZLinkStoreCancellation cancellation) {
        synchronized (gate) {
            AggregateState state = aggregates.get(fence.aggregateId());
            if (!sameAggregate(state, fence)
                || state.state != State.COMMITTED
                || state.progress == null) {
                return completed(Optional.empty());
            }
            return completed(Optional.of(progressSnapshot(state, fence)));
        }
    }

    public CompletionStage<ZLinkAggregateProgressWriteResult>
        compareExchangeAggregateProgress(
            ZLinkAggregateFence fence,
            String expectedStoreVersion,
            ZLinkAggregateProgress progress,
            ZLinkStoreCancellation cancellation) {
        Objects.requireNonNull(expectedStoreVersion, "expectedStoreVersion");
        Objects.requireNonNull(progress, "progress");
        synchronized (gate) {
            AggregateState state = aggregates.get(fence.aggregateId());
            if (!sameAggregate(state, fence)
                || state.state != State.COMMITTED
                || !expectedStoreVersion.equals(state.storeVersion)
                || !ownerLeaseIsLive.test(state.request.targetOwner())) {
                return completed(new ZLinkAggregateProgressConflict());
            }
            state.progress = progress;
            state.storeVersion = nextVersion();
            return completed(new ZLinkAggregateProgressStored(
                progressSnapshot(state, fence)));
        }
    }

    public CompletionStage<List<ZLinkAggregateProgressSnapshot>>
        listAggregateProgress(ZLinkStoreCancellation cancellation) {
        synchronized (gate) {
            List<ZLinkAggregateProgressSnapshot> result = aggregates.values()
                .stream()
                .filter(value -> value.state == State.COMMITTED
                    && value.progress != null)
                .map(value -> progressSnapshot(
                    value,
                    new ZLinkAggregateFence(
                        value.request.aggregateId(),
                        value.request.aggregateGeneration())))
                .toList();
            return completed(result);
        }
    }

    public CompletionStage<Boolean> removeAggregateProgress(
        ZLinkAggregateFence fence,
        String expectedStoreVersion,
        ZLinkStoreCancellation cancellation) {
        Objects.requireNonNull(expectedStoreVersion, "expectedStoreVersion");
        synchronized (gate) {
            AggregateState state = aggregates.get(fence.aggregateId());
            if (!sameAggregate(state, fence)
                || state.state != State.COMMITTED
                || !expectedStoreVersion.equals(state.storeVersion)
                || !ownerLeaseIsLive.test(state.request.targetOwner())) {
                return completed(false);
            }
            aggregates.remove(fence.aggregateId());
            return completed(true);
        }
    }

    private static ZLinkAggregateProgressSnapshot progressSnapshot(
        AggregateState state,
        ZLinkAggregateFence fence) {
        return new ZLinkAggregateProgressSnapshot(
            fence,
            state.storeVersion,
            state.request,
            state.progress);
    }

    private static ZLinkAggregateProgress initialProgress(
        ZLinkAggregatePrepareRequest request) {
        for (ZLinkAggregateParticipant participant : request.participants()) {
            try {
                return ZLinkCanonicalRelocationAuthorityStateCodec.progress(
                    participant.authorityPayload());
            } catch (RuntimeException ignored) {
                // The next participant may contain the canonical publication.
            }
        }
        throw new IllegalStateException(
            "committed aggregate has no canonical relocation root");
    }

    private static ZLinkAggregateProgress tryInitialProgress(
        ZLinkAggregatePrepareRequest request) {
        try {
            return initialProgress(request);
        } catch (RuntimeException ignored) {
            return null;
        }
    }

    private boolean matches(
        Row current,
        ZLinkAuthorityExpectation expectation) {
        return current != null
            && current.storeVersion.equals(
                ((ZLinkAuthorityExpectFound) expectation).storeVersion());
    }

    private String nextVersion() {
        if (revision == Long.MAX_VALUE) {
            throw new ZLinkConfigurationException(
                "authority Store revision is exhausted");
        }
        return Long.toString(++revision);
    }

    private static boolean sameReservation(
        ReservationState state,
        ZLinkObjectReservation reservation) {
        return state != null
            && state.reservation.equals(reservation);
    }

    private boolean targetDescriptorIsCurrent(
        ZLinkRelocationCapacityReservationRequest request) {
        ZLinkMeshNodeDescriptor descriptor = descriptorLookup.find(
            request.targetDescriptor(),
            request.targetDescriptorLifecycleGeneration(),
            request.targetOwner());
        if (descriptor == null
            || descriptor.lifecycleGeneration()
                != request.targetDescriptorLifecycleGeneration()
            || !descriptor.ownerId().equals(request.targetOwner().ownerId())
            || descriptor.leaseGeneration()
                != request.targetOwner().leaseGeneration()
            || descriptor.state()
                != systems.zlink.framework.runtime.host
                    .ZLinkFrameworkRuntimeState.SERVING
            || descriptor.objectRole() != ZLinkMeshNodeObjectRole.SERVER
            || descriptor.placementWeight() <= 0
            || descriptor.objectCapabilities().stream().noneMatch(
                capability ->
                    capability.objectKind() == request.objectKind()
                        && capability.stableType().equals(
                            request.stableType()))) {
            return false;
        }
        return currentCapacityFits(descriptor, request.capacityBundle());
    }

    private boolean currentCapacityFits(
        ZLinkMeshNodeDescriptor descriptor,
        ZLinkPlacementCapacityBundle bundle) {
        AllocationCounterKey nodeKey = new AllocationCounterKey(
            new ZLinkMeshNodeDescriptorKey(
                descriptor.meshName(),
                descriptor.rid()),
            descriptor.lifecycleGeneration());
        if (!hasCapacity(
                actorAllocationCounters.get(nodeKey),
                descriptor.capacity().actors().limit(),
                0)
            || !hasCapacity(
                spotAllocationCounters.get(nodeKey),
                descriptor.capacity().spots().limit(),
                0)) {
            return false;
        }
        if (bundle.spotType().isEmpty()) {
            return true;
        }
        ZLinkSpotTypeCapacityDelta delta = bundle.spotType().orElseThrow();
        ZLinkObjectCapability capability =
            descriptor.objectCapabilities().stream()
                .filter(candidate ->
                    candidate.objectKind() == delta.objectKind()
                        && candidate.stableType().equals(
                            delta.stableType()))
                .findFirst()
                .orElse(null);
        return capability != null
            && hasCapacity(
                typeAllocationCounters.get(
                    new TypeAllocationCounterKey(
                        nodeKey.descriptor(),
                        nodeKey.lifecycleGeneration(),
                        delta.objectKind(),
                        delta.stableType())),
                capability.spotLimit(),
                0);
    }

    private DescriptorAdmission descriptorAdmission(
        ZLinkMeshNodeDescriptorKey descriptorKey,
        long lifecycleGeneration,
        ZLinkLocationOwnerToken owner,
        ZLinkPlacementObjectKind objectKind,
        String stableType,
        ZLinkPlacementCapacityBundle capacityBundle) {
        ZLinkMeshNodeDescriptor descriptor = descriptorLookup.find(
            descriptorKey,
            lifecycleGeneration,
            owner);
        if (descriptor == null
            || !descriptor.meshName().equals(
                descriptorKey.meshName())
            || !descriptor.rid().equals(descriptorKey.rid())
            || descriptor.lifecycleGeneration()
                != lifecycleGeneration
            || !descriptor.ownerId().equals(owner.ownerId())
            || descriptor.leaseGeneration()
                != owner.leaseGeneration()
            || descriptor.state()
                != systems.zlink.framework.runtime.host
                    .ZLinkFrameworkRuntimeState.SERVING
            || descriptor.objectRole()
                != ZLinkMeshNodeObjectRole.SERVER
            || descriptor.placementWeight() <= 0) {
            return DescriptorAdmission.UNAVAILABLE;
        }
        ZLinkObjectCapability capability =
            descriptor.objectCapabilities().stream()
                .filter(candidate ->
                    candidate.objectKind() == objectKind
                        && candidate.stableType().equals(stableType))
                .findFirst()
                .orElse(null);
        if (capability == null) {
            return DescriptorAdmission.UNAVAILABLE;
        }
        if (!bundleMatchesObject(
            capacityBundle,
            objectKind,
            stableType)) {
            return DescriptorAdmission.UNAVAILABLE;
        }
        return canReserve(
            descriptor,
            capacityBundle)
                ? DescriptorAdmission.ACCEPTED
                : DescriptorAdmission.CAPACITY_EXHAUSTED;
    }

    private boolean descriptorIsCurrent(
        ZLinkMeshNodeDescriptorKey descriptorKey,
        long lifecycleGeneration,
        ZLinkLocationOwnerToken owner,
        ZLinkPlacementObjectKind objectKind,
        String stableType) {
        ZLinkMeshNodeDescriptor descriptor = descriptorLookup.find(
            descriptorKey,
            lifecycleGeneration,
            owner);
        return descriptor != null
            && descriptor.state()
                == systems.zlink.framework.runtime.host
                    .ZLinkFrameworkRuntimeState.SERVING
            && descriptor.objectRole() == ZLinkMeshNodeObjectRole.SERVER
            && descriptor.objectCapabilities().stream().anyMatch(
                capability ->
                    capability.objectKind() == objectKind
                        && capability.stableType().equals(stableType));
    }

    private DescriptorAdmission descriptorAdmission(
        ZLinkMeshNodeDescriptorKey descriptorKey,
        long lifecycleGeneration,
        ZLinkLocationOwnerToken owner,
        ZLinkPlacementCapacityBundle capacityBundle) {
        ZLinkMeshNodeDescriptor descriptor = descriptorLookup.find(
            descriptorKey,
            lifecycleGeneration,
            owner);
        if (descriptor == null
            || descriptor.lifecycleGeneration() != lifecycleGeneration
            || !descriptor.ownerId().equals(owner.ownerId())
            || descriptor.leaseGeneration() != owner.leaseGeneration()
            || descriptor.state()
                != systems.zlink.framework.runtime.host
                    .ZLinkFrameworkRuntimeState.SERVING
            || descriptor.objectRole() != ZLinkMeshNodeObjectRole.SERVER
            || descriptor.placementWeight() <= 0) {
            return DescriptorAdmission.UNAVAILABLE;
        }
        if (capacityBundle.spotType().isPresent()) {
            ZLinkSpotTypeCapacityDelta spot =
                capacityBundle.spotType().orElseThrow();
            boolean supported = descriptor.objectCapabilities().stream()
                .anyMatch(capability ->
                    capability.objectKind() == spot.objectKind()
                        && capability.stableType().equals(
                            spot.stableType()));
            if (!supported) {
                return DescriptorAdmission.UNAVAILABLE;
            }
        }
        return canReserve(descriptor, capacityBundle)
            ? DescriptorAdmission.ACCEPTED
            : DescriptorAdmission.CAPACITY_EXHAUSTED;
    }

    private boolean canReserve(
        ZLinkMeshNodeDescriptor descriptor,
        ZLinkPlacementCapacityBundle bundle) {
        AllocationCounterKey nodeKey = new AllocationCounterKey(
            new ZLinkMeshNodeDescriptorKey(
                descriptor.meshName(),
                descriptor.rid()),
            descriptor.lifecycleGeneration());
        CapacityCounter actors = actorAllocationCounters.get(nodeKey);
        CapacityCounter spots = spotAllocationCounters.get(nodeKey);
        if (!hasCapacity(
                actors,
                descriptor.capacity().actors().limit(),
                bundle.actorSlots())
            || !hasCapacity(
                spots,
                descriptor.capacity().spots().limit(),
                bundle.spotSlots())) {
            return false;
        }
        if (bundle.spotType().isEmpty()) {
            return true;
        }
        ZLinkSpotTypeCapacityDelta delta =
            bundle.spotType().orElseThrow();
        ZLinkObjectCapability capability =
            descriptor.objectCapabilities().stream()
                .filter(candidate ->
                    candidate.objectKind() == delta.objectKind()
                        && candidate.stableType().equals(
                            delta.stableType()))
                .findFirst()
                .orElse(null);
        if (capability == null) {
            return false;
        }
        CapacityCounter type = typeAllocationCounters.get(
            new TypeAllocationCounterKey(
                nodeKey.descriptor(),
                nodeKey.lifecycleGeneration(),
                delta.objectKind(),
                delta.stableType()));
        return hasCapacity(type, capability.spotLimit(), delta.slots());
    }

    private static boolean hasCapacity(
        CapacityCounter counter,
        int limit,
        int requested) {
        long active = counter == null ? 0 : counter.active;
        long pending = counter == null ? 0 : counter.pending;
        return limit == 0 || active + pending + requested <= limit;
    }

    private static boolean bundleMatchesObject(
        ZLinkPlacementCapacityBundle bundle,
        ZLinkPlacementObjectKind kind,
        String stableType) {
        if (kind == ZLinkPlacementObjectKind.ACTOR) {
            return bundle.actorSlots() > 0
                && bundle.spotSlots() == 0
                && bundle.spotType().isEmpty();
        }
        return bundle.actorSlots() == 0
            && bundle.spotSlots() > 0
            && bundle.spotType()
                .filter(delta ->
                    delta.objectKind() == kind
                        && delta.stableType().equals(stableType)
                        && delta.slots() == bundle.spotSlots())
                .isPresent();
    }

    private static boolean sourceAllocationMatches(
        ZLinkPlacementAllocation allocation,
        ZLinkRelocationCapacityReservationRequest request) {
        return allocation.state()
                == ZLinkPlacementAllocationState.ACTIVE
            && allocation.objectKind() == request.objectKind()
            && allocation.stableType().equals(request.stableType())
            && allocation.descriptor().equals(
                request.sourceDescriptor())
            && allocation.descriptorLifecycleGeneration()
                == request.sourceDescriptorLifecycleGeneration()
            && allocation.capacityBundle().equals(
                request.capacityBundle());
    }

    private static ZLinkPlacementAllocation activeTargetAllocation(
        ZLinkRelocationCapacityReservationRequest request) {
        return new ZLinkPlacementAllocation(
            ZLinkPlacementAllocationState.ACTIVE,
            request.objectKind(),
            request.stableType(),
            request.targetDescriptor(),
            request.targetDescriptorLifecycleGeneration(),
            request.capacityBundle());
    }

    private static ZLinkPlacementAllocation aggregateTargetAllocation(
        ZLinkAggregatePrepareRequest request) {
        ZLinkSpotTypeCapacityDelta spotType =
            request.capacityBundle().spotType().orElse(null);
        return new ZLinkPlacementAllocation(
            ZLinkPlacementAllocationState.ACTIVE,
            spotType == null
                ? ZLinkPlacementObjectKind.ACTOR
                : spotType.objectKind(),
            spotType == null ? "aggregate-actors" : spotType.stableType(),
            request.targetDescriptor(),
            request.targetDescriptorLifecycleGeneration(),
            request.capacityBundle());
    }

    private static ZLinkPlacementAllocation aggregateTargetAllocation(
        ZLinkAggregatePrepareRequest request,
        ZLinkPlacementAllocation source) {
        return new ZLinkPlacementAllocation(
            ZLinkPlacementAllocationState.ACTIVE,
            source.objectKind(),
            source.stableType(),
            request.targetDescriptor(),
            request.targetDescriptorLifecycleGeneration(),
            source.capacityBundle());
    }

    private boolean targetSupportsAllocation(
        ZLinkAggregatePrepareRequest request,
        ZLinkPlacementAllocation allocation) {
        ZLinkMeshNodeDescriptor descriptor = descriptorLookup.find(
            request.targetDescriptor(),
            request.targetDescriptorLifecycleGeneration(),
            request.targetOwner());
        return descriptor != null
            && descriptor.objectCapabilities().stream().anyMatch(
                capability ->
                    capability.objectKind() == allocation.objectKind()
                        && capability.stableType().equals(
                            allocation.stableType()));
    }

    private boolean aggregateBundleMatchesParticipants(
        ZLinkAggregatePrepareRequest request) {
        long actors = 0;
        long spots = 0;
        ZLinkSpotTypeCapacityDelta expectedSpotType = null;
        for (ZLinkAggregateParticipant participant : request.participants()) {
            if (participant.ownerTransition()
                != ZLinkAuthorityGenerationTransition.NEW_OWNER) {
                continue;
            }
            Row row = rows.get(participant.authorityKey());
            if (row == null) {
                return false;
            }
            ZLinkPlacementCapacityBundle bundle =
                row.allocation.capacityBundle();
            actors = Math.addExact(actors, bundle.actorSlots());
            spots = Math.addExact(spots, bundle.spotSlots());
            if (bundle.spotType().isPresent()) {
                ZLinkSpotTypeCapacityDelta current =
                    bundle.spotType().orElseThrow();
                if (expectedSpotType != null
                    && (expectedSpotType.objectKind()
                            != current.objectKind()
                        || !expectedSpotType.stableType().equals(
                            current.stableType()))) {
                    return false;
                }
                expectedSpotType = current;
            }
        }
        ZLinkPlacementCapacityBundle requested = request.capacityBundle();
        return actors == requested.actorSlots()
            && spots == requested.spotSlots()
            && java.util.Objects.equals(
                expectedSpotType,
                requested.spotType().orElse(null));
    }

    private static boolean pendingReservationMatches(
        Row row,
        ZLinkObjectReservation reservation) {
        return row != null
            && row.storeVersion.equals(reservation.storeVersion())
            && row.objectGeneration == reservation.objectGeneration()
            && row.authorityOwnerGeneration
                == reservation.authorityOwnerGeneration()
            && row.owner.equals(reservation.targetOwner())
            && row.allocation.state()
                == ZLinkPlacementAllocationState.PENDING
            && row.allocation.descriptor().equals(
                reservation.targetDescriptor())
            && row.allocation.descriptorLifecycleGeneration()
                == reservation.targetDescriptorLifecycleGeneration();
    }

    private static ZLinkPlacementAllocation withAllocationState(
        ZLinkPlacementAllocation allocation,
        ZLinkPlacementAllocationState state) {
        return new ZLinkPlacementAllocation(
            state,
            allocation.objectKind(),
            allocation.stableType(),
            allocation.descriptor(),
            allocation.descriptorLifecycleGeneration(),
            allocation.capacityBundle());
    }

    private void activateAllocation(ZLinkPlacementAllocation allocation) {
        adjustPending(allocation, allocation.capacityBundle(), -1);
        adjustActive(allocation, allocation.capacityBundle(), 1);
    }

    private void relocateAllocation(
        ZLinkPlacementAllocation source,
        ZLinkPlacementAllocation target) {
        adjustActive(source, source.capacityBundle(), -1);
        adjustPending(target, target.capacityBundle(), -1);
        adjustActive(target, target.capacityBundle(), 1);
    }

    private void adjustPending(
        ZLinkPlacementAllocation allocation,
        ZLinkPlacementCapacityBundle bundle,
        int direction) {
        adjustBundle(allocation, bundle, direction, false);
    }

    private void adjustActive(
        ZLinkPlacementAllocation allocation,
        ZLinkPlacementCapacityBundle bundle,
        int direction) {
        adjustBundle(allocation, bundle, direction, true);
    }

    private void adjustBundle(
        ZLinkPlacementAllocation allocation,
        ZLinkPlacementCapacityBundle bundle,
        int direction,
        boolean active) {
        AllocationCounterKey nodeKey = new AllocationCounterKey(
            allocation.descriptor(),
            allocation.descriptorLifecycleGeneration());
        adjustCounter(
            actorAllocationCounters.computeIfAbsent(
                nodeKey, ignored -> new CapacityCounter()),
            (long) direction * bundle.actorSlots(),
            active);
        adjustCounter(
            spotAllocationCounters.computeIfAbsent(
                nodeKey, ignored -> new CapacityCounter()),
            (long) direction * bundle.spotSlots(),
            active);
        bundle.spotType().ifPresent(delta ->
            adjustCounter(
                typeAllocationCounters.computeIfAbsent(
                    new TypeAllocationCounterKey(
                        nodeKey.descriptor(),
                        nodeKey.lifecycleGeneration(),
                        delta.objectKind(),
                        delta.stableType()),
                    ignored -> new CapacityCounter()),
                (long) direction * delta.slots(),
                active));
    }

    private static void adjustCounter(
        CapacityCounter counter,
        long delta,
        boolean active) {
        long value = Math.addExact(
            active ? counter.active : counter.pending,
            delta);
        if (value < 0) {
            throw new IllegalStateException(
                "placement capacity became negative");
        }
        if (active) {
            counter.active = value;
        } else {
            counter.pending = value;
        }
    }

    long activeCapacity(
        ZLinkMeshNodeDescriptorKey descriptor,
        long lifecycleGeneration) {
        synchronized (gate) {
            AllocationCounterKey key = new AllocationCounterKey(
                descriptor,
                lifecycleGeneration);
            CapacityCounter actors = actorAllocationCounters.get(key);
            CapacityCounter spots = spotAllocationCounters.get(key);
            return (actors == null ? 0 : actors.active)
                + (spots == null ? 0 : spots.active);
        }
    }

    long pendingCapacity(
        ZLinkMeshNodeDescriptorKey descriptor,
        long lifecycleGeneration) {
        synchronized (gate) {
            AllocationCounterKey key = new AllocationCounterKey(
                descriptor,
                lifecycleGeneration);
            CapacityCounter actors = actorAllocationCounters.get(key);
            CapacityCounter spots = spotAllocationCounters.get(key);
            return (actors == null ? 0 : actors.pending)
                + (spots == null ? 0 : spots.pending);
        }
    }

    long[] kindCapacity(
        ZLinkMeshNodeDescriptorKey descriptor,
        long lifecycleGeneration,
        boolean actors) {
        synchronized (gate) {
            CapacityCounter counter = (actors
                ? actorAllocationCounters
                : spotAllocationCounters).get(
                    new AllocationCounterKey(
                        descriptor,
                        lifecycleGeneration));
            return counter == null
                ? new long[] {0, 0}
                : new long[] {counter.active, counter.pending};
        }
    }

    long[] typeCapacity(
        ZLinkMeshNodeDescriptorKey descriptor,
        long lifecycleGeneration,
        ZLinkPlacementObjectKind kind,
        String stableType) {
        synchronized (gate) {
            CapacityCounter counter = typeAllocationCounters.get(
                new TypeAllocationCounterKey(
                    descriptor,
                    lifecycleGeneration,
                    kind,
                    stableType));
            return counter == null
                ? new long[] {0, 0}
                : new long[] {counter.active, counter.pending};
        }
    }

    byte[] membershipMutation(String authorityKey) {
        synchronized (gate) {
            byte[] value = membershipMutations.get(authorityKey);
            return value == null ? null : value.clone();
        }
    }

    private static boolean exactAggregateRequest(
        ZLinkAggregatePrepareRequest left,
        ZLinkAggregatePrepareRequest right) {
        if (!left.aggregateId().equals(right.aggregateId())
            || left.aggregateGeneration()
                != right.aggregateGeneration()
            || !left.targetOwner().equals(right.targetOwner())
            || !left.targetDescriptor().equals(
                right.targetDescriptor())
            || left.targetDescriptorLifecycleGeneration()
                != right.targetDescriptorLifecycleGeneration()
            || !left.capacityBundle().equals(
                right.capacityBundle())
            || !Arrays.equals(
                left.inventoryDigest(),
                right.inventoryDigest())
            || left.participants().size()
                != right.participants().size()) {
            return false;
        }
        for (int index = 0;
             index < left.participants().size();
             index++) {
            ZLinkAggregateParticipant first =
                left.participants().get(index);
            ZLinkAggregateParticipant second =
                right.participants().get(index);
            if (!first.authorityKey().equals(second.authorityKey())
                || !first.expectedStoreVersion().equals(
                    second.expectedStoreVersion())
                || first.ownerTransition()
                    != second.ownerTransition()
                || !Arrays.equals(
                    first.authorityPayload(),
                    second.authorityPayload())
                || !Arrays.equals(
                    first.membershipMutation(),
                    second.membershipMutation())) {
                return false;
            }
        }
        return true;
    }

    private static boolean aggregateParticipantsAreCanonical(
        List<ZLinkAggregateParticipant> participants) {
        byte[] previous = null;
        for (ZLinkAggregateParticipant participant : participants) {
            byte[] current = participant.authorityKey()
                .getBytes(StandardCharsets.UTF_8);
            if (previous != null
                && Arrays.compareUnsigned(previous, current) >= 0) {
                return false;
            }
            previous = current;
        }
        return true;
    }

    private static boolean hasCounterRoom(long current, int increments) {
        return increments >= 0
            && current <= Long.MAX_VALUE - increments;
    }

    private boolean aggregateStateIsCurrent(
        ZLinkAggregatePrepareRequest request,
        ZLinkAggregateFence aggregateFence) {
        ZLinkMeshNodeDescriptor target = descriptorLookup.find(
            request.targetDescriptor(),
            request.targetDescriptorLifecycleGeneration(),
            request.targetOwner());
        if (target == null
            || target.state()
                != systems.zlink.framework.runtime.host
                    .ZLinkFrameworkRuntimeState.SERVING
            || target.objectRole() != ZLinkMeshNodeObjectRole.SERVER
            || target.placementWeight() <= 0) {
            return false;
        }
        for (ZLinkAggregateParticipant participant :
            request.participants()) {
            Row current = rows.get(participant.authorityKey());
            if (current == null
                || !current.storeVersion.equals(
                    participant.expectedStoreVersion())) {
                return false;
            }
            if (participant.ownerTransition()
                != ZLinkAuthorityGenerationTransition.NEW_OWNER) {
                continue;
            }
            if (!targetSupportsAllocation(
                request,
                current.allocation)) {
                return false;
            }
        }
        return true;
    }

    private static boolean sameAggregate(
        AggregateState state,
        ZLinkAggregateFence fence) {
        return state != null
            && state.request.aggregateGeneration()
                == fence.aggregateGeneration();
    }

    private ZLinkAuthoritySnapshot snapshot(Row row, Instant now) {
        java.util.Optional<
            systems.zlink.framework.runtime.internal.locations.ZLinkPendingObjectCreation> pending =
            java.util.Optional.empty();
        if (row.allocation.state()
                == ZLinkPlacementAllocationState.PENDING) {
            ReservationState state = reservations.values().stream()
                .filter(value ->
                    value.state == State.PREPARED
                    && pendingReservationMatches(
                        row, value.reservation))
                .findFirst()
                .orElse(null);
            if (state != null) {
                pending = java.util.Optional.of(
                    new systems.zlink.framework.runtime.internal.locations.ZLinkPendingObjectCreation(
                            state.reservation
                                .reservationVersion(),
                            state.request
                                .creationIntentReference(),
                            state.request.creationIntentHash(),
                            state.request
                                .creationIntentEncodedSize()));
            }
        }
        return new ZLinkAuthoritySnapshot(
            row.storeVersion,
            row.payload,
            row.objectGeneration,
            row.authorityOwnerGeneration,
            row.owner.ownerId(),
            row.owner.leaseGeneration(),
            row.allocation,
            pending,
            now);
    }

    private static ZLinkAuthorityStored stored(Row row, Instant now) {
        return new ZLinkAuthorityStored(
            row.storeVersion,
            row.payload,
            row.objectGeneration,
            row.authorityOwnerGeneration,
            row.owner.ownerId(),
            row.owner.leaseGeneration(),
            row.allocation,
            now);
    }

    private static <T> CompletionStage<T> completed(T value) {
        return CompletableFuture.completedFuture(value);
    }

    private record Row(
        String storeVersion,
        byte[] payload,
        long objectGeneration,
        long authorityOwnerGeneration,
        ZLinkLocationOwnerToken owner,
        ZLinkPlacementAllocation allocation) {
        private Row {
            payload = payload.clone();
        }

        private Row withPayloadAndAllocation(
            String version,
            byte[] value,
            ZLinkPlacementAllocation nextAllocation) {
            return new Row(
                version,
                value,
                objectGeneration,
                authorityOwnerGeneration,
                owner,
                nextAllocation);
        }
    }

    private record AllocationCounterKey(
        ZLinkMeshNodeDescriptorKey descriptor,
        long lifecycleGeneration) {
    }

    private record TypeAllocationCounterKey(
        ZLinkMeshNodeDescriptorKey descriptor,
        long lifecycleGeneration,
        ZLinkPlacementObjectKind objectKind,
        String stableType) {
    }

    private static final class CapacityCounter {
        private long active;
        private long pending;
    }

    private enum DescriptorAdmission {
        ACCEPTED,
        UNAVAILABLE,
        CAPACITY_EXHAUSTED
    }

    private enum State {
        RESERVED,
        PREPARED,
        COMMITTED,
        ABORTED
    }

    private static final class ReservationState {
        private final ZLinkObjectReservation reservation;
        private final ZLinkObjectReservationRequest request;
        private State state;

        private ReservationState(
            ZLinkObjectReservation reservation,
            ZLinkObjectReservationRequest request,
            State state) {
            this.reservation = reservation;
            this.request = request;
            this.state = state;
        }
    }

    private static final class AggregateState {
        private final ZLinkAggregatePrepareRequest request;
        private State state;
        private String storeVersion;
        private ZLinkAggregateProgress progress;

        private AggregateState(
            ZLinkAggregatePrepareRequest request,
            State state) {
            this.request = request;
            this.state = state;
        }
    }

    private static final class CapacityState {
        private final ZLinkRelocationCapacityReservationRequest request;
        private State state;
        private UUID boundAggregateId;
        private long boundAggregateGeneration;

        private CapacityState(
            ZLinkRelocationCapacityReservationRequest request,
            State state) {
            this.request = request;
            this.state = state;
        }

        private boolean isBoundTo(ZLinkAggregateFence fence) {
            return boundAggregateId != null
                && boundAggregateId.equals(fence.aggregateId())
                && boundAggregateGeneration
                    == fence.aggregateGeneration();
        }
    }

    @FunctionalInterface
    interface DescriptorLookup {
        ZLinkMeshNodeDescriptor find(
            ZLinkMeshNodeDescriptorKey descriptor,
            long lifecycleGeneration,
            ZLinkLocationOwnerToken owner);
    }
}
