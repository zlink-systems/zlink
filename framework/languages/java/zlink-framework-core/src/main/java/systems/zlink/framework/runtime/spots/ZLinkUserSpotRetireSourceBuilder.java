package systems.zlink.framework.runtime.spots;

import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;
import java.util.LinkedHashMap;
import java.util.Map;
import java.util.Objects;
import java.util.Optional;
import java.util.UUID;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionException;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.ThreadLocalRandom;
import java.util.concurrent.atomic.AtomicReference;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.actors.ZLinkActor;
import systems.zlink.framework.actors.ZLinkRelocationCancellation;
import systems.zlink.framework.execution.ZLinkAsyncSerialQueue;
import systems.zlink.framework.locations.*;
import systems.zlink.framework.runtime.internal.locations.*;
import systems.zlink.framework.runtime.internal.locations
    .ZLinkLocationRepository;
import systems.zlink.framework.runtime.host.ZLinkFrameworkRuntimeState;
import systems.zlink.framework.runtime.internal.locations
    .ZLinkAggregateRelocationCoordinator;
import systems.zlink.framework.runtime.internal.locations
    .ZLinkRelocationPermitPool;
import systems.zlink.framework.runtime.internal.locations
    .ZLinkRelocationStored;
import systems.zlink.framework.runtime.internal.relocation
    .ZLinkRelocationAdapterRegistry;
import systems.zlink.framework.runtime.locations.ZLinkAuthorityKeyCodec;
import systems.zlink.framework.runtime.locations
    .ZLinkServiceAuthorityPayloadCodec;
import systems.zlink.framework.runtime.mesh.MeshNodeRegistration;
import systems.zlink.framework.runtime.internal.configuration
    .ZLinkObjectFactoryRegistration.RelocatableActorFactory;
import systems.zlink.framework.runtime.internal.configuration
    .ZLinkObjectFactoryRegistration.RelocatableSpotFactory;
import systems.zlink.framework.runtime.internal.configuration
    .ZLinkObjectFactoryRegistration.RelocationPolicy;
import systems.zlink.framework.spots.ZLinkSpot;

/**
 * Builds the reversible source half of one User Spot Retire transaction from
 * live runtime objects and exact Location authority snapshots.
 */
final class ZLinkUserSpotRetireSourceBuilder {
    private static final long SNAPSHOT_RESERVATION_BYTES =
        64L * 1024 * 1024;
    private static final long ENVELOPE_RESERVATION_BYTES = 64L * 1024;
    private static final int PAGE_SIZE = 1000;
    private static final int MAX_DESCRIPTORS = 65_536;
    private static final byte[] SOURCE_CLEANUP_PENDING =
        "zlink.spot.source.pending.v1".getBytes(
            java.nio.charset.StandardCharsets.UTF_8);

    private final String meshName;
    private final RoutingId localNodeRid;
    private final long localNodeGeneration;
    private final ZLinkLocationRepository locations;
    private final ZLinkAggregateRelocationCoordinator coordinator;
    private final ZLinkRelocationPermitPool permits;
    private final ZLinkSpotLifecycle spots;
    private final ZLinkSpotRuntime relocationReplies;
    private final ZLinkActorSessionCoordinator actors;
    private final ZLinkRelocationAdapterRegistry adapters;
    private final Map<String, RelocatableSpotFactory<?>>
        spotFactories;
    private final Map<String, RelocatableActorFactory<?>>
        actorFactories;
    private final ZLinkServiceAuthorityPayloadCodec spotAuthorities =
        new ZLinkServiceAuthorityPayloadCodec();
    private final List<UnresolvedPreparation> unresolved =
        java.util.Collections.synchronizedList(new ArrayList<>());

    ZLinkUserSpotRetireSourceBuilder(
        String meshName,
        RoutingId localNodeRid,
        long localNodeGeneration,
        ZLinkLocationRepository locations,
        ZLinkAggregateRelocationCoordinator coordinator,
        ZLinkRelocationPermitPool permits,
        ZLinkSpotLifecycle spots,
        ZLinkActorSessionCoordinator actors,
        ZLinkRelocationAdapterRegistry adapters,
        Map<String, RelocatableSpotFactory<?>>
            spotFactories,
        Map<String, RelocatableActorFactory<?>>
            actorFactories) {
        this(
            meshName,
            localNodeRid,
            localNodeGeneration,
            locations,
            coordinator,
            permits,
            spots,
            actors,
            adapters,
            spotFactories,
            actorFactories,
            null);
    }

    ZLinkUserSpotRetireSourceBuilder(
        String meshName,
        RoutingId localNodeRid,
        long localNodeGeneration,
        ZLinkLocationRepository locations,
        ZLinkAggregateRelocationCoordinator coordinator,
        ZLinkRelocationPermitPool permits,
        ZLinkSpotLifecycle spots,
        ZLinkActorSessionCoordinator actors,
        ZLinkRelocationAdapterRegistry adapters,
        Map<String, RelocatableSpotFactory<?>>
            spotFactories,
        Map<String, RelocatableActorFactory<?>>
            actorFactories,
        ZLinkSpotRuntime relocationReplies) {
        if (meshName == null || meshName.isBlank()) {
            throw new IllegalArgumentException("meshName is required");
        }
        if (localNodeGeneration <= 0) {
            throw new IllegalArgumentException(
                "local node generation must be positive");
        }
        this.meshName = meshName;
        this.localNodeRid = Objects.requireNonNull(
            localNodeRid, "localNodeRid");
        this.localNodeGeneration = localNodeGeneration;
        this.locations = Objects.requireNonNull(locations, "locations");
        this.coordinator = Objects.requireNonNull(coordinator, "coordinator");
        this.permits = Objects.requireNonNull(permits, "permits");
        this.spots = Objects.requireNonNull(spots, "spots");
        this.relocationReplies = relocationReplies;
        this.actors = Objects.requireNonNull(actors, "actors");
        this.adapters = Objects.requireNonNull(adapters, "adapters");
        this.spotFactories = Map.copyOf(
            Objects.requireNonNull(spotFactories, "spotFactories"));
        this.actorFactories = Map.copyOf(
            Objects.requireNonNull(actorFactories, "actorFactories"));
    }

