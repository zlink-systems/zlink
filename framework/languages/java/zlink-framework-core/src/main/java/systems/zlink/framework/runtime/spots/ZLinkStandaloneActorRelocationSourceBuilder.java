package systems.zlink.framework.runtime.spots;

import java.util.ArrayList;
import java.util.List;
import java.util.Map;
import java.util.Objects;
import java.util.Optional;
import java.util.UUID;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionException;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.ThreadLocalRandom;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.actors.ZLinkActor;
import systems.zlink.framework.actors.ZLinkRelocationCancellation;
import systems.zlink.framework.execution.ZLinkAsyncSerialQueue;
import systems.zlink.framework.locations.*;
import systems.zlink.framework.runtime.internal.locations.*;
import systems.zlink.framework.runtime.host.ZLinkFrameworkRuntimeState;
import systems.zlink.framework.runtime.internal.locations
    .ZLinkAggregateRelocationCoordinator;
import systems.zlink.framework.runtime.internal.locations
    .ZLinkLocationRepository;
import systems.zlink.framework.runtime.internal.locations
    .ZLinkRelocationPermitPool;
import systems.zlink.framework.runtime.internal.relocation
    .ZLinkRelocationAdapterRegistry;
import systems.zlink.framework.runtime.locations.ZLinkActorAuthorityPayloadCodec;
import systems.zlink.framework.runtime.locations.ZLinkAuthorityKeyCodec;
import systems.zlink.framework.runtime.mesh.MeshNodeRegistration;
import systems.zlink.framework.runtime.internal.configuration
    .ZLinkObjectFactoryRegistration.RelocatableActorFactory;
import systems.zlink.framework.runtime.internal.configuration
    .ZLinkObjectFactoryRegistration.RelocationPolicy;

/**
 * Builds the reversible source half of one Entry Spot Actor relocation from
 * the live Actor and its exact authority row.
 */
final class ZLinkStandaloneActorRelocationSourceBuilder {
    private static final int PAGE_SIZE = 1000;
    private static final int MAX_DESCRIPTORS = 65_536;
    private static final long SNAPSHOT_RESERVATION_BYTES = 64L * 1024 * 1024;
    private static final long ENVELOPE_RESERVATION_BYTES = 64L * 1024;

    private final String meshName;
    private final RoutingId localNodeRid;
    private final long localNodeGeneration;
    private final ZLinkLocationRepository locations;
    private final ZLinkAggregateRelocationCoordinator coordinator;
    private final ZLinkRelocationPermitPool permits;
    private final ZLinkActorSessionCoordinator actors;
    private final ZLinkRelocationAdapterRegistry adapters;
    private final Map<String, RelocatableActorFactory<?>>
        factories;
    private final ZLinkSpotRuntime relocationReplies;
    private final ZLinkActorAuthorityPayloadCodec authorities =
        new ZLinkActorAuthorityPayloadCodec();

    ZLinkStandaloneActorRelocationSourceBuilder(
        String meshName,
        RoutingId localNodeRid,
        long localNodeGeneration,
        ZLinkLocationRepository locations,
        ZLinkAggregateRelocationCoordinator coordinator,
        ZLinkRelocationPermitPool permits,
        ZLinkActorSessionCoordinator actors,
        ZLinkRelocationAdapterRegistry adapters,
        Map<String, RelocatableActorFactory<?>>
            factories,
        ZLinkSpotRuntime relocationReplies) {
        this.meshName = requireText(meshName, "meshName");
        this.localNodeRid = Objects.requireNonNull(
            localNodeRid, "localNodeRid");
        if (localNodeGeneration <= 0) {
            throw new IllegalArgumentException(
                "local node generation must be positive");
        }
        this.localNodeGeneration = localNodeGeneration;
        this.locations = Objects.requireNonNull(locations, "locations");
        this.coordinator = Objects.requireNonNull(coordinator, "coordinator");
        this.permits = Objects.requireNonNull(permits, "permits");
        this.actors = Objects.requireNonNull(actors, "actors");
        this.adapters = Objects.requireNonNull(adapters, "adapters");
        this.factories = Map.copyOf(
            Objects.requireNonNull(factories, "factories"));
        this.relocationReplies = Objects.requireNonNull(
            relocationReplies, "relocationReplies");
    }

