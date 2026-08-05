package systems.zlink.framework.runtime.actors;

import java.security.MessageDigest;
import java.time.Duration;
import java.time.Instant;
import java.util.List;
import java.util.Optional;
import java.util.Set;
import java.util.UUID;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionException;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.ConcurrentHashMap;
import java.util.concurrent.TimeUnit;
import java.util.logging.Logger;
import systems.zlink.framework.ZLinkEncodedPayload;
import systems.zlink.framework.ZLinkMessageSerializer;
import systems.zlink.framework.actors.ActorRef;
import systems.zlink.framework.actors.ZLinkActorCreateResult;
import systems.zlink.framework.errors.ZLinkFrameworkErrorKind;
import systems.zlink.framework.errors.ZLinkFrameworkException;
import systems.zlink.framework.locations.*;
import systems.zlink.framework.runtime.internal.locations.*;
import systems.zlink.framework.runtime.internal.locations
    .ZLinkLocationRepository;
import systems.zlink.framework.runtime.internal.binding.spot.MeshNodeState;
import systems.zlink.framework.runtime.internal.binding.spot.MeshNodeStatus;
import systems.zlink.framework.runtime.internal.binding.spot.MeshPeerEntry;
import systems.zlink.framework.runtime.internal.binding.spot.MeshPeerState;
import systems.zlink.framework.messaging.ZLinkMessage;
import systems.zlink.framework.runtime.internal.backend.ZLinkInternalMeshNode;
import systems.zlink.framework.runtime.locations.ZLinkActorAuthorityPayloadCodec;
import systems.zlink.framework.runtime.locations.ZLinkAuthorityKeyCodec;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceM6AWireCodec;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceM6BWireCodec;
import systems.zlink.framework.spots.ZLinkSpotKind;

/**
 * Owns the durable Actor creation reservation and terminal publication path.
 */