    CompletionStage<PreparedSource> prepare(
        String spotId,
        ZLinkRelocationTargetPolicy targetPolicy,
        ZLinkStoreCancellation cancellation) {
        Objects.requireNonNull(targetPolicy, "targetPolicy");
        Objects.requireNonNull(cancellation, "cancellation");
        ZLinkSpot<?> spot = activeSpot(spotId, cancellation);
        if (spot == null) {
            return cancellation.isCancellationRequested()
                ? cancelled()
                : failed(new IllegalStateException(
                    "User Spot is not active locally: " + spotId));
        }
        return admit(spotId, spot, targetPolicy, cancellation)
            .thenCompose(admission -> sealAndCapture(
                spot,
                admission,
                cancellation));
    }

    CompletionStage<Void> preflight(
        String spotId,
        ZLinkRelocationTargetPolicy targetPolicy,
        ZLinkRelocationCapacityPlan capacityPlan,
        ZLinkStoreCancellation cancellation) {
        Objects.requireNonNull(targetPolicy, "targetPolicy");
        Objects.requireNonNull(capacityPlan, "capacityPlan");
        Objects.requireNonNull(cancellation, "cancellation");
        ZLinkSpot<?> spot = activeSpot(spotId, cancellation);
        if (spot == null) {
            return cancellation.isCancellationRequested()
                ? cancelled()
                : failed(new IllegalStateException(
                    "User Spot is not active locally: " + spotId));
        }
        return admit(
            spotId,
            spot,
            targetPolicy,
            capacityPlan,
            cancellation).thenApply(admission -> {
                capacityPlan.reserveUserSpot(
                    spotId,
                    admission.target(),
                    admission.inventory().spot().stableType(),
                    admission.inventory().actors().size());
                return null;
            });
    }

    CompletionStage<PreparedSource> preparePinned(
        String spotId,
        ZLinkRelocationTargetPolicy targetPolicy,
        ZLinkMeshNodeDescriptor pinnedTarget,
        ZLinkStoreCancellation cancellation) {
        Objects.requireNonNull(pinnedTarget, "pinnedTarget");
        ZLinkSpot<?> spot = activeSpot(spotId, cancellation);
        if (spot == null) {
            return cancellation.isCancellationRequested()
                ? cancelled()
                : failed(new IllegalStateException(
                    "User Spot is not active locally: " + spotId));
        }
        List<String> actorIds = actors.actorIdsInSpot(spotId);
        return readInventory(spotId, actorIds, cancellation)
            .thenCompose(inventory -> {
                validateLiveInventory(spot, inventory);
                return listDescriptors(cancellation)
                    .thenApply(descriptors -> new Admission(
                        inventory,
                        descriptors.stream()
                            .filter(candidate -> candidate.rid().equals(
                                pinnedTarget.rid()))
                            .filter(candidate -> candidate.lifecycleGeneration()
                                == pinnedTarget.lifecycleGeneration())
                            .filter(this::baseEligible)
                            .filter(candidate -> hasCapabilities(
                                inventory, candidate))
                            .findFirst()
                            .orElseThrow(() -> new ZLinkUserSpotRetireRuntime
                                .RelocationBlockedException(
                                systems.zlink.framework.runtime.host
                                    .ZLinkFrameworkRelocationReason.TARGET_UNAVAILABLE,
                                "Pinned User Spot relocation target is no longer Ready")),
                        descriptors));
            })
            .thenCompose(admission -> sealAndCapture(
                spot, admission, cancellation));
    }

    private ZLinkSpot<?> activeSpot(
        String spotId,
        ZLinkStoreCancellation cancellation) {
        if (cancellation.isCancellationRequested()) {
            return null;
        }
        return spots.spotFor(spotId);
    }

    private CompletionStage<Admission> admit(
        String spotId,
        ZLinkSpot<?> spot,
        ZLinkRelocationTargetPolicy targetPolicy,
        ZLinkStoreCancellation cancellation) {
        return admit(
            spotId, spot, targetPolicy, null, cancellation);
    }

    private CompletionStage<Admission> admit(
        String spotId,
        ZLinkSpot<?> spot,
        ZLinkRelocationTargetPolicy targetPolicy,
        ZLinkRelocationCapacityPlan capacityPlan,
        ZLinkStoreCancellation cancellation) {
        List<String> actorIds = actors.actorIdsInSpot(spotId);
        return readInventory(spotId, actorIds, cancellation)
            .thenCompose(inventory -> {
                validateLiveInventory(spot, inventory);
                return listDescriptors(cancellation)
                    .thenApply(descriptors -> new Admission(
                        inventory,
                        selectTarget(
                            inventory,
                            descriptors,
                            targetPolicy,
                            capacityPlan),
                        descriptors));
            });
    }

    int unresolvedPreparationCount() {
        return unresolved.size();
    }

    private CompletionStage<PreparedSource> sealAndCapture(
        ZLinkSpot<?> spot,
        Admission admission,
        ZLinkStoreCancellation cancellation) {
        AtomicReference<ZLinkRelocationPermitPool.Lease> acquired =
            new AtomicReference<>();
        int captures = snapshotCount(admission.inventory());
        ZLinkUserSpotRelocationBarrier barrier = spots.relocationBarrier(
            admission.inventory().spot().id(), actors);
        return barrier.sealForRelocation(preview -> {
                if (!preview.participantActorIds().equals(
                        admission.inventory().actorIds())
                    || cancellation.isCancellationRequested()) {
                    return false;
                }
                long estimate = estimatePayload(preview, captures);
                ZLinkRelocationPermitPool.Lease lease = permits.tryAcquire(
                    ZLinkRelocationPermitPool.Request.outboundAggregate(
                        estimate,
                        captures,
                        true));
                acquired.set(lease);
                return lease != null;
            }, cancellation::isCancellationRequested)
            .thenCompose(sealed -> {
                if (sealed.isEmpty()) {
                    close(acquired.get());
                    return cancellation.isCancellationRequested()
                        ? cancelled()
                        : failed(new IllegalStateException(
                            "User Spot relocation seal or permit was unavailable"));
                }
                ZLinkRelocationPermitPool.Lease lease = acquired.get();
                if (lease == null) {
                    barrier.abort(sealed.orElseThrow());
                    return failed(new IllegalStateException(
                        "User Spot relocation permit was not acquired"));
                }
                return captureSealed(
                    spot,
                    admission,
                    cancellation,
                    barrier,
                    sealed.orElseThrow(),
                    lease);
            });
    }