    CompletionStage<PreparedSource> prepare(
        String actorId,
        ZLinkRelocationTargetPolicy targetPolicy,
        ZLinkStoreCancellation cancellation) {
        Objects.requireNonNull(targetPolicy, "targetPolicy");
        Objects.requireNonNull(cancellation, "cancellation");
        ZLinkActor actor = activeActor(actorId, cancellation);
        if (actor == null) {
            return cancellation.isCancellationRequested()
                ? cancelled()
                : failed(new IllegalStateException(
                    "Actor is not active locally: " + actorId));
        }
        return admit(actorId, targetPolicy, cancellation)
            .thenCompose(admission -> sealAndCapture(
                actor, admission, cancellation));
    }

    CompletionStage<Void> preflight(
        String actorId,
        ZLinkRelocationTargetPolicy targetPolicy,
        ZLinkRelocationCapacityPlan capacityPlan,
        ZLinkStoreCancellation cancellation) {
        Objects.requireNonNull(targetPolicy, "targetPolicy");
        Objects.requireNonNull(capacityPlan, "capacityPlan");
        Objects.requireNonNull(cancellation, "cancellation");
        ZLinkActor actor = activeActor(actorId, cancellation);
        if (actor == null) {
            return cancellation.isCancellationRequested()
                ? cancelled()
                : failed(new IllegalStateException(
                    "Actor is not active locally: " + actorId));
        }
        return admit(
            actorId,
            targetPolicy,
            capacityPlan,
            cancellation).thenApply(admission -> {
                capacityPlan.reserveActor(actorId, admission.target());
                return null;
            });
    }

    private ZLinkActor activeActor(
        String actorId,
        ZLinkStoreCancellation cancellation) {
        if (cancellation.isCancellationRequested()) {
            return null;
        }
        return actors.localActor(actorId).orElse(null);
    }

    private CompletionStage<Admission> admit(
        String actorId,
        ZLinkRelocationTargetPolicy targetPolicy,
        ZLinkStoreCancellation cancellation) {
        return admit(actorId, targetPolicy, null, cancellation);
    }

    CompletionStage<PreparedSource> preparePinned(
        String actorId,
        ZLinkRelocationTargetPolicy targetPolicy,
        ZLinkMeshNodeDescriptor pinnedTarget,
        ZLinkStoreCancellation cancellation) {
        Objects.requireNonNull(pinnedTarget, "pinnedTarget");
        ZLinkActor actor = activeActor(actorId, cancellation);
        if (actor == null) {
            return cancellation.isCancellationRequested()
                ? cancelled()
                : failed(new IllegalStateException(
                    "Actor is not active locally: " + actorId));
        }
        return readOwned(actorId, cancellation)
            .thenCompose(owned -> listDescriptors(cancellation)
                .thenApply(descriptors -> new Admission(
                    owned,
                    requirePinnedTarget(
                        pinnedTarget,
                        descriptors,
                        candidate -> hasCapability(owned, candidate)),
                    sessionRoute(owned, descriptors))))
            .thenCompose(admission -> sealAndCapture(
                actor, admission, cancellation));
    }

    private ZLinkMeshNodeDescriptor requirePinnedTarget(
        ZLinkMeshNodeDescriptor pinned,
        List<ZLinkMeshNodeDescriptor> descriptors,
        java.util.function.Predicate<ZLinkMeshNodeDescriptor> capability) {
        return descriptors.stream()
            .filter(candidate -> candidate.rid().equals(pinned.rid()))
            .filter(candidate -> candidate.lifecycleGeneration()
                == pinned.lifecycleGeneration())
            .filter(this::baseEligible)
            .filter(capability)
            .findFirst()
            .orElseThrow(() -> new ZLinkUserSpotRetireRuntime
                .RelocationBlockedException(
                    systems.zlink.framework.runtime.host
                        .ZLinkFrameworkRelocationReason.TARGET_UNAVAILABLE,
                    "Pinned Actor relocation target is no longer Ready"));
    }

    private CompletionStage<Admission> admit(
        String actorId,
        ZLinkRelocationTargetPolicy targetPolicy,
        ZLinkRelocationCapacityPlan capacityPlan,
        ZLinkStoreCancellation cancellation) {
        return readOwned(actorId, cancellation)
            .thenCompose(owned -> listDescriptors(cancellation)
                .thenApply(descriptors -> new Admission(
                    owned,
                    selectTarget(
                        owned,
                        descriptors,
                        targetPolicy,
                        capacityPlan),
                    sessionRoute(owned, descriptors))));
    }