public final class ZLinkActorCreationCoordinator
    implements ZLinkActorRuntime.CreationSubmitter,
        ZLinkInternalMeshNode.ActorCreateOperationHandler {
    private static final boolean STREAM_TRACE =
        "1".equals(System.getenv("ZLINK_JAVA_STREAM_TRACE"));
    private static final Logger LOGGER =
        Logger.getLogger(ZLinkActorCreationCoordinator.class.getName());
    private static final ZLinkStoreCancellation OPEN = () -> false;
    private static final Duration TERMINAL_RETENTION =
        Duration.ofMinutes(5);
    private static final int TERMINAL_REQUEST_FAILED = 105;
    private static final int TERMINAL_INVALID_STATE = 107;
    private static final int FAILURE_ACTOR_CREATE_FAILED = 2;

    private final String meshName;
    private final ZLinkInternalMeshNode node;
    private final ZLinkLocationRepository locations;
    private final ZLinkActorRuntime actors;
    private final ZLinkMessageSerializer serializer;
    private final ZLinkActorAuthorityPayloadCodec authorities =
        new ZLinkActorAuthorityPayloadCodec();
    private final ZLinkServiceM6AWireCodec payloads =
        new ZLinkServiceM6AWireCodec();
    private final ZLinkServiceM6BWireCodec wire =
        new ZLinkServiceM6BWireCodec();
    private final ConcurrentHashMap<String, CompletableFuture<Void>>
        targetTails = new ConcurrentHashMap<>();

    public ZLinkActorCreationCoordinator(
        String meshName,
        ZLinkInternalMeshNode node,
        ZLinkLocationRepository locations,
        ZLinkActorRuntime actors,
        ZLinkMessageSerializer serializer) {
        this.meshName = java.util.Objects.requireNonNull(
            meshName, "meshName");
        this.node = java.util.Objects.requireNonNull(node, "node");
        this.locations = java.util.Objects.requireNonNull(
            locations, "locations");
        this.actors = java.util.Objects.requireNonNull(actors, "actors");
        this.serializer = java.util.Objects.requireNonNull(
            serializer, "serializer");
    }

    @Override
    public CompletionStage<ZLinkActorCreateResult> submit(
        String actorId,
        String actorType,
        ZLinkMessage createRequest,
        boolean getOrCreate,
        Duration timeout) {
        UUID id = UUID.randomUUID();
        long high = id.getMostSignificantBits() & Long.MAX_VALUE;
        long low = id.getLeastSignificantBits() & Long.MAX_VALUE;
        if (high == 0 && low == 0) {
            low = 1;
        }
        streamTrace("actor-create coordinator-submit actor=" + actorId
            + " type=" + actorType
            + " getOrCreate=" + getOrCreate
            + " timeout=" + timeout);
        var operation = new ZLinkCreationOperationIdentity(
            node.status().routingId(),
            node.status().lifecycleGeneration(),
            high,
            low);
        long deadline = Math.addExact(
            System.currentTimeMillis(),
            timeout.toMillis());
        byte[] requestEnvelope = encodeCreateRequest(createRequest);
        return resumeOrCreate(
            operation,
            actorId,
            actorType,
            requestEnvelope,
            getOrCreate,
            deadline,
            Set.of());
    }

    private CompletionStage<ZLinkActorCreateResult> resumeOrCreate(
        ZLinkCreationOperationIdentity operation,
        String actorId,
        String actorType,
        byte[] requestEnvelope,
        boolean getOrCreate,
        long deadline,
        Set<ZLinkMeshNodeDescriptorKey> excludedTargets) {
        streamTrace("actor-create resume actor=" + actorId
            + " excluded=" + excludedTargets.size());
        return locations.readCreationTerminal(operation, OPEN)
            .thenCompose(read -> {
                if (read instanceof ZLinkCreationTerminalFound found) {
                    streamTrace("actor-create terminal-found actor=" + actorId);
                    return completedResult(found.terminal());
                }
                if (System.currentTimeMillis() >= deadline) {
                    return failed("Actor create deadline expired");
                }
                return selectTarget(
                        actorType, deadline, excludedTargets)
                    .thenCompose(target -> {
                        streamTrace("actor-create target-selected actor=" + actorId
                            + " node=" + target.rid()
                            + " mesh=" + target.meshName());
                        return resolveEntrySpot(target)
                        .thenCompose(entry -> reserveAndSubmit(
                            operation,
                            actorId,
                            actorType,
                            requestEnvelope,
                            getOrCreate,
                            deadline,
                            target,
                            entry,
                            excludedTargets));
                    });
            });
    }

    private CompletionStage<ZLinkActorCreateResult> reserveAndSubmit(
        ZLinkCreationOperationIdentity operation,
        String actorId,
        String actorType,
        byte[] requestEnvelope,
        boolean getOrCreate,
        long deadline,
        ZLinkMeshNodeDescriptor target,
        EntrySpot entry,
        Set<ZLinkMeshNodeDescriptorKey> excludedTargets) {
        streamTrace("actor-create reserve-start actor=" + actorId
            + " target=" + target.rid()
            + " entry=" + entry.spotId());
        if (!isExactReadyTarget(target, node.status(), node.peers())) {
            streamTrace("actor-create reserve-deferred actor=" + actorId
                + " target=" + target.rid()
                + " generation=" + target.lifecycleGeneration());
            if (System.currentTimeMillis() >= deadline) {
                return failed("Actor placement target is no longer ready");
            }
            return awaitConflict().thenCompose(ignored ->
                resumeOrCreate(
                    operation,
                    actorId,
                    actorType,
                    requestEnvelope,
                    getOrCreate,
                    deadline,
                    excludedTargets));
        }
        String key = ZLinkAuthorityKeyCodec.actor(actorId);
        byte[] creating = authorities.encode(
            ZLinkActorAuthorityPayloadCodec.State.CREATING,
            actorType,
            actorId,
            entry.spotId(),
            entry.spotGeneration(),
            1,
            target.ownerId(),
            target.leaseGeneration(),
            target.meshName(),
            target.rid(),
            target.lifecycleGeneration());
        var request = new ZLinkObjectReservationRequest(
            ZLinkPlacementObjectKind.ACTOR,
            key,
            actorType,
            inline(requestEnvelope),
            sha256(requestEnvelope),
            requestEnvelope.length,
            new ZLinkMeshNodeDescriptorKey(
                target.meshName(), target.rid()),
            target.lifecycleGeneration(),
            new ZLinkLocationOwnerToken(
                target.ownerId(), target.leaseGeneration()),
            creating,
            ZLinkPlacementCapacityBundle.actor(1));
        return locations.reserve(request, OPEN)
            .thenCompose(result -> {
                streamTrace("actor-create reserve-result actor=" + actorId
                    + " result=" + result.getClass().getSimpleName());
                if (result instanceof ZLinkObjectAlreadyExists exists) {
                    return existing(exists.current(), actorId, actorType);
                }
                if (result instanceof ZLinkObjectTypeMismatch) {
                    return CompletableFuture.failedFuture(
                        frameworkFailure(
                            ZLinkFrameworkErrorKind.REJECTED,
                            "Actor type does not match"));
                }
                if (result instanceof ZLinkObjectConflict conflict) {
                    Set<ZLinkMeshNodeDescriptorKey> retryTargets =
                        conflict.current() instanceof ZLinkAuthorityMissing
                            ? excluding(excludedTargets, target)
                            : excludedTargets;
                    return awaitConflict().thenCompose(ignored ->
                        resumeOrCreate(
                            operation,
                            actorId,
                            actorType,
                            requestEnvelope,
                            getOrCreate,
                            deadline,
                            retryTargets));
                }
                if (result instanceof ZLinkPlacementCapacityExhausted) {
                    return resumeOrCreate(
                        operation,
                        actorId,
                        actorType,
                        requestEnvelope,
                        getOrCreate,
                        deadline,
                        excluding(excludedTargets, target));
                }
                if (!(result instanceof ZLinkObjectReserved reserved)) {
                    return failed(
                        "Actor reservation failed: "
                            + result.getClass().getSimpleName());
                }
                ZLinkObjectReservation reservation =
                    reserved.reservation();
                var fence = reservationFence(reservation);
                var intent = new ZLinkInternalMeshNode.ActorCreateIntent(
                    actorId,
                    actorType,
                    fence,
                    operation.operationIdHigh(),
                    operation.operationIdLow(),
                    deadline);
                streamTrace("actor-create mesh-request actor=" + actorId
                    + " target=" + target.rid());
                return node.requestActorCreate(
                        target.rid(), intent, remainingTimeout(deadline))
                    .thenCompose(response -> {
                        streamTrace("actor-create mesh-response actor=" + actorId);
                        return completedResult(response.terminalEnvelope());
                    })
                    .exceptionallyCompose(failure -> {
                        streamTrace("actor-create mesh-failure actor=" + actorId
                            + " error=" + unwrap(failure));
                        return locations.readCreationTerminal(operation, OPEN)
                            .thenCompose(read ->
                                read instanceof
                                    ZLinkCreationTerminalFound found
                                    ? completedResult(found.terminal())
                                    : CompletableFuture.failedFuture(
                                        unwrap(failure)));
                    });
            });
    }

    private static Duration remainingTimeout(long deadlineUnixMs) {
        long remainingMillis = deadlineUnixMs - System.currentTimeMillis();
        if (remainingMillis <= 0) {
            return Duration.ofNanos(1);
        }
        return Duration.ofMillis(remainingMillis);
    }

    private CompletionStage<ZLinkActorCreateResult> existing(
        ZLinkAuthoritySnapshot snapshot,
        String actorId,
        String actorType) {
        var authority = authorities.decode(snapshot.payload())
            .orElseThrow(() -> frameworkFailure(
                ZLinkFrameworkErrorKind.INTERNAL_FAILURE,
                "Actor authority payload is invalid"));
        if (authority.state()
                != ZLinkActorAuthorityPayloadCodec.State.READY
            || !authority.actorId().equals(actorId)
            || !authority.stableType().equals(actorType)) {
            return failed("Actor authority is not Ready");
        }
        return CompletableFuture.completedFuture(
            new ZLinkActorCreateResult.Existing(
                new ActorRef(
                    actorId,
                    snapshot.objectGeneration(),
                    authority.meshName(),
                    authority.nodeRid())));
    }

    @Override
    public CompletionStage<ZLinkInternalMeshNode.ActorCreateResponse> create(
        ZLinkInternalMeshNode.ActorCreateRequest request) {
        return serializeTarget(
            request.intent().actorId(),
            () -> executeTarget(request));
    }

    private CompletionStage<ZLinkInternalMeshNode.ActorCreateResponse>
        executeTarget(ZLinkInternalMeshNode.ActorCreateRequest request) {
        streamTrace("actor-create target-execute actor="
            + request.intent().actorId());
        ZLinkCreationOperationIdentity operation =
            new ZLinkCreationOperationIdentity(
                request.sourceNodeRid(),
                request.sourceNodeGeneration(),
                request.operationHigh(),
                request.operationLow());
        return locations.readCreationTerminal(operation, OPEN)
            .thenCompose(read -> {
                if (read instanceof ZLinkCreationTerminalFound found) {
                    return verifiedResponse(found.terminal());
                }
                if (System.currentTimeMillis()
                        >= request.intent().deadlineUnixMs()) {
                    return failReserved(
                        request,
                        operation,
                        "Actor create deadline expired");
                }
                return executeReserved(request, operation);
            });
    }

    private CompletionStage<ZLinkInternalMeshNode.ActorCreateResponse>
        executeReserved(
            ZLinkInternalMeshNode.ActorCreateRequest request,
            ZLinkCreationOperationIdentity operation) {
        String key = ZLinkAuthorityKeyCodec.actor(
            request.intent().actorId());
        return locations.read(key, OPEN).thenCompose(read -> {
            if (!(read instanceof ZLinkAuthoritySnapshot snapshot)) {
                return replayAfterConflict(
                    operation, "Actor reservation is missing");
            }
            ZLinkObjectReservation reservation;
            try {
                reservation = validateReservation(
                    request, key, snapshot);
            } catch (RuntimeException stale) {
                return replayAfterConflict(
                    operation,
                    stale.getMessage() == null
                        ? "Actor reservation is stale"
                        : stale.getMessage());
            }
            byte[] stored;
            try {
                stored = decodeInline(snapshot.pendingCreation()
                    .orElseThrow().requestContentReference());
            } catch (RuntimeException invalid) {
                return failReserved(
                    request,
                    operation,
                    reservation,
                    "Actor create payload is invalid");
            }
            var pending = snapshot.pendingCreation().orElseThrow();
            if (stored.length != pending.requestEncodedSize()
                || !MessageDigest.isEqual(
                    sha256(stored), pending.requestSha256())) {
                return failReserved(
                    request,
                    operation,
                    reservation,
                    "Actor create payload failed integrity validation");
            }
            var application = payloads.decodeApplicationPayload(stored);
            ZLinkMessage createRequest = ZLinkMessage.fromEncoded(
                ZLinkEncodedPayload.from(application.payload()),
                serializer);
            return actors.createReservedActor(
                    request.intent().actorId(),
                    request.intent().stableType(),
                    createRequest,
                    snapshot.objectGeneration(),
                    snapshot.authorityOwnerGeneration())
                .thenCompose(result -> completeTarget(
                    request,
                    operation,
                    snapshot,
                    reservation,
                    result))
                .exceptionallyCompose(failure -> failReserved(
                    request,
                    operation,
                    reservation,
                    failure.getMessage() == null
                        ? "Actor create callback failed"
                        : failure.getMessage()));
        });
    }

    private CompletionStage<ZLinkInternalMeshNode.ActorCreateResponse>
        completeTarget(
            ZLinkInternalMeshNode.ActorCreateRequest request,
            ZLinkCreationOperationIdentity operation,
            ZLinkAuthoritySnapshot snapshot,
            ZLinkObjectReservation reservation,
            ZLinkActorCreateResult result) {
        if (result instanceof ZLinkActorCreateResult.Rejected rejected) {
            byte[] envelope = terminalEnvelope(
                new ZLinkServiceM6BWireCodec.ActorCreateTerminal(
                    ZLinkServiceM6BWireCodec.ActorCreateResult.REJECTED,
                    null),
                rejected.reply(),
                0,
                0);
            ZLinkCreationOperationTerminal terminal = terminal(
                operation,
                reservation,
                ZLinkCreationTerminalState.REJECTED,
                envelope,
                request.intent().deadlineUnixMs());
            return locations.reject(reservation, terminal, OPEN)
                .thenCompose(status ->
                    status == ZLinkObjectRejectResult.REJECTED
                        || status
                            == ZLinkObjectRejectResult.ALREADY_REJECTED
                        ? response(envelope)
                        : replayAfterConflict(
                            operation,
                            "Actor rejection lost its reservation"));
        }
        ActorRef actor = result instanceof
                ZLinkActorCreateResult.Created created
            ? created.actor()
            : ((ZLinkActorCreateResult.Existing) result).actor();
        ZLinkMessage reply = result instanceof
                ZLinkActorCreateResult.Created created
            ? created.reply()
            : null;
        ActorRef published = new ActorRef(
            request.intent().actorId(),
            snapshot.objectGeneration(),
            meshName,
            node.status().routingId());
        byte[] envelope = terminalEnvelope(
            new ZLinkServiceM6BWireCodec.ActorCreateTerminal(
                ZLinkServiceM6BWireCodec.ActorCreateResult.CREATED,
                published),
            reply,
            0,
            0);
        ZLinkCreationOperationTerminal terminal = terminal(
            operation,
            reservation,
            ZLinkCreationTerminalState.CREATED,
            envelope,
            request.intent().deadlineUnixMs());
        var current = authorities.decode(snapshot.payload())
            .orElseThrow(() -> stale("invalid Actor authority"));
        byte[] ready = authorities.encode(
            ZLinkActorAuthorityPayloadCodec.State.READY,
            request.intent().stableType(),
            request.intent().actorId(),
            current.currentSpotId(),
            current.currentSpotGeneration(),
            current.currentSpotKind(),
            snapshot.ownerId(),
            snapshot.ownerLeaseGeneration(),
            meshName,
            node.status().routingId(),
            node.status().lifecycleGeneration());
        return locations.commit(reservation, ready, terminal, OPEN)
            .thenCompose(status -> {
                if (status == ZLinkObjectCommitResult.COMMITTED
                    || status
                        == ZLinkObjectCommitResult.ALREADY_COMMITTED) {
                    return response(envelope);
                }
                return actors.discardReservedActor(actor.actorId())
                    .thenCompose(ignored -> replayAfterConflict(
                        operation,
                        "Actor Ready commit lost its reservation"));
            });
    }

    private ZLinkObjectReservation validateReservation(
        ZLinkInternalMeshNode.ActorCreateRequest request,
        String key,
        ZLinkAuthoritySnapshot snapshot) {
        var fence = request.intent().reservation();
        var allocation = snapshot.allocation();
        var pending = snapshot.pendingCreation().orElseThrow(
            () -> stale("Actor creation projection is missing"));
        var authority = authorities.decode(snapshot.payload())
            .orElseThrow(() -> stale("invalid Actor authority"));
        require(
            authority.state()
                == ZLinkActorAuthorityPayloadCodec.State.CREATING
                && authority.actorId().equals(request.intent().actorId())
                && authority.stableType()
                    .equals(request.intent().stableType())
                && authority.meshName().equals(meshName)
                && authority.nodeRid().equals(node.status().routingId())
                && authority.nodeGeneration()
                    == node.status().lifecycleGeneration()
                && allocation.state()
                    == ZLinkPlacementAllocationState.PENDING
                && allocation.objectKind()
                    == ZLinkPlacementObjectKind.ACTOR
                && allocation.stableType()
                    .equals(request.intent().stableType())
                && pending.reservationId().equals(fence.reservationId())
                && snapshot.storeVersion().equals(fence.storeVersion())
                && snapshot.objectGeneration()
                    == fence.objectGeneration()
                && snapshot.authorityOwnerGeneration()
                    == fence.authorityOwnerGeneration()
                && snapshot.ownerId().equals(fence.targetOwnerId())
                && snapshot.ownerLeaseGeneration()
                    == fence.targetOwnerLeaseGeneration()
                && fence.targetNodeRid().equals(node.status().routingId())
                && fence.targetNodeGeneration()
                    == node.status().lifecycleGeneration()
                && allocation.capacityBundle().equals(
                    ZLinkPlacementCapacityBundle.actor(
                        Math.toIntExact(
                            fence.pendingCapacityDelta()))),
            "stale Actor create reservation");
        return new ZLinkObjectReservation(
            key,
            snapshot.storeVersion(),
            snapshot.objectGeneration(),
            snapshot.authorityOwnerGeneration(),
            fence.reservationId(),
            allocation.descriptor(),
            allocation.descriptorLifecycleGeneration(),
            new ZLinkLocationOwnerToken(
                snapshot.ownerId(),
                snapshot.ownerLeaseGeneration()));
    }

    private CompletionStage<ZLinkInternalMeshNode.ActorCreateResponse>
        failReserved(
            ZLinkInternalMeshNode.ActorCreateRequest request,
            ZLinkCreationOperationIdentity operation,
            String message) {
        String key = ZLinkAuthorityKeyCodec.actor(
            request.intent().actorId());
        return locations.read(key, OPEN).thenCompose(read -> {
            if (!(read instanceof ZLinkAuthoritySnapshot snapshot)) {
                return replayAfterConflict(operation, message);
            }
            return failReserved(
                request,
                operation,
                validateReservation(request, key, snapshot),
                message);
        });
    }

    private CompletionStage<ZLinkInternalMeshNode.ActorCreateResponse>
        failReserved(
            ZLinkInternalMeshNode.ActorCreateRequest request,
            ZLinkCreationOperationIdentity operation,
            ZLinkObjectReservation reservation,
            String message) {
        byte[] envelope = terminalEnvelope(
            null,
            null,
            TERMINAL_REQUEST_FAILED,
            FAILURE_ACTOR_CREATE_FAILED);
        ZLinkCreationOperationTerminal terminal = terminal(
            operation,
            reservation,
            ZLinkCreationTerminalState.FAILED,
            envelope,
            request.intent().deadlineUnixMs());
        return actors.discardReservedActor(request.intent().actorId())
            .thenCompose(ignored ->
                locations.abort(reservation, terminal, OPEN))
            .thenCompose(ignored -> response(envelope));
    }

    private CompletionStage<ZLinkInternalMeshNode.ActorCreateResponse>
        replayAfterConflict(
            ZLinkCreationOperationIdentity operation,
            String message) {
        return locations.readCreationTerminal(operation, OPEN)
            .thenCompose(read ->
                read instanceof ZLinkCreationTerminalFound found
                    ? verifiedResponse(found.terminal())
                    : CompletableFuture.failedFuture(stale(message)));
    }

    private byte[] terminalEnvelope(
        ZLinkServiceM6BWireCodec.ActorCreateTerminal creation,
        ZLinkMessage reply,
        int terminalResult,
        int failureCode) {
        byte[] applicationFrame = reply == null
            ? null
            : payloads.encodeApplicationPayload(
                new ZLinkServiceM6AWireCodec.ApplicationPayload(
                    "zlink.actor-create-reply",
                    "application/zlink-framework-json-v1",
                    reply.toEncodedPayload(serializer).bytes()));
        return wire.encodeCreationOperationTerminal(
            new ZLinkServiceM6BWireCodec.ActorCreationTerminal(
                terminalResult,
                failureCode,
                creation,
                applicationFrame));
    }

    private ZLinkCreationOperationTerminal terminal(
        ZLinkCreationOperationIdentity operation,
        ZLinkObjectReservation reservation,
        ZLinkCreationTerminalState state,
        byte[] envelope,
        long deadlineUnixMs) {
        return new ZLinkCreationOperationTerminal(
            operation,
            reservation,
            state,
            envelope,
            sha256(envelope),
            Instant.ofEpochMilli(Math.addExact(
                deadlineUnixMs,
                TERMINAL_RETENTION.toMillis())));
    }

    private CompletionStage<ZLinkActorCreateResult> completedResult(
        ZLinkCreationOperationTerminal terminal) {
        if (!MessageDigest.isEqual(
            sha256(terminal.terminalEnvelope()),
            terminal.terminalSha256())) {
            return failed(
                "Actor creation terminal failed integrity validation");
        }
        return completedResult(terminal.terminalEnvelope());
    }

    private CompletionStage<ZLinkActorCreateResult> completedResult(
        byte[] envelope) {
        try {
            var terminal = wire.decodeCreationOperationTerminal(envelope);
            if (terminal.terminalResult() != 0
                || terminal.creation() == null) {
                return failed(
                    "Actor create failed with terminal result "
                        + terminal.terminalResult()
                        + " and failure code "
                        + terminal.failureCode());
            }
            ZLinkMessage reply = decodeReply(
                terminal.applicationPayloadFrame());
            return CompletableFuture.completedFuture(
                switch (terminal.creation().result()) {
                    case EXISTING ->
                        new ZLinkActorCreateResult.Existing(
                            terminal.creation().actor());
                    case CREATED ->
                        new ZLinkActorCreateResult.Created(
                            terminal.creation().actor(), reply);
                    case REJECTED ->
                        new ZLinkActorCreateResult.Rejected(reply);
                });
        } catch (RuntimeException failure) {
            return CompletableFuture.failedFuture(failure);
        }
    }

    private ZLinkMessage decodeReply(byte[] frame) {
        if (frame == null) {
            return null;
        }
        var payload = payloads.decodeApplicationPayload(frame);
        return ZLinkMessage.fromEncoded(
            ZLinkEncodedPayload.from(payload.payload()), serializer);
    }

    private CompletionStage<ZLinkMeshNodeDescriptor> selectTarget(
        String actorType,
        long deadline,
        Set<ZLinkMeshNodeDescriptorKey> excludedTargets) {
        return locations.listMeshNodes(
                meshName, ZLinkPageRequest.firstPage())
            .thenCompose(page -> {
                MeshNodeStatus localStatus = node.status();
                List<MeshPeerEntry> peerSnapshot = node.peers();
                List<ZLinkMeshNodeDescriptor> candidates =
                    page.items().stream()
                        .filter(candidate ->
                            candidate.state()
                                == systems.zlink.framework.runtime.host
                                    .ZLinkFrameworkRuntimeState.SERVING
                                && candidate.objectRole()
                                    == ZLinkMeshNodeObjectRole.SERVER
                                && candidate.placementWeight() > 0
                                && !excludedTargets.contains(
                                    descriptorKey(candidate))
                                && candidate.objectCapabilities().stream()
                                    .anyMatch(capability ->
                                        capability.objectKind()
                                            == ZLinkPlacementObjectKind.ACTOR
                                        && capability.stableType()
                                                .equals(actorType)
                                        && hasCapacity(
                                                candidate,
                                                capability))
                                && isExactReadyTarget(
                                    candidate, localStatus, peerSnapshot))
                        .toList();
                streamTrace("actor-create candidates actorType=" + actorType
                    + " count=" + candidates.size()
                    + " nodes=" + candidates.stream()
                        .map(candidate -> candidate.rid()
                            + "@" + candidate.lifecycleGeneration())
                        .toList());
                if (candidates.isEmpty()) {
                    if (System.currentTimeMillis() >= deadline) {
                        return failed("No Ready Actor placement target");
                    }
                    return awaitConflict().thenCompose(
                        ignored -> selectTarget(
                            actorType, deadline, Set.of()));
                }
                Optional<ZLinkMeshNodeDescriptor> localTarget =
                    localCandidate(candidates, node.status().routingId());
                if (localTarget.isPresent()) {
                    streamTrace("actor-create local-target actorType="
                        + actorType
                        + " node=" + localTarget.get().rid());
                    return CompletableFuture.completedFuture(
                        localTarget.get());
                }
                long total = candidates.stream()
                    .mapToLong(ZLinkMeshNodeDescriptor::placementWeight)
                    .sum();
                long selected = java.util.concurrent.ThreadLocalRandom
                    .current().nextLong(total);
                for (var candidate : candidates) {
                    selected -= candidate.placementWeight();
                    if (selected < 0) {
                        return CompletableFuture.completedFuture(candidate);
                    }
                }
                return CompletableFuture.completedFuture(
                    candidates.getLast());
            });
    }

    static Optional<ZLinkMeshNodeDescriptor> localCandidate(
        List<ZLinkMeshNodeDescriptor> candidates,
        systems.zlink.contracts.core.RoutingId localRoutingId) {
        return candidates.stream()
            .filter(candidate -> candidate.rid().equals(localRoutingId))
            .findFirst();
    }

    static boolean isExactReadyTarget(
        ZLinkMeshNodeDescriptor candidate,
        MeshNodeStatus localStatus,
        List<MeshPeerEntry> peers) {
        java.util.Objects.requireNonNull(candidate, "candidate");
        java.util.Objects.requireNonNull(localStatus, "localStatus");
        java.util.Objects.requireNonNull(peers, "peers");
        if (candidate.rid().equals(localStatus.routingId())) {
            return candidate.meshName().equals(localStatus.meshName())
                && localStatus.state() == MeshNodeState.READY
                && candidate.lifecycleGeneration()
                    == localStatus.lifecycleGeneration();
        }
        return peers.stream().anyMatch(peer ->
            peer.routingId().equals(candidate.rid())
                && peer.lifecycleGeneration()
                    == candidate.lifecycleGeneration()
                && peer.state() == MeshPeerState.ADMITTED);
    }

    public CompletionStage<ZLinkActorRuntime.EntrySpotTarget>
        selectEntrySpotTarget(
            String actorType,
            Duration timeout) {
        if (actorType == null || actorType.isBlank()) {
            return failed("Actor stable type is not available");
        }
        if (timeout == null || timeout.isZero() || timeout.isNegative()) {
            return failed("Entry Spot selection timeout must be positive");
        }
        long deadline;
        try {
            deadline = Math.addExact(
                System.currentTimeMillis(),
                timeout.toMillis());
        } catch (ArithmeticException overflow) {
            deadline = Long.MAX_VALUE;
        }
        return selectTarget(actorType, deadline, Set.of())
            .thenCompose(target -> target.entrySpotId()
                .<CompletionStage<ZLinkActorRuntime.EntrySpotTarget>>map(
                    spotId -> CompletableFuture.completedFuture(
                        new ZLinkActorRuntime.EntrySpotTarget(
                            target.rid(),
                            spotId)))
                .orElseGet(() -> failed(
                    "Target descriptor has no Entry Spot identity")));
    }

    static boolean hasCapacity(
        ZLinkMeshNodeDescriptor candidate,
        ZLinkObjectCapability capability) {
        return capability.objectKind() == ZLinkPlacementObjectKind.ACTOR
            && hasRoom(candidate.capacity().actors());
    }

    private static boolean hasRoom(
        systems.zlink.framework.locations.ZLinkCapacityUsage usage) {
        return usage.limit() == 0
            || (long) usage.active() + usage.reserved() < usage.limit();
    }

    private static ZLinkMeshNodeDescriptorKey descriptorKey(
        ZLinkMeshNodeDescriptor descriptor) {
        return new ZLinkMeshNodeDescriptorKey(
            descriptor.meshName(), descriptor.rid());
    }

    private static Set<ZLinkMeshNodeDescriptorKey> excluding(
        Set<ZLinkMeshNodeDescriptorKey> current,
        ZLinkMeshNodeDescriptor descriptor) {
        var result = new java.util.HashSet<>(current);
        result.add(descriptorKey(descriptor));
        return Set.copyOf(result);
    }

    private CompletionStage<EntrySpot> resolveEntrySpot(
        ZLinkMeshNodeDescriptor target) {
        return target.entrySpotId()
            .<CompletionStage<EntrySpot>>map(spotId ->
                CompletableFuture.completedFuture(
                    new EntrySpot(spotId, target.lifecycleGeneration())))
            .orElseGet(() -> failed(
                "Target descriptor has no Entry Spot identity"));
    }

    private record EntrySpot(String spotId, long spotGeneration) {
    }

    private static ZLinkServiceM6BWireCodec.ReservationFence
        reservationFence(ZLinkObjectReservation reservation) {
        return new ZLinkServiceM6BWireCodec.ReservationFence(
            reservation.reservationVersion(),
            reservation.storeVersion(),
            reservation.objectGeneration(),
            reservation.authorityOwnerGeneration(),
            reservation.targetDescriptor().rid(),
            reservation.targetDescriptorLifecycleGeneration(),
            reservation.targetOwner().ownerId(),
            reservation.targetOwner().leaseGeneration(),
            1);
    }

    private byte[] encodeCreateRequest(ZLinkMessage request) {
        byte[] encoded = request.toEncodedPayload(serializer).bytes();
        return payloads.encodeApplicationPayload(
            new ZLinkServiceM6AWireCodec.ApplicationPayload(
                "zlink.actor-create",
                "application/zlink-framework-json-v1",
                encoded));
    }

    private CompletionStage<ZLinkInternalMeshNode.ActorCreateResponse>
        response(byte[] envelope) {
        return CompletableFuture.completedFuture(
            new ZLinkInternalMeshNode.ActorCreateResponse(envelope));
    }

    private CompletionStage<ZLinkInternalMeshNode.ActorCreateResponse>
        verifiedResponse(ZLinkCreationOperationTerminal terminal) {
        if (!MessageDigest.isEqual(
            sha256(terminal.terminalEnvelope()),
            terminal.terminalSha256())) {
            return CompletableFuture.failedFuture(stale(
                "Actor creation terminal failed integrity validation"));
        }
        wire.decodeCreationOperationTerminal(
            terminal.terminalEnvelope());
        return response(terminal.terminalEnvelope());
    }

    private CompletionStage<ZLinkInternalMeshNode.ActorCreateResponse>
        serializeTarget(
            String actorId,
            java.util.function.Supplier<CompletionStage<
                ZLinkInternalMeshNode.ActorCreateResponse>> operation) {
        CompletableFuture<Void> next = new CompletableFuture<>();
        CompletableFuture<Void> previous =
            targetTails.put(actorId, next);
        CompletionStage<Void> ready = previous == null
            ? CompletableFuture.completedFuture(null)
            : previous.handle((ignored, failure) -> null);
        CompletionStage<ZLinkInternalMeshNode.ActorCreateResponse> result =
            ready.thenCompose(ignored -> operation.get());
        result.whenComplete((ignored, failure) -> {
            next.complete(null);
            targetTails.remove(actorId, next);
        });
        return result;
    }

    private static CompletionStage<Void> awaitConflict() {
        return CompletableFuture.supplyAsync(
            () -> null,
            CompletableFuture.delayedExecutor(
                10, TimeUnit.MILLISECONDS));
    }

    private static String inline(byte[] value) {
        return "inline-v1:"
            + java.util.Base64.getUrlEncoder()
                .withoutPadding()
                .encodeToString(value);
    }

    private static byte[] decodeInline(String value) {
        String prefix = "inline-v1:";
        if (value == null || !value.startsWith(prefix)) {
            throw new IllegalArgumentException(
                "unsupported Actor creation intent reference");
        }
        return java.util.Base64.getUrlDecoder().decode(
            value.substring(prefix.length()));
    }

    private static byte[] sha256(byte[] value) {
        try {
            return MessageDigest.getInstance("SHA-256").digest(value);
        } catch (java.security.NoSuchAlgorithmException impossible) {
            throw new AssertionError(impossible);
        }
    }

    private static void require(boolean value, String message) {
        if (!value) {
            throw stale(message);
        }
    }

    private static ZLinkFrameworkException stale(String message) {
        return frameworkFailure(
            ZLinkFrameworkErrorKind.INTERNAL_FAILURE, message);
    }

    private static void streamTrace(String message) {
        if (STREAM_TRACE) {
            LOGGER.warning("[zlink-java-stream-trace] " + message);
        }
    }

    private static ZLinkFrameworkException frameworkFailure(
        ZLinkFrameworkErrorKind kind,
        String message) {
        return new ZLinkFrameworkException(kind, message);
    }

    private static <T> CompletionStage<T> failed(String message) {
        return CompletableFuture.failedFuture(stale(message));
    }

    private static Throwable unwrap(Throwable failure) {
        Throwable current = failure;
        while ((current instanceof CompletionException
            || current instanceof java.util.concurrent.ExecutionException)
            && current.getCause() != null) {
            current = current.getCause();
        }
        return current;
    }
}