    private CompletionStage<PreparedSource> captureSealed(
        ZLinkSpot<?> spot,
        Admission admission,
        ZLinkStoreCancellation cancellation,
        ZLinkUserSpotRelocationBarrier barrier,
        ZLinkUserSpotRelocationBarrier.Seal seal,
        ZLinkRelocationPermitPool.Lease lease) {
        return readInventory(
                admission.inventory().spot().id(),
                seal.participantActorIds(),
                cancellation)
            .thenCompose(current -> {
                if (!sameInventory(admission.inventory(), current)) {
                    return failed(new IllegalStateException(
                        "User Spot authority changed before Capture"));
                }
                return barrier.runCapture(
                    seal,
                    () -> capture(
                        spot,
                        current,
                        seal,
                        admission.target(),
                        admission.descriptors(),
                        cancellation));
            })
            .thenCompose(captured -> {
                UUID aggregateId = UUID.randomUUID();
                List<ZLinkSpotRetireControl.ParticipantFence> inventory =
                    participantFences(captured);
                byte[] root = ZLinkCanonicalUserSpotRelocationEnvelope.encode(
                    captured.staging(),
                    aggregateId,
                    captured.inventory().spot().snapshot()
                        .authorityOwnerGeneration(),
                    inventory);
                if (!lease.tryShrinkPayload(root.length)) {
                    return failed(new IllegalStateException(
                        "captured relocation root exceeded its permit"));
                }
                var authorityRequest = relocationRequest(
                    captured,
                    root,
                    admission.target(),
                    aggregateId);
                return coordinator.stageRoot(authorityRequest, cancellation)
                    .thenApply(stagedRoot -> new PreparedSource(
                        coordinator,
                        barrier,
                        seal,
                        lease,
                        captured,
                        authorityRequest,
                        stagedRoot,
                        spots,
                        actors,
                        relocationReplies,
                        stageRequest(
                            captured,
                            authorityRequest,
                            stagedRoot.stored(),
                            admission.target())));
            })
            .exceptionallyCompose(failure -> {
                Throwable cause = unwrap(failure);
                if (cause instanceof ZLinkAggregateRelocationCoordinator
                    .PreparationOutcomeUnknownException) {
                    unresolved.add(new UnresolvedPreparation(
                        admission.inventory().spot().id(), seal, lease));
                    return failed(cause);
                }
                try {
                    barrier.abort(seal);
                    if (relocationReplies != null) {
                        admission.inventory().actorIds().forEach(
                            relocationReplies
                                ::resumeActorTimersAfterRelocationAbort);
                    }
                } finally {
                    lease.close();
                }
                return failed(cause);
            });
    }

    private CompletionStage<Captured> capture(
        ZLinkSpot<?> spot,
        Inventory inventory,
        ZLinkUserSpotRelocationBarrier.Seal seal,
        ZLinkMeshNodeDescriptor target,
        List<ZLinkMeshNodeDescriptor> descriptors,
        ZLinkStoreCancellation cancellation) {
        ZLinkRelocationCancellation relocationCancellation =
            cancellation::isCancellationRequested;
        CompletionStage<byte[]> spotState = captureState(
            inventory.spot().stableType(),
            inventory.spot().policy(),
            () -> adapters.captureSpot(
                inventory.spot().stableType(),
                spot,
                relocationCancellation));
        return spotState.thenCompose(state -> {
            List<ZLinkUserSpotAggregateStagingOwner.ActorParticipant>
                participants = new ArrayList<>();
            CompletionStage<Void> chain = CompletableFuture.completedFuture(null);
            for (Owned actor : inventory.actors()) {
                chain = chain.thenCompose(ignored -> {
                    ZLinkActor live = actors.localActor(actor.id())
                        .orElseThrow(() -> new IllegalStateException(
                            "Actor disappeared during Capture: " + actor.id()));
                    return captureState(
                            actor.stableType(),
                            actor.policy(),
                            () -> adapters.captureActor(
                                actor.stableType(),
                                live,
                                relocationCancellation))
                        .thenAccept(actorState -> participants.add(
                            new ZLinkUserSpotAggregateStagingOwner.ActorParticipant(
                                actor.id(),
                                actor.stableType(),
                                actorState,
                                isSnapshot(actor.policy()),
                                actors.actorRef(actor.id()),
                                relocationReplies == null
                                    ? ZLinkSpotTimerRelocationEnvelope
                                        .encodeCanonical(List.of())
                                    : relocationReplies
                                        .freezeActorTimerRelocationEnvelope(
                                            actor.id()))));
                });
            }
            return chain.thenApply(ignored -> new Captured(
                inventory,
                new ZLinkUserSpotAggregateStagingOwner.Request(
                    spotFactories.get(inventory.spot().stableType()).spotType(),
                    inventory.spot().stableType(),
                    inventory.spot().id(),
                    inventory.spot().snapshot().objectGeneration(),
                    state,
                    isSnapshot(inventory.spot().policy()),
                    seal.timerEnvelope(),
                    participants,
                    seal.capturedRecords()),
                sessionRoutes(
                    inventory,
                    descriptors,
                    seal.capturedRecords())));
        });
    }