    private CompletionStage<PreparedSource> sealAndCapture(
        ZLinkActor actor,
        Admission admission,
        ZLinkStoreCancellation cancellation) {
        return permits.acquire(
                ZLinkRelocationPermitPool.Request.outboundAggregate(
                admission.owned().snapshotPolicy()
                    ? SNAPSHOT_RESERVATION_BYTES
                    : ENVELOPE_RESERVATION_BYTES,
                admission.owned().snapshotPolicy() ? 1 : 0,
                    false),
                cancellation)
            .thenCompose(permit -> sealAndCaptureWithPermit(
                actor, admission, cancellation, permit));
    }

    private CompletionStage<PreparedSource> sealAndCaptureWithPermit(
        ZLinkActor actor,
        Admission admission,
        ZLinkStoreCancellation cancellation,
        ZLinkRelocationPermitPool.Lease permit) {
        Optional<ZLinkAsyncSerialQueue.RelocationSeal> sealed =
            actors.trySealActorRelocation(admission.owned().actorId());
        if (sealed.isEmpty()) {
            permit.close();
            return failed(new IllegalStateException(
                "Actor relocation queue cannot be sealed"));
        }
        ZLinkAsyncSerialQueue.RelocationSeal seal = sealed.orElseThrow();
        byte[] timerEnvelope =
            relocationReplies.freezeActorTimerRelocationEnvelope(
                admission.owned().actorId());
        ZLinkRelocationCancellation relocationCancellation =
            cancellation::isCancellationRequested;
        CompletionStage<byte[]> captured = admission.owned().snapshotPolicy()
            ? adapters.captureActor(
                admission.owned().stableType(),
                actor,
                relocationCancellation)
            : CompletableFuture.completedFuture(new byte[0]);
        return captured.thenCompose(state -> {
            byte[] applicationState = Objects.requireNonNull(
                state, "Actor Capture returned null").clone();
            UUID relocationId = UUID.randomUUID();
            byte[] initialRoot =
                ZLinkCanonicalActorRelocationEnvelope.encode(
                    relocationId,
                    admission.owned().actorId(),
                    admission.owned().snapshot().objectGeneration(),
                    admission.owned().snapshot()
                        .authorityOwnerGeneration(),
                    admission.owned().snapshotPolicy(),
                    applicationState,
                    seal.captured(),
                    timerEnvelope);
            var request = request(
                admission.owned(),
                admission.target(),
                relocationId,
                initialRoot);
            Optional<ZLinkSpotRetireControl.SessionRouteFence> sessionRoute =
                admission.sessionRoute().or(() ->
                    capturedSessionRoute(
                        admission.owned(),
                        seal.captured()));
            return coordinator.stageRoot(request, cancellation)
                .thenApply(staged -> new PreparedSource(
                    coordinator,
                    actors,
                    relocationReplies,
                    seal,
                    permit,
                    admission.owned(),
                    admission.target(),
                    relocationId,
                    applicationState,
                    timerEnvelope,
                    staged,
                    stageRequest(
                        admission,
                        relocationId,
                        staged,
                        sessionRoute)));
        }).exceptionallyCompose(failure -> {
            relocationReplies.resumeActorTimersAfterRelocationAbort(
                admission.owned().actorId());
            actors.abortActorRelocation(
                admission.owned().actorId(), seal);
            permit.close();
            return failed(unwrap(failure));
        });
    }