    private List<ZLinkSpotRetireControl.SessionRouteFence> sessionRoutes(
        Inventory inventory,
        List<ZLinkMeshNodeDescriptor> descriptors,
        Map<String, List<systems.zlink.framework.execution
            .ZLinkAsyncSerialQueue.QueuedRecord>> acceptedRecords) {
        List<ZLinkSpotRetireControl.SessionRouteFence> routes =
            new ArrayList<>();
        for (Owned actor : inventory.actors()) {
            var knownRoute = actors.boundSessionRoute(actor.id());
            knownRoute.ifPresent(route -> {
                ZLinkMeshNodeDescriptor owner = descriptors.stream()
                    .filter(value -> value.rid().equals(
                        route.sessionOwnerNodeRid()))
                    .findFirst()
                    .orElseThrow(() -> new IllegalStateException(
                        "bound Session owner descriptor is unavailable: "
                            + route.sessionOwnerNodeRid()));
                routes.add(new ZLinkSpotRetireControl.SessionRouteFence(
                    actor.id(),
                    actor.snapshot().objectGeneration(),
                    actor.snapshot().authorityOwnerGeneration(),
                    actor.snapshot().storeVersion(),
                    route.sessionOwnerNodeRid(),
                    owner.lifecycleGeneration(),
                    owner.ownerId(),
                    owner.leaseGeneration(),
                    route.sessionRid(),
                    route.bindingGeneration(),
                    route.lastAcceptedSessionSequence()));
            });
        }
        for (byte[] encoded : acceptedRecords.values().stream()
            .flatMap(List::stream)
            .map(systems.zlink.framework.execution
                .ZLinkAsyncSerialQueue.QueuedRecord::payload)
            .toList()) {
            ZLinkActorAcceptedJournal.Record record;
            try {
                record = ZLinkActorAcceptedJournal.decode(encoded);
            } catch (IllegalArgumentException notActorRecord) {
                continue;
            }
            if (record.sourceSessionRid() == null
                || routes.stream().anyMatch(route ->
                    route.actorId().equals(record.actorId()))) {
                continue;
            }
            Owned actor = inventory.actors().stream()
                .filter(value -> value.id().equals(record.actorId()))
                .findFirst()
                .orElseThrow(() -> new IllegalStateException(
                    "bound Session record is outside Actor inventory: "
                        + record.actorId()));
            routes.add(new ZLinkSpotRetireControl.SessionRouteFence(
                actor.id(),
                actor.snapshot().objectGeneration(),
                actor.snapshot().authorityOwnerGeneration(),
                actor.snapshot().storeVersion(),
                record.sourceNodeRid(),
                record.sourceNodeGeneration(),
                record.sourceOwnerId(),
                record.sourceOwnerLeaseGeneration(),
                record.sourceSessionRid(),
                record.sourceBindingGeneration(),
                record.sourceSessionSequence()));
        }
        routes.sort((left, right) -> Arrays.compareUnsigned(
            left.actorId().getBytes(java.nio.charset.StandardCharsets.UTF_8),
            right.actorId().getBytes(java.nio.charset.StandardCharsets.UTF_8)));
        return List.copyOf(routes);
    }

    private static CompletionStage<byte[]> captureState(
        String stableType,
        RelocationPolicy policy,
        java.util.function.Supplier<CompletionStage<byte[]>> capture) {
        if (policy instanceof RelocationPolicy.Recreate) {
            return CompletableFuture.completedFuture(new byte[0]);
        }
        if (policy
            instanceof RelocationPolicy.PreserveState) {
            return Objects.requireNonNull(
                    capture.get(),
                    "Capture returned null")
                .thenApply(value -> Objects.requireNonNull(
                    value,
                    "Capture returned null state").clone());
        }
        return failed(new IllegalStateException(
            "relocation is disabled for stable type: " + stableType));
    }

    private CompletionStage<Inventory> readInventory(
        String spotId,
        List<String> actorIds,
        ZLinkStoreCancellation cancellation) {
        return readOwned(
                ZLinkAuthorityKeyCodec.spot(spotId),
                spotId,
                ZLinkPlacementObjectKind.USER_SPOT,
                cancellation)
            .thenCompose(spot -> {
                List<Owned> actorRows = new ArrayList<>();
                CompletionStage<Void> chain = CompletableFuture.completedFuture(null);
                for (String actorId : actorIds) {
                    chain = chain.thenCompose(ignored -> readOwned(
                            ZLinkAuthorityKeyCodec.actor(actorId),
                            actorId,
                            ZLinkPlacementObjectKind.ACTOR,
                            cancellation)
                        .thenAccept(actorRows::add));
                }
                return chain.thenApply(ignored -> new Inventory(
                    withPolicy(spot),
                    actorRows.stream().map(this::withPolicy).toList()));
            });
    }

    private CompletionStage<Owned> readOwned(
        String key,
        String id,
        ZLinkPlacementObjectKind kind,
        ZLinkStoreCancellation cancellation) {
        if (cancellation.isCancellationRequested()) {
            return cancelled();
        }
        return locations.read(key, cancellation).thenCompose(read -> {
            if (!(read instanceof ZLinkAuthoritySnapshot snapshot)
                || snapshot.allocation().state()
                    != ZLinkPlacementAllocationState.ACTIVE
                || snapshot.allocation().objectKind() != kind
                || !snapshot.allocation().descriptor().meshName()
                    .equals(meshName)
                || !snapshot.allocation().descriptor().rid()
                    .equals(localNodeRid)
                || snapshot.allocation().descriptorLifecycleGeneration()
                    != localNodeGeneration) {
                return failed(new IllegalStateException(
                    "relocation authority is not Ready on the source: " + key));
            }
            return CompletableFuture.completedFuture(new Owned(
                key,
                id,
                snapshot.allocation().stableType(),
                snapshot,
                null));
        });
    }

    private Owned withPolicy(Owned owned) {
        RelocationPolicy policy;
        if (owned.snapshot().allocation().objectKind()
            == ZLinkPlacementObjectKind.USER_SPOT) {
            var factory = spotFactories.get(owned.stableType());
            policy = factory == null ? null : factory.relocationPolicy();
        } else {
            var factory = actorFactories.get(owned.stableType());
            policy = factory == null ? null : factory.relocationPolicy();
        }
        if (policy == null
            || policy
                instanceof RelocationPolicy.Disabled) {
            throw new IllegalStateException(
                "relocation policy is unavailable: " + owned.stableType());
        }
        return new Owned(
            owned.key(),
            owned.id(),
            owned.stableType(),
            owned.snapshot(),
            policy);
    }

    private void validateLiveInventory(ZLinkSpot<?> spot, Inventory inventory) {
        var authority = spotAuthorities.decode(inventory.spot().snapshot().payload())
            .orElseThrow(() -> new IllegalStateException(
                "User Spot authority payload is invalid"));
        if (authority.kind() != ZLinkServiceAuthorityPayloadCodec.Kind.USER
            || authority.state()
                != ZLinkServiceAuthorityPayloadCodec.State.READY
            || !authority.spotId().equals(inventory.spot().id())
            || !authority.stableType().equals(inventory.spot().stableType())
            || !authority.meshName().equals(meshName)
            || !authority.nodeRid().equals(localNodeRid)
            || authority.nodeGeneration() != localNodeGeneration
            || !spotFactories.get(inventory.spot().stableType())
                .spotType().isInstance(spot)) {
            throw new IllegalStateException(
                "live User Spot does not match Location authority");
        }
        for (Owned actor : inventory.actors()) {
            if (!actors.actorType(actor.id()).equals(actor.stableType())
                || actors.actorRef(actor.id()).generation()
                    != actor.snapshot().objectGeneration()
                || !actor.snapshot().ownerId().equals(
                    inventory.spot().snapshot().ownerId())
                || actor.snapshot().ownerLeaseGeneration()
                    != inventory.spot().snapshot().ownerLeaseGeneration()) {
                throw new IllegalStateException(
                    "live Actor does not match aggregate authority: "
                        + actor.id());
            }
        }
    }

    private CompletionStage<List<ZLinkMeshNodeDescriptor>> listDescriptors(
        ZLinkStoreCancellation cancellation) {
        List<ZLinkMeshNodeDescriptor> result = new ArrayList<>();
        return listDescriptorPage(null, result, cancellation);
    }

    private CompletionStage<List<ZLinkMeshNodeDescriptor>> listDescriptorPage(
        String cursor,
        List<ZLinkMeshNodeDescriptor> result,
        ZLinkStoreCancellation cancellation) {
        if (cancellation.isCancellationRequested()) {
            return cancelled();
        }
        return locations.listMeshNodes(
                meshName,
                new ZLinkPageRequest(PAGE_SIZE, cursor))
            .thenCompose(page -> {
                result.addAll(page.items());
                if (result.size() > MAX_DESCRIPTORS) {
                    return failed(new IllegalStateException(
                        "MeshNode descriptor inventory exceeds its bound"));
                }
                String next = page.continuationToken();
                if (next == null || next.isBlank()) {
                    return CompletableFuture.completedFuture(List.copyOf(result));
                }
                if (next.equals(cursor)) {
                    return failed(new IllegalStateException(
                        "MeshNode descriptor cursor did not advance"));
                }
                return listDescriptorPage(next, result, cancellation);
            });
    }

    private ZLinkMeshNodeDescriptor selectTarget(
        Inventory inventory,
        List<ZLinkMeshNodeDescriptor> descriptors,
        ZLinkRelocationTargetPolicy targetPolicy,
        ZLinkRelocationCapacityPlan capacityPlan) {
        return ZLinkRelocationTargetSelector.select(
            descriptors,
            targetPolicy,
            this::baseEligible,
            candidate -> hasCapabilities(inventory, candidate),
            candidate -> hasCapacity(inventory, candidate)
                && (capacityPlan == null
                    || capacityPlan.canReserveUserSpot(
                        candidate,
                        inventory.spot().stableType(),
                        inventory.actors().size())),
            "No eligible User Spot relocation target is Ready");
    }

    private boolean baseEligible(ZLinkMeshNodeDescriptor candidate) {
        if (!candidate.meshName().equals(meshName)
            || candidate.rid().equals(localNodeRid)
            || candidate.state() != ZLinkFrameworkRuntimeState.SERVING
            || candidate.objectRole() != ZLinkMeshNodeObjectRole.SERVER) {
            return false;
        }
        return true;
    }

    private boolean hasCapabilities(
        Inventory inventory,
        ZLinkMeshNodeDescriptor candidate) {
        if (!hasCapability(candidate, inventory.spot())) {
            return false;
        }
        for (Owned actor : inventory.actors()) {
            if (!hasCapability(candidate, actor)) {
                return false;
            }
        }
        return true;
    }

    private static boolean hasCapacity(
        Inventory inventory,
        ZLinkMeshNodeDescriptor candidate) {
        return hasCapacity(candidate.capacity().spots(), 1)
            && hasCapacity(
                candidate.capacity().actors(),
                inventory.actors().size())
            && hasTypeCapacity(
                candidate,
                ZLinkPlacementObjectKind.USER_SPOT,
                inventory.spot().stableType(),
                1)
            && (candidate.activationConcurrency().limit() == 0
                || candidate.activationConcurrency().active()
                    < candidate.activationConcurrency().limit());
    }

    private static boolean hasCapability(
        ZLinkMeshNodeDescriptor target,
        Owned participant) {
        ZLinkPlacementObjectKind kind =
            participant.snapshot().allocation().objectKind();
        ZLinkObjectMaintenancePolicyKind policy = policyKind(
            participant.policy());
        return target.objectCapabilities().stream().anyMatch(capability ->
            capability.objectKind() == kind
                && capability.stableType().equals(participant.stableType())
                && capability.policy() == policy
                && capability.hasSnapshotAdapter() == isSnapshot(
                    participant.policy()));
    }

    private static boolean hasCapacity(ZLinkCapacityUsage usage, int required) {
        return usage.limit() == 0
            || (long) usage.active() + usage.reserved() + required
                <= usage.limit();
    }

    private static boolean hasTypeCapacity(
        ZLinkMeshNodeDescriptor target,
        ZLinkPlacementObjectKind kind,
        String stableType,
        int required) {
        return target.capacity().spotTypes().stream()
            .filter(type -> type.objectKind() == kind
                && type.stableType().equals(stableType))
            .findFirst()
            .map(type -> hasCapacity(type.usage(), required))
            .orElse(true);
    }

    private static boolean sameInventory(Inventory expected, Inventory actual) {
        if (!sameOwned(expected.spot(), actual.spot())
            || !expected.actorIds().equals(actual.actorIds())) {
            return false;
        }
        for (int index = 0; index < expected.actors().size(); index++) {
            if (!sameOwned(expected.actors().get(index), actual.actors().get(index))) {
                return false;
            }
        }
        return true;
    }