    private CompletionStage<Owned> readOwned(
        String actorId,
        ZLinkStoreCancellation cancellation) {
        String key = ZLinkAuthorityKeyCodec.actor(actorId);
        return locations.read(key, cancellation).thenCompose(read -> {
            if (!(read instanceof ZLinkAuthoritySnapshot snapshot)
                || snapshot.allocation().state()
                    != ZLinkPlacementAllocationState.ACTIVE
                || snapshot.allocation().objectKind()
                    != ZLinkPlacementObjectKind.ACTOR
                || !snapshot.allocation().descriptor().meshName()
                    .equals(meshName)
                || !snapshot.allocation().descriptor().rid()
                    .equals(localNodeRid)
                || snapshot.allocation().descriptorLifecycleGeneration()
                    != localNodeGeneration) {
                return failed(new IllegalStateException(
                    "Actor authority is not Ready on the source: " + key));
            }
            var authority = authorities.decode(snapshot.payload())
                .orElseThrow(() -> new IllegalStateException(
                    "Actor authority payload is invalid"));
            String stableType = actors.actorType(actorId);
            var factory = factories.get(stableType);
            if (factory == null
                || factory.relocationPolicy()
                    instanceof RelocationPolicy.Disabled) {
                return failed(new IllegalStateException(
                    "Actor relocation policy is unavailable: " + stableType));
            }
            if (authority.state() != ZLinkActorAuthorityPayloadCodec.State.READY
                || authority.currentSpotKind() != 1
                || !authority.actorId().equals(actorId)
                || !authority.stableType().equals(stableType)
                || !authority.nodeRid().equals(localNodeRid)
                || authority.nodeGeneration() != localNodeGeneration
                || actors.actorRef(actorId).generation()
                    != snapshot.objectGeneration()) {
                return failed(new IllegalStateException(
                    "live Entry Spot Actor differs from Location authority"));
            }
            return CompletableFuture.completedFuture(new Owned(
                key,
                actorId,
                stableType,
                snapshot,
                factory.relocationPolicy()
                    instanceof RelocationPolicy.PreserveState));
        });
    }