    private static boolean sameOwned(Owned expected, Owned actual) {
        return expected.key().equals(actual.key())
            && expected.stableType().equals(actual.stableType())
            && expected.snapshot().storeVersion().equals(
                actual.snapshot().storeVersion())
            && expected.snapshot().objectGeneration()
                == actual.snapshot().objectGeneration()
            && expected.snapshot().authorityOwnerGeneration()
                == actual.snapshot().authorityOwnerGeneration()
            && expected.snapshot().ownerId().equals(actual.snapshot().ownerId())
            && expected.snapshot().ownerLeaseGeneration()
                == actual.snapshot().ownerLeaseGeneration()
            && Arrays.equals(
                expected.snapshot().payload(),
                actual.snapshot().payload());
    }

    private static int snapshotCount(Inventory inventory) {
        int count = isSnapshot(inventory.spot().policy()) ? 1 : 0;
        for (Owned actor : inventory.actors()) {
            if (isSnapshot(actor.policy())) {
                count++;
            }
        }
        return count;
    }

    private static long estimatePayload(
        ZLinkUserSpotRelocationBarrier.Preview preview,
        int snapshotCount) {
        long bytes = ENVELOPE_RESERVATION_BYTES;
        bytes = Math.addExact(bytes, preview.timerEnvelope().length);
        for (var records : preview.capturedRecords().values()) {
            for (var record : records) {
                bytes = Math.addExact(bytes, 16L + record.payload().length);
            }
        }
        return Math.addExact(
            bytes,
            Math.multiplyExact(SNAPSHOT_RESERVATION_BYTES, snapshotCount));
    }

    private static ZLinkObjectMaintenancePolicyKind policyKind(
        RelocationPolicy policy) {
        if (policy
            instanceof RelocationPolicy.PreserveState) {
            return ZLinkObjectMaintenancePolicyKind.SNAPSHOT;
        }
        if (policy instanceof RelocationPolicy.Recreate) {
            return ZLinkObjectMaintenancePolicyKind.RECREATE;
        }
        return ZLinkObjectMaintenancePolicyKind.DISABLED;
    }

    private static boolean isSnapshot(
        RelocationPolicy policy) {
        return policy
            instanceof RelocationPolicy.PreserveState;
    }

    private static ZLinkAggregateRelocationCoordinator.Request
        relocationRequest(
            Captured captured,
            byte[] root,
            ZLinkMeshNodeDescriptor target,
            UUID aggregateId) {
        List<ZLinkAggregateRelocationCoordinator.Participant> participants =
            new ArrayList<>();
        participants.add(participant(
            captured.inventory().spot(),
            SOURCE_CLEANUP_PENDING));
        for (Owned actor : captured.inventory().actors()) {
            participants.add(participant(actor, new byte[0]));
        }
        return new ZLinkAggregateRelocationCoordinator.Request(
            aggregateId,
            1,
            participants,
            root,
            new ZLinkMeshNodeDescriptorKey(target.meshName(), target.rid()),
            target.lifecycleGeneration(),
            new ZLinkPlacementCapacityBundle(
                captured.inventory().actors().size(),
                1,
                Optional.of(new ZLinkSpotTypeCapacityDelta(
                    ZLinkPlacementObjectKind.USER_SPOT,
                    captured.inventory().spot().stableType(),
                    1))),
            new ZLinkLocationOwnerToken(
                target.ownerId(),
                target.leaseGeneration()));
    }

    private static ZLinkAggregateRelocationCoordinator.Participant participant(
        Owned owned,
        byte[] completion) {
        return new ZLinkAggregateRelocationCoordinator.Participant(
            owned.key(),
            owned.snapshot().allocation().objectKind(),
            owned.snapshot().objectGeneration(),
            owned.snapshot().authorityOwnerGeneration(),
            owned.snapshot().storeVersion(),
            ZLinkAuthorityGenerationTransition.NEW_OWNER,
            owned.snapshot().payload(),
            completion);
    }

    private static List<ZLinkSpotRetireControl.ParticipantFence>
        participantFences(Captured captured) {
        return java.util.stream.Stream.concat(
                    java.util.stream.Stream.of(captured.inventory().spot()),
                    captured.inventory().actors().stream())
                .map(owned -> new ZLinkSpotRetireControl.ParticipantFence(
                    owned.key(),
                    owned.snapshot().allocation().objectKind()
                        == ZLinkPlacementObjectKind.ACTOR ? 1 : 2,
                    owned.id(),
                    owned.stableType(),
                    isSnapshot(owned.policy()),
                    owned.snapshot().objectGeneration(),
                    owned.snapshot().authorityOwnerGeneration()))
                .sorted((left, right) -> Arrays.compareUnsigned(
                    left.authorityKey().getBytes(
                        java.nio.charset.StandardCharsets.UTF_8),
                    right.authorityKey().getBytes(
                        java.nio.charset.StandardCharsets.UTF_8)))
                .toList();
    }

    private ZLinkSpotRetireControl.StageRequest stageRequest(
        Captured captured,
        ZLinkAggregateRelocationCoordinator.Request authorityRequest,
        ZLinkRelocationStored stored,
        ZLinkMeshNodeDescriptor target) {
        ZLinkAuthoritySnapshot source =
            captured.inventory().spot().snapshot();
        return new ZLinkSpotRetireControl.StageRequest(
            new ZLinkSpotRetireControl.Fence(
                authorityRequest.aggregateId(),
                authorityRequest.aggregateGeneration()),
            localNodeRid,
            localNodeGeneration,
            source.ownerId(),
            source.ownerLeaseGeneration(),
            target.rid(),
            target.lifecycleGeneration(),
            target.ownerId(),
            target.leaseGeneration(),
            meshName,
            captured.inventory().spot().id(),
            captured.inventory().spot().stableType(),
            false,
            captured.staging().restoreSpotSnapshot(),
            stored.reference(),
            stored.checksumCrc32c(),
            participantFences(captured),
            captured.sessionRoutes());
    }

    static final class PreparedSource {
        private final ZLinkAggregateRelocationCoordinator coordinator;
        private final ZLinkUserSpotRelocationBarrier barrier;
        private final ZLinkUserSpotRelocationBarrier.Seal seal;
        private final ZLinkRelocationPermitPool.Lease permit;
        private final Captured captured;
        private final ZLinkAggregateRelocationCoordinator.Request
            authorityRequest;
        private final ZLinkAggregateRelocationCoordinator.StagedRoot
            stagedRoot;
        private final ZLinkSpotLifecycle spots;
        private final ZLinkActorSessionCoordinator actors;
        private final ZLinkSpotRuntime relocationReplies;
        private final ZLinkSpotRetireControl.StageRequest stageRequest;
        private ZLinkAggregateRelocationCoordinator.Prepared finalPrepared;
        private boolean finalJournalEmpty;
        private Map<String, List<ZLinkAsyncSerialQueue.QueuedRecord>>
            finalJournal = Map.of();
        private boolean sourceCommitted;
        private boolean terminal;

        private PreparedSource(
            ZLinkAggregateRelocationCoordinator coordinator,
            ZLinkUserSpotRelocationBarrier barrier,
            ZLinkUserSpotRelocationBarrier.Seal seal,
            ZLinkRelocationPermitPool.Lease permit,
            Captured captured,
            ZLinkAggregateRelocationCoordinator.Request authorityRequest,
            ZLinkAggregateRelocationCoordinator.StagedRoot stagedRoot,
            ZLinkSpotLifecycle spots,
            ZLinkActorSessionCoordinator actors,
            ZLinkSpotRuntime relocationReplies,
            ZLinkSpotRetireControl.StageRequest stageRequest) {
            this.coordinator = coordinator;
            this.barrier = barrier;
            this.seal = seal;
            this.permit = permit;
            this.captured = captured;
            this.authorityRequest = authorityRequest;
            this.stagedRoot = stagedRoot;
            this.spots = Objects.requireNonNull(spots, "spots");
            this.actors = Objects.requireNonNull(actors, "actors");
            this.relocationReplies = relocationReplies;
            this.stageRequest = stageRequest;
        }

        ZLinkAggregateRelocationCoordinator.StagedRoot stagedRoot() {
            return stagedRoot;
        }

        ZLinkSpotRetireControl.StageRequest stageRequest() {
            return stageRequest;
        }

        ZLinkUserSpotAggregateStagingOwner.Request stagingRequest() {
            return captured.staging();
        }

        synchronized CompletionStage<ZLinkAggregateRelocationCoordinator.Prepared>
            freezeAndPrepareFinal(ZLinkStoreCancellation cancellation) {
            if (terminal || sourceCommitted || finalPrepared != null) {
                return failed(new IllegalStateException(
                    "source relocation final root is already terminal"));
            }
            Map<String, List<ZLinkAsyncSerialQueue.QueuedRecord>> held =
                barrier.freezeIngress(seal).orElseThrow(() ->
                    new IllegalStateException(
                        "source relocation ingress freeze was lost"));
            var finalStaging = withHeldIngress(captured.staging(), held);
            byte[] finalRoot = ZLinkCanonicalUserSpotRelocationEnvelope.encode(
                finalStaging,
                authorityRequest.aggregateId(),
                captured.inventory().spot().snapshot()
                    .authorityOwnerGeneration(),
                participantFences(captured));
            var request = new ZLinkAggregateRelocationCoordinator.Request(
                authorityRequest.aggregateId(),
                authorityRequest.aggregateGeneration(),
                authorityRequest.participants(),
                finalRoot,
                authorityRequest.targetDescriptor(),
                authorityRequest.targetDescriptorLifecycleGeneration(),
                authorityRequest.capacityBundle(),
                authorityRequest.targetOwner());
            return coordinator.prepare(request, cancellation)
                .thenApply(prepared -> {
                    synchronized (PreparedSource.this) {
                        finalPrepared = prepared;
                        finalJournal = finalStaging.acceptedJournal();
                        finalJournalEmpty = finalStaging.acceptedJournal()
                            .values().stream().allMatch(List::isEmpty);
                    }
                    return prepared;
                });
        }

        synchronized CompletionStage<Void> requireReplaySupport() {
            if (finalPrepared == null) {
                return failed(new IllegalStateException(
                    "source relocation final root is not prepared"));
            }
            return CompletableFuture.completedFuture(null);
        }

        synchronized ZLinkUserSpotRelocationBarrier.Committed
            commitSourceBarrier(Map<String, Long> targetOwnerGenerations) {
            if (terminal || sourceCommitted || finalPrepared == null) {
                throw new IllegalStateException(
                    "source relocation final root is not prepared or is terminal");
            }
            Objects.requireNonNull(
                targetOwnerGenerations,
                "targetOwnerGenerations");
            Map<String, ZLinkSpotRelocationReplyRoutes.CommittedFence>
                replyFences = new java.util.LinkedHashMap<>();
            for (int index = 0;
                index < stageRequest.participants().size();
                index++) {
                var participant = stageRequest.participants().get(index);
                replyFences.put(
                    participant.objectId().equals(stageRequest.spotId())
                        ? "spot"
                        : participant.objectId(),
                    new ZLinkSpotRelocationReplyRoutes.CommittedFence(
                        participant.authorityKey(),
                        index + 1L,
                        targetOwnerGeneration(
                            targetOwnerGenerations,
                            participant.authorityKey())));
            }
            if (relocationReplies == null) {
                throw new IllegalStateException(
                    "relocation reply runtime is unavailable");
            }
            relocationReplies.bindCanonicalRelocationReplies(
                finalJournal,
                stageRequest.targetNodeRid(),
                stageRequest.targetNodeGeneration(),
                Map.copyOf(replyFences));
            ZLinkUserSpotRelocationBarrier.Committed committed =
                barrier.commit(seal).orElseThrow(() ->
                    new IllegalStateException(
                        "source relocation barrier was lost"));
            sourceCommitted = true;
            return committed;
        }