    private CompletionStage<List<ZLinkMeshNodeDescriptor>> listDescriptors(
        ZLinkStoreCancellation cancellation) {
        return listDescriptorPage(null, new ArrayList<>(), cancellation);
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
                    return CompletableFuture.completedFuture(
                        List.copyOf(result));
                }
                if (next.equals(cursor)) {
                    return failed(new IllegalStateException(
                        "MeshNode descriptor cursor did not advance"));
                }
                return listDescriptorPage(next, result, cancellation);
            });
    }

    private ZLinkMeshNodeDescriptor selectTarget(
        Owned actor,
        List<ZLinkMeshNodeDescriptor> descriptors,
        ZLinkRelocationTargetPolicy targetPolicy,
        ZLinkRelocationCapacityPlan capacityPlan) {
        return ZLinkRelocationTargetSelector.select(
            descriptors,
            targetPolicy,
            this::baseEligible,
            candidate -> hasCapability(actor, candidate),
            candidate -> hasCapacity(candidate)
                && (capacityPlan == null
                    || capacityPlan.canReserveActor(candidate)),
            "No eligible Entry Spot Actor relocation target is Ready");
    }

    private boolean baseEligible(ZLinkMeshNodeDescriptor candidate) {
        if (!candidate.meshName().equals(meshName)
            || candidate.rid().equals(localNodeRid)
            || candidate.state() != ZLinkFrameworkRuntimeState.SERVING
            || candidate.objectRole() != ZLinkMeshNodeObjectRole.SERVER
            || candidate.entrySpotId().isEmpty()) {
            return false;
        }
        return true;
    }

    private boolean hasCapability(
        Owned actor,
        ZLinkMeshNodeDescriptor candidate) {
        var factory = factories.get(actor.stableType());
        ZLinkObjectMaintenancePolicyKind policy =
            factory.relocationPolicy()
                instanceof RelocationPolicy.PreserveState
                ? ZLinkObjectMaintenancePolicyKind.SNAPSHOT
                : ZLinkObjectMaintenancePolicyKind.RECREATE;
        return candidate.objectCapabilities().stream().anyMatch(capability ->
            capability.objectKind() == ZLinkPlacementObjectKind.ACTOR
                && capability.stableType().equals(actor.stableType())
                && capability.policy() == policy
                && capability.hasSnapshotAdapter()
                    == actor.snapshotPolicy());
    }

    private boolean hasCapacity(ZLinkMeshNodeDescriptor candidate) {
        return hasCapacity(candidate.capacity().actors(), 1)
            && (candidate.activationConcurrency().limit() == 0
                || candidate.activationConcurrency().active()
                    < candidate.activationConcurrency().limit());
    }

    private ZLinkAggregateRelocationCoordinator.Request request(
        Owned actor,
        ZLinkMeshNodeDescriptor target,
        UUID relocationId,
        byte[] root) {
        String targetSpotId = target.entrySpotId().orElseThrow();
        byte[] targetAuthority = authorities.encode(
            ZLinkActorAuthorityPayloadCodec.State.READY,
            actor.stableType(),
            actor.actorId(),
            targetSpotId,
            target.lifecycleGeneration(),
            1,
            target.ownerId(),
            target.leaseGeneration(),
            meshName,
            target.rid(),
            target.lifecycleGeneration());
        var participant = new ZLinkAggregateRelocationCoordinator.Participant(
            actor.authorityKey(),
            ZLinkPlacementObjectKind.ACTOR,
            actor.snapshot().objectGeneration(),
            actor.snapshot().authorityOwnerGeneration(),
            actor.snapshot().storeVersion(),
            ZLinkAuthorityGenerationTransition.NEW_OWNER,
            targetAuthority,
            new byte[0]);
        return new ZLinkAggregateRelocationCoordinator.Request(
            relocationId,
            1,
            List.of(participant),
            root,
            new ZLinkMeshNodeDescriptorKey(meshName, target.rid()),
            target.lifecycleGeneration(),
            ZLinkPlacementCapacityBundle.actor(1),
            new ZLinkLocationOwnerToken(
                target.ownerId(), target.leaseGeneration()));
    }

    private ZLinkSpotRetireControl.StageRequest stageRequest(
        Admission admission,
        UUID relocationId,
        ZLinkAggregateRelocationCoordinator.StagedRoot staged,
        Optional<ZLinkSpotRetireControl.SessionRouteFence> sessionRoute) {
        Owned actor = admission.owned();
        ZLinkMeshNodeDescriptor target = admission.target();
        return new ZLinkSpotRetireControl.StageRequest(
            new ZLinkSpotRetireControl.Fence(relocationId, 1),
            localNodeRid,
            localNodeGeneration,
            actor.snapshot().ownerId(),
            actor.snapshot().ownerLeaseGeneration(),
            target.rid(),
            target.lifecycleGeneration(),
            target.ownerId(),
            target.leaseGeneration(),
            meshName,
            target.entrySpotId().orElseThrow(),
            actor.stableType(),
            false,
            actor.snapshotPolicy(),
            staged.stored().reference(),
            staged.stored().checksumCrc32c(),
            List.of(new ZLinkSpotRetireControl.ParticipantFence(
                actor.authorityKey(),
                1,
                actor.actorId(),
                actor.stableType(),
                actor.snapshotPolicy(),
                actor.snapshot().objectGeneration(),
                actor.snapshot().authorityOwnerGeneration())),
            sessionRoute.stream().toList());
    }

    private Optional<ZLinkSpotRetireControl.SessionRouteFence>
        capturedSessionRoute(
            Owned actor,
            List<ZLinkAsyncSerialQueue.QueuedRecord> captured) {
        for (ZLinkAsyncSerialQueue.QueuedRecord queued : captured) {
            ZLinkActorAcceptedJournal.Record record;
            try {
                record = ZLinkActorAcceptedJournal.decode(queued.payload());
            } catch (IllegalArgumentException notActorRecord) {
                continue;
            }
            if (!record.actorId().equals(actor.actorId())
                || record.sourceSessionRid() == null) {
                continue;
            }
            return Optional.of(
                new ZLinkSpotRetireControl.SessionRouteFence(
                    actor.actorId(),
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
        return Optional.empty();
    }

    private Optional<ZLinkSpotRetireControl.SessionRouteFence> sessionRoute(
        Owned actor,
        List<ZLinkMeshNodeDescriptor> descriptors) {
        var knownRoute = actors.boundSessionRoute(actor.actorId());
        return knownRoute.map(route -> {
            ZLinkMeshNodeDescriptor owner = descriptors.stream()
                .filter(value -> value.rid().equals(
                    route.sessionOwnerNodeRid()))
                .findFirst()
                .orElseThrow(() -> new IllegalStateException(
                    "bound Session owner descriptor is unavailable: "
                        + route.sessionOwnerNodeRid()));
            return new ZLinkSpotRetireControl.SessionRouteFence(
                actor.actorId(),
                actor.snapshot().objectGeneration(),
                actor.snapshot().authorityOwnerGeneration(),
                actor.snapshot().storeVersion(),
                route.sessionOwnerNodeRid(),
                owner.lifecycleGeneration(),
                owner.ownerId(),
                owner.leaseGeneration(),
                route.sessionRid(),
                route.bindingGeneration(),
                route.lastAcceptedSessionSequence());
        });
    }

    private static boolean hasCapacity(
        ZLinkCapacityUsage usage,
        int required) {
        return usage.limit() == 0
            || (long) usage.active() + usage.reserved() + required
                <= usage.limit();
    }

    static final class PreparedSource {
        private final ZLinkAggregateRelocationCoordinator coordinator;
        private final ZLinkActorSessionCoordinator actors;
        private final ZLinkSpotRuntime relocationReplies;
        private final ZLinkAsyncSerialQueue.RelocationSeal seal;
        private final ZLinkRelocationPermitPool.Lease permit;
        private final Owned owned;
        private final ZLinkMeshNodeDescriptor target;
        private final UUID relocationId;
        private final byte[] state;
        private final byte[] timerEnvelope;
        private final ZLinkAggregateRelocationCoordinator.StagedRoot initial;
        private final ZLinkSpotRetireControl.StageRequest stageRequest;
        private ZLinkAggregateRelocationCoordinator.Prepared prepared;
        private List<ZLinkAsyncSerialQueue.QueuedRecord> finalJournal =
            List.of();
        private boolean committed;
        private boolean terminal;

        private PreparedSource(
            ZLinkAggregateRelocationCoordinator coordinator,
            ZLinkActorSessionCoordinator actors,
            ZLinkSpotRuntime relocationReplies,
            ZLinkAsyncSerialQueue.RelocationSeal seal,
            ZLinkRelocationPermitPool.Lease permit,
            Owned owned,
            ZLinkMeshNodeDescriptor target,
            UUID relocationId,
            byte[] state,
            byte[] timerEnvelope,
            ZLinkAggregateRelocationCoordinator.StagedRoot initial,
            ZLinkSpotRetireControl.StageRequest stageRequest) {
            this.coordinator = coordinator;
            this.actors = actors;
            this.relocationReplies = relocationReplies;
            this.seal = seal;
            this.permit = permit;
            this.owned = owned;
            this.target = target;
            this.relocationId = relocationId;
            this.state = state.clone();
            this.timerEnvelope = timerEnvelope.clone();
            this.initial = initial;
            this.stageRequest = stageRequest;
        }

        ZLinkStandaloneActorRelocationStagingOwner.Request targetRequest() {
            return new ZLinkStandaloneActorRelocationStagingOwner.Request(
                relocationId,
                owned.actorId(),
                owned.stableType(),
                owned.snapshot().objectGeneration(),
                owned.snapshot().authorityOwnerGeneration(),
                owned.snapshotPolicy(),
                target.entrySpotId().orElseThrow());
        }

        ZLinkAggregateRelocationCoordinator.StagedRoot initialRoot() {
            return initial;
        }

        ZLinkSpotRetireControl.StageRequest stageRequest() {
            return stageRequest;
        }

        synchronized CompletionStage<
            ZLinkAggregateRelocationCoordinator.Prepared> freezeAndPrepare(
                ZLinkStoreCancellation cancellation) {
            if (terminal || committed || prepared != null) {
                return failed(new IllegalStateException(
                    "Actor relocation source is already terminal"));
            }
            List<ZLinkAsyncSerialQueue.QueuedRecord> held =
                actors.freezeActorRelocationIngress(
                    owned.actorId(), seal).orElseThrow(() ->
                        new IllegalStateException(
                            "Actor relocation ingress freeze was lost"));
            List<ZLinkAsyncSerialQueue.QueuedRecord> journal =
                new ArrayList<>(seal.captured());
            journal.addAll(held);
            byte[] finalRoot =
                ZLinkCanonicalActorRelocationEnvelope.encode(
                    relocationId,
                    owned.actorId(),
                    owned.snapshot().objectGeneration(),
                    owned.snapshot().authorityOwnerGeneration(),
                    owned.snapshotPolicy(),
                    state,
                    journal,
                    timerEnvelope);
            if (!permit.tryShrinkPayload(finalRoot.length)) {
                return failed(new IllegalStateException(
                    "Actor relocation root exceeded its permit"));
            }
            var request = new ZLinkAggregateRelocationCoordinator.Request(
                initial.request().aggregateId(),
                initial.request().aggregateGeneration(),
                initial.request().participants(),
                finalRoot,
                initial.request().targetDescriptor(),
                initial.request().targetDescriptorLifecycleGeneration(),
                initial.request().capacityBundle(),
                initial.request().targetOwner());
            return coordinator.prepare(request, cancellation)
                .thenApply(value -> {
                    synchronized (PreparedSource.this) {
                        prepared = value;
                        finalJournal = List.copyOf(journal);
                    }
                    return value;
                });
        }

        synchronized void commitSourceQueue(
            Map<String, Long> targetOwnerGenerations) {
            if (terminal || committed || prepared == null) {
                throw new IllegalStateException(
                    "Actor relocation source queue cannot be committed");
            }
            Objects.requireNonNull(
                targetOwnerGenerations,
                "targetOwnerGenerations");
            ZLinkSpotRetireControl.ParticipantFence participant =
                stageRequest.participants().stream()
                    .filter(value ->
                        value.objectId().equals(owned.actorId()))
                    .findFirst()
                    .orElseThrow(() -> new IllegalStateException(
                        "Actor relocation participant is missing"));
            relocationReplies.bindCanonicalRelocationReplies(
                Map.of("actor:" + owned.actorId(), finalJournal),
                stageRequest.targetNodeRid(),
                stageRequest.targetNodeGeneration(),
                Map.of(
                    owned.actorId(),
                    new ZLinkSpotRelocationReplyRoutes.CommittedFence(
                        participant.authorityKey(),
                        1L,
                        targetOwnerGeneration(
                            targetOwnerGenerations,
                            participant.authorityKey()))));
            if (actors.commitActorRelocation(
                    owned.actorId(), seal).isEmpty()) {
                throw new IllegalStateException(
                    "Actor relocation source queue cannot be committed");
            }
            committed = true;
        }

        synchronized CompletionStage<
            ZLinkAggregateRelocationCoordinator.Published> commitAuthority(
                ZLinkStoreCancellation cancellation) {
            if (terminal || committed || prepared == null) {
                return failed(new IllegalStateException(
                    "Actor relocation source is not ready for authority commit"));
            }
            return coordinator.commit(prepared, cancellation);
        }

        synchronized CompletionStage<
            ZLinkAggregateRelocationCoordinator.Published>
                completeSourceCleanup(
                    ZLinkAggregateRelocationCoordinator.Published published,
                    ZLinkStoreCancellation cancellation) {
            if (!committed || terminal || prepared == null) {
                return failed(new IllegalStateException(
                    "Actor relocation source is not committed"));
            }
            return coordinator.completeSourceCleanup(
                published,
                prepared.request().root(),
                cancellation);
        }

        synchronized CompletionStage<Void> discardInitialAfterCommit() {
            if (!committed || terminal) {
                return failed(new IllegalStateException(
                    "Actor relocation source is not committed"));
            }
            return coordinator.discardStagedRoot(initial);
        }

        synchronized void releasePermitAfterCompletion() {
            if (!committed) {
                throw new IllegalStateException(
                    "Actor relocation source is not committed");
            }
            finish();
        }

        CompletionStage<Void> cleanupLocal() {
            if (!committed || terminal) {
                return failed(new IllegalStateException(
                    "Actor relocation source is not committed"));
            }
            relocationReplies.closeActorTimersAfterRelocation(
                owned.actorId());
            return actors.completeRelocationSource(
                List.of(owned.actorId()));
        }

        synchronized CompletionStage<Void> abort() {
            if (terminal || committed) {
                return failed(new IllegalStateException(
                    "committed Actor relocation cannot be aborted"));
            }
            CompletionStage<Void> authority = prepared == null
                ? CompletableFuture.completedFuture(null)
                : coordinator.abort(prepared);
            return authority
                .thenCompose(ignored ->
                    coordinator.discardStagedRoot(initial))
                .thenRun(() -> {
                    if (!actors.abortActorRelocation(
                        owned.actorId(), seal)) {
                        throw new IllegalStateException(
                            "Actor relocation source queue was lost");
                    }
                    relocationReplies.resumeActorTimersAfterRelocationAbort(
                        owned.actorId());
                    finish();
                });
        }

        private synchronized void finish() {
            if (!terminal) {
                terminal = true;
                permit.close();
            }
        }
    }

    private record Admission(
        Owned owned,
        ZLinkMeshNodeDescriptor target,
        Optional<ZLinkSpotRetireControl.SessionRouteFence> sessionRoute) {
        private Admission {
            sessionRoute = Objects.requireNonNull(
                sessionRoute, "sessionRoute");
        }
    }

    private record Owned(
        String authorityKey,
        String actorId,
        String stableType,
        ZLinkAuthoritySnapshot snapshot,
        boolean snapshotPolicy) {
    }

    private static String requireText(String value, String name) {
        if (value == null || value.isBlank() || value.indexOf('\0') >= 0) {
            throw new IllegalArgumentException(name + " is invalid");
        }
        return value;
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