        synchronized CompletionStage<
            ZLinkAggregateRelocationCoordinator.Published> commitAuthority(
                ZLinkStoreCancellation cancellation) {
            if (sourceCommitted || terminal || finalPrepared == null) {
                return failed(new IllegalStateException(
                    "source relocation is not ready for authority commit"));
            }
            return coordinator.commit(finalPrepared, cancellation);
        }

        synchronized CompletionStage<
            ZLinkAggregateRelocationCoordinator.Published>
                completeSourceCleanup(
                    ZLinkAggregateRelocationCoordinator.Published published,
                    byte[] completedRoot,
                    ZLinkStoreCancellation cancellation) {
            if (!sourceCommitted || terminal) {
                return failed(new IllegalStateException(
                    "source relocation is not committed"));
            }
            return coordinator.completeSourceCleanup(
                published,
                completedRoot,
                cancellation);
        }

        synchronized CompletionStage<Void> abortPrecommit() {
            if (terminal || sourceCommitted) {
                return failed(new IllegalStateException(
                    "committed source relocation cannot be aborted"));
            }
            CompletionStage<Void> abortAuthority = finalPrepared == null
                ? CompletableFuture.completedFuture(null)
                : coordinator.abort(finalPrepared);
            return abortAuthority
                .thenCompose(ignored -> coordinator.discardStagedRoot(
                    stagedRoot))
                .thenRun(() -> {
                synchronized (PreparedSource.this) {
                    if (!barrier.abort(seal)) {
                        throw new IllegalStateException(
                            "source relocation barrier was lost during abort");
                    }
                    if (relocationReplies != null) {
                        captured.inventory().actorIds().forEach(
                            relocationReplies
                                ::resumeActorTimersAfterRelocationAbort);
                    }
                    terminal = true;
                    permit.close();
                }
            });
        }

        synchronized CompletionStage<Void> discardInitialAfterCommit() {
            if (!sourceCommitted || terminal) {
                return failed(new IllegalStateException(
                    "source relocation is not committed"));
            }
            return coordinator.discardStagedRoot(stagedRoot);
        }

        CompletionStage<Void> cleanupLocal(java.time.Instant deadline) {
            Objects.requireNonNull(deadline, "deadline");
            List<String> actorIds = captured.inventory().actorIds();
            String spotId = captured.inventory().spot().id();
            long generation = captured.inventory().spot().snapshot()
                .objectGeneration();
            if (relocationReplies != null) {
                actorIds.forEach(
                    relocationReplies::closeActorTimersAfterRelocation);
            }
            return actors.completeRelocationSource(actorIds)
                .thenCompose(ignored -> spots.completeRelocationSource(
                    spotId,
                    generation,
                    deadline));
        }

        synchronized void releasePermitAfterCompletion() {
            if (!sourceCommitted || terminal) {
                throw new IllegalStateException(
                    "source relocation is not ready to release its permit");
            }
            terminal = true;
            permit.close();
        }
    }

    private static ZLinkUserSpotAggregateStagingOwner.Request withHeldIngress(
        ZLinkUserSpotAggregateStagingOwner.Request initial,
        Map<String, List<ZLinkAsyncSerialQueue.QueuedRecord>> held) {
        LinkedHashMap<String, List<ZLinkAsyncSerialQueue.QueuedRecord>> journal =
            new LinkedHashMap<>();
        initial.acceptedJournal().forEach((lane, records) ->
            journal.put(lane, new ArrayList<>(records)));
        held.forEach((lane, records) -> journal
            .computeIfAbsent(lane, ignored -> new ArrayList<>())
            .addAll(records));
        LinkedHashMap<String, List<ZLinkAsyncSerialQueue.QueuedRecord>> frozen =
            new LinkedHashMap<>();
        journal.forEach((lane, records) -> frozen.put(
            lane,
            List.copyOf(records)));
        return new ZLinkUserSpotAggregateStagingOwner.Request(
            initial.spotType(),
            initial.spotStableType(),
            initial.spotId(),
            initial.objectGeneration(),
            initial.spotState(),
            initial.restoreSpotSnapshot(),
            initial.timerEnvelope(),
            initial.actors(),
            frozen);
    }

    private record Admission(
        Inventory inventory,
        ZLinkMeshNodeDescriptor target,
        List<ZLinkMeshNodeDescriptor> descriptors) {
        private Admission {
            descriptors = List.copyOf(descriptors);
        }
    }

    private record Inventory(Owned spot, List<Owned> actors) {
        private Inventory {
            actors = List.copyOf(actors);
        }

        List<String> actorIds() {
            return actors.stream().map(Owned::id).toList();
        }
    }

    private record Owned(
        String key,
        String id,
        String stableType,
        ZLinkAuthoritySnapshot snapshot,
        RelocationPolicy policy) {
    }

    private record Captured(
        Inventory inventory,
        ZLinkUserSpotAggregateStagingOwner.Request staging,
        List<ZLinkSpotRetireControl.SessionRouteFence> sessionRoutes) {
        private Captured {
            sessionRoutes = List.copyOf(sessionRoutes);
        }
    }

    private record UnresolvedPreparation(
        String spotId,
        ZLinkUserSpotRelocationBarrier.Seal seal,
        ZLinkRelocationPermitPool.Lease permit) {
    }

    private static void close(AutoCloseable closeable) {
        if (closeable == null) {
            return;
        }
        try {
            closeable.close();
        } catch (Exception ignored) {
        }
    }

    private static Throwable unwrap(Throwable failure) {
        Throwable current = failure;
        while (current instanceof CompletionException
            && current.getCause() != null) {
            current = current.getCause();
        }
        return current;
    }

    private static <T> CompletionStage<T> cancelled() {
        return failed(new java.util.concurrent.CancellationException());
    }

    private static <T> CompletionStage<T> failed(Throwable failure) {
        return CompletableFuture.failedFuture(failure);
    }

    private static long targetOwnerGeneration(
        Map<String, Long> generations,
        String authorityKey) {
        Long generation = generations.get(authorityKey);
        if (generation == null || generation <= 0) {
            throw new IllegalArgumentException(
                "committed owner generation is absent: " + authorityKey);
        }
        return generation;
    }
}
