package systems.zlink.framework.runtime.spots;

import java.nio.charset.StandardCharsets;
import java.time.Duration;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Comparator;
import java.util.HashMap;
import java.util.List;
import java.util.Map;
import java.util.Objects;
import java.util.Optional;
import java.util.UUID;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.ConcurrentHashMap;
import java.util.concurrent.atomic.AtomicLong;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.runtime.internal.backend.ZLinkInternalMeshNode;
import systems.zlink.framework.runtime.internal.locations.ZLinkAggregateRelocationCoordinator;
import systems.zlink.framework.runtime.internal.locations.ZLinkAuthorityEntry;
import systems.zlink.framework.runtime.internal.locations.ZLinkAuthorityPage;
import systems.zlink.framework.runtime.internal.locations.ZLinkAuthorityReadResult;
import systems.zlink.framework.runtime.internal.locations.ZLinkAuthorityScanCursor;
import systems.zlink.framework.runtime.internal.locations.ZLinkAuthorityScanExpired;
import systems.zlink.framework.runtime.internal.locations.ZLinkAuthoritySnapshot;
import systems.zlink.framework.runtime.internal.locations.ZLinkLocationRepository;
import systems.zlink.framework.runtime.internal.locations
    .ZLinkCanonicalRelocationAuthorityStateCodec;
import systems.zlink.framework.runtime.internal.locations
    .ZLinkRelocationStartupScanner;
import systems.zlink.framework.runtime.internal.locations.ZLinkServiceRelocationEnvelopeCodec;
import systems.zlink.framework.runtime.internal.locations.ZLinkStoreCancellation;
import systems.zlink.framework.runtime.locations.ZLinkActorAuthorityPayloadCodec;
import systems.zlink.framework.runtime.locations.ZLinkAuthorityKeyCodec;
import systems.zlink.framework.runtime.locations.ZLinkServiceAuthorityPayloadCodec;
import systems.zlink.framework.runtime.protocol.ServiceWireConstants;

/**
 * Owns commands 30-35/40-41 for one production MeshNode.
 *
 * <p>The control wire carries numeric participant ids. The target resolves
 * their stable identities from the source-owned Location authority and checks
 * the immutable relocation root before creating hidden runtime state.</p>
 */
final class ZLinkCanonicalRelocationStateMachine
    implements ZLinkCanonicalRelocationTransitionOwner.StateMachine,
        ZLinkRelocationTransitionClient {
    private static final ZLinkStoreCancellation OPEN = () -> false;
    private static final int OFFER_MESSAGES_PER_PARTICIPANT = 64;
    private static final String ACTOR_AUTHORITY_PREFIX = "zla1:a:";
    private static final int SCAN_PAGE_SIZE = 1000;

    private final ZLinkInternalMeshNode node;
    private final String meshName;
    private final String entrySpotId;
    private final ZLinkLocationRepository locations;
    private final ZLinkAggregateRelocationCoordinator coordinator;
    private final ZLinkSpotRetireControl.TargetEndpoint target;
    private final RoutingId localNodeRid;
    private final long localNodeGeneration;
    private final AtomicLong reservationGeneration = new AtomicLong();
    private final ConcurrentHashMap<Fence, SourceSlot> sources =
        new ConcurrentHashMap<>();
    private final ConcurrentHashMap<Fence, TargetSlot> targets =
        new ConcurrentHashMap<>();
    private final ConcurrentHashMap<Fence, RecoverySlot> recoveries =
        new ConcurrentHashMap<>();

    ZLinkCanonicalRelocationStateMachine(
        ZLinkInternalMeshNode node,
        String meshName,
        String entrySpotId,
        ZLinkLocationRepository locations,
        ZLinkAggregateRelocationCoordinator coordinator,
        ZLinkSpotRetireControl.TargetEndpoint target) {
        this.node = Objects.requireNonNull(node, "node");
        this.meshName = requireText(meshName, "meshName");
        this.entrySpotId = entrySpotId;
        this.locations = Objects.requireNonNull(locations, "locations");
        this.coordinator = Objects.requireNonNull(coordinator, "coordinator");
        this.target = Objects.requireNonNull(target, "target");
        localNodeRid = node.status().routingId();
        localNodeGeneration = node.status().lifecycleGeneration();
    }

    CompletionStage<Void> recoverPublished(
        ZLinkRelocationStartupScanner.Candidate candidate) {
        Objects.requireNonNull(candidate, "candidate");
        if (!candidate.targetNodeRid().equals(localNodeRid)
            || candidate.targetNodeGeneration() != localNodeGeneration) {
            return CompletableFuture.completedFuture(null);
        }
        ZLinkSpotRetireControl.StageRequest request =
            recoveryRequest(candidate);
        Fence fence = Fence.from(request.fence());
        RecoverySlot created = new RecoverySlot(request);
        RecoverySlot current = recoveries.putIfAbsent(fence, created);
        RecoverySlot slot = current == null ? created : current;
        if (!slot.request().equals(request)) {
            return failed(new IllegalStateException(
                "published relocation recovery fence differs"));
        }
        if (current == null) {
            target.stage(request)
                .thenCompose(ignored -> target.publish(request))
                .whenComplete((ignored, failure) -> {
                    if (failure == null) {
                        created.ready().complete(null);
                    } else {
                        recoveries.remove(fence, created);
                        created.ready().completeExceptionally(unwrap(failure));
                    }
                });
        }
        return slot.ready().thenCompose(ignored ->
            candidate.sourceCleanupCompleted()
                ? finalizeRecovery(fence, slot)
                : CompletableFuture.completedFuture(null));
    }

    @Override
    public CompletionStage<Void> stage(
        RoutingId targetNodeRid,
        ZLinkSpotRetireControl.StageRequest request,
        Duration timeout) {
        Objects.requireNonNull(targetNodeRid, "targetNodeRid");
        Objects.requireNonNull(request, "request");
        requireTimeout(timeout);
        if (!targetNodeRid.equals(request.targetNodeRid())) {
            return failed(new IllegalArgumentException(
                "canonical relocation target RID differs"));
        }
        Fence fence = Fence.from(request.fence());
        return sourcePrepare(request).thenCompose(prepare -> {
            SourceSlot candidate = new SourceSlot(request, prepare);
            SourceSlot current = sources.putIfAbsent(fence, candidate);
            SourceSlot slot = current == null ? candidate : current;
            if (!slot.request().equals(request)
                || !Arrays.equals(
                    ZLinkCanonicalRelocationProtocol.encodePrepare(
                        slot.prepare()),
                    ZLinkCanonicalRelocationProtocol.encodePrepare(prepare))) {
                return failed(new IllegalArgumentException(
                    "duplicate canonical relocation stage differs"));
            }
            CompletionStage<Void> send = current == null
                ? send(targetNodeRid,
                    ZLinkCanonicalRelocationProtocol.encodePrepare(prepare))
                : CompletableFuture.completedFuture(null);
            return send.thenCompose(ignored ->
                timed(slot.reserved(), timeout, "relocation reservation"));
        });
    }

    @Override
    public CompletionStage<Void> publish(
        RoutingId targetNodeRid,
        ZLinkSpotRetireControl.Fence fence,
        Duration timeout) {
        requireTimeout(timeout);
        Fence key = Fence.from(fence);
        SourceSlot slot = requireSource(key, targetNodeRid);
        ZLinkCanonicalRelocationProtocol.ControlData command =
            new ZLinkCanonicalRelocationProtocol.ControlData(
                key.id(),
                key.attempt(),
                slot.prepare().coordinator(),
                ZLinkCanonicalRelocationProtocol.SOURCE,
                primaryParticipant(slot),
                1,
                source(slot.request()),
                4,
                slot.prepare().object(),
                0,
                0);
        return sendUntilAck(
            targetNodeRid,
            ZLinkCanonicalRelocationProtocol.encodeControlData(command),
            slot.publishAck(),
            timeout,
            "relocation publish ACK");
    }

    @Override
    public CompletionStage<Void> abort(
        RoutingId targetNodeRid,
        ZLinkSpotRetireControl.Fence fence,
        Duration timeout) {
        requireTimeout(timeout);
        Fence key = Fence.from(fence);
        SourceSlot slot = sources.get(key);
        if (slot == null) {
            return CompletableFuture.completedFuture(null);
        }
        if (!targetNodeRid.equals(slot.request().targetNodeRid())) {
            return failed(new IllegalArgumentException(
                "canonical relocation abort target differs"));
        }
        ZLinkCanonicalRelocationProtocol.ControlData command =
            new ZLinkCanonicalRelocationProtocol.ControlData(
                key.id(),
                key.attempt(),
                slot.prepare().coordinator(),
                ZLinkCanonicalRelocationProtocol.SOURCE,
                primaryParticipant(slot),
                2,
                source(slot.request()),
                9,
                slot.prepare().object(),
                0,
                0);
        return send(targetNodeRid,
                ZLinkCanonicalRelocationProtocol.encodeControlData(command))
            .thenCompose(ignored ->
                timed(slot.abortAck(), timeout, "relocation abort ACK"))
            .whenComplete((ignored, failure) -> {
                if (failure == null) {
                    sources.remove(key, slot);
                }
            });
    }

    @Override
    public CompletionStage<Void> finalizeAfterCompletion(
        RoutingId targetNodeRid,
        ZLinkSpotRetireControl.Fence fence,
        Duration timeout) {
        requireTimeout(timeout);
        Fence key = Fence.from(fence);
        SourceSlot slot = requireSource(key, targetNodeRid);
        List<ZLinkCanonicalRelocationProtocol.Terminal> terminals =
            terminalHighWater(slot);
        ZLinkCanonicalRelocationProtocol.Seal seal =
            new ZLinkCanonicalRelocationProtocol.Seal(
                key.id(),
                key.attempt(),
                slot.prepare().coordinator(),
                ZLinkCanonicalRelocationProtocol.SOURCE,
                false,
                terminals);
        return send(targetNodeRid,
                ZLinkCanonicalRelocationProtocol.encodeSeal(seal))
            .thenCompose(ignored ->
                timed(slot.sealAck(), timeout, "relocation seal"))
            .thenCompose(ignored -> {
                ZLinkCanonicalRelocationProtocol.Complete complete =
                    new ZLinkCanonicalRelocationProtocol.Complete(
                        key.id(),
                        key.attempt(),
                        slot.prepare().coordinator(),
                        ZLinkCanonicalRelocationProtocol.SOURCE,
                        source(slot.request()),
                        1);
                return send(targetNodeRid,
                    ZLinkCanonicalRelocationProtocol.encodeComplete(complete));
            })
            .whenComplete((ignored, failure) -> {
                if (failure == null) {
                    sources.remove(key, slot);
                }
            });
    }

    @Override
    public CompletionStage<Void> apply(
        RoutingId transportSource,
        int command,
        byte[] encoded) {
        try {
            return switch (command) {
                case ServiceWireConstants.COMMAND_RELOCATION_PREPARE ->
                    onPrepare(
                        transportSource,
                        ZLinkCanonicalRelocationProtocol.decodePrepare(
                            encoded));
                case ServiceWireConstants.COMMAND_RELOCATION_READY ->
                    onReady(
                        transportSource,
                        ZLinkCanonicalRelocationProtocol.decodeReady(encoded));
                case ServiceWireConstants.COMMAND_RELOCATION_RESERVED ->
                    onReserved(
                        transportSource,
                        ZLinkCanonicalRelocationProtocol.decodeReserved(
                            encoded));
                case ServiceWireConstants.COMMAND_RELOCATION_DATA ->
                    onControl(
                        transportSource,
                        ZLinkCanonicalRelocationProtocol.decodeControlData(
                            encoded));
                case ServiceWireConstants.COMMAND_RELOCATION_ACK ->
                    onAck(
                        transportSource,
                        ZLinkCanonicalRelocationProtocol.decodeAck(encoded));
                case ServiceWireConstants.COMMAND_RELOCATION_SEAL ->
                    onSeal(
                        transportSource,
                        ZLinkCanonicalRelocationProtocol.decodeSeal(encoded));
                case ServiceWireConstants.COMMAND_RELOCATION_COMPLETE ->
                    onComplete(
                        transportSource,
                        ZLinkCanonicalRelocationProtocol.decodeComplete(
                            encoded));
                default -> failed(new IllegalArgumentException(
                    "unsupported canonical relocation command"));
            };
        } catch (RuntimeException failure) {
            return failed(failure);
        }
    }

    private CompletionStage<Void> onPrepare(
        RoutingId transportSource,
        ZLinkCanonicalRelocationProtocol.Prepare prepare) {
        validateTargetCandidate(prepare.candidate());
        if (!transportSource.equals(prepare.sourceNodeRid())
            || prepare.initiatorRole()
                != ZLinkCanonicalRelocationProtocol.SOURCE) {
            return failed(new IllegalArgumentException(
                "canonical relocation prepare source differs"));
        }
        Fence fence = new Fence(prepare.id(), prepare.attempt());
        TargetSlot candidate = new TargetSlot(prepare);
        TargetSlot current = targets.putIfAbsent(fence, candidate);
        TargetSlot slot = current == null ? candidate : current;
        if (!Arrays.equals(
                ZLinkCanonicalRelocationProtocol.encodePrepare(slot.prepare()),
                ZLinkCanonicalRelocationProtocol.encodePrepare(prepare))) {
            return failed(new IllegalArgumentException(
                "duplicate canonical relocation prepare differs"));
        }
        if (current != null) {
            return slot.offer().thenCompose(offer ->
                send(transportSource,
                    ZLinkCanonicalRelocationProtocol.encodeReady(offer)));
        }
        reconstruct(prepare).whenComplete((request, failure) -> {
            if (failure != null) {
                targets.remove(fence, candidate);
                candidate.offer().completeExceptionally(unwrap(failure));
                return;
            }
            candidate.request().complete(request);
            long generation = nextReservationGeneration();
            ZLinkCanonicalRelocationProtocol.Ready offer =
                new ZLinkCanonicalRelocationProtocol.Ready(
                    prepare.id(),
                    prepare.attempt(),
                    prepare.round(),
                    prepare.coordinator(),
                    prepare.candidate(),
                    prepare.object(),
                    ZLinkCanonicalRelocationProtocol.TARGET,
                    prepare.requiredMessages(),
                    prepare.requiredBytes(),
                    prepare.participants(),
                    prepare.sourceNodeGeneration(),
                    localNodeGeneration,
                    generation,
                    prepare.root(),
                    prepare.applicationVersion(),
                    progress(prepare));
            candidate.reservationGeneration(generation);
            candidate.offer().complete(offer);
        });
        return candidate.offer().thenCompose(offer ->
            send(transportSource,
                ZLinkCanonicalRelocationProtocol.encodeReady(offer)));
    }

    private CompletionStage<Void> onReady(
        RoutingId transportSource,
        ZLinkCanonicalRelocationProtocol.Ready ready) {
        Fence fence = new Fence(ready.id(), ready.attempt());
        if (ready.role() == ZLinkCanonicalRelocationProtocol.TARGET) {
            SourceSlot slot = requireSource(fence, transportSource);
            validateOffer(slot.prepare(), ready, transportSource);
            ZLinkCanonicalRelocationProtocol.Ready acceptance =
                new ZLinkCanonicalRelocationProtocol.Ready(
                    ready.id(),
                    ready.attempt(),
                    ready.round(),
                    ready.coordinator(),
                    ready.candidate(),
                    ready.object(),
                    ZLinkCanonicalRelocationProtocol.SOURCE,
                    ready.offeredMessages(),
                    ready.offeredBytes(),
                    ready.participants(),
                    slot.prepare().sourceNodeGeneration(),
                    ready.targetNodeGeneration(),
                    ready.reservationGeneration(),
                    ready.root(),
                    ready.applicationVersion(),
                    ready.progress());
            return send(
                transportSource,
                ZLinkCanonicalRelocationProtocol.encodeReady(acceptance));
        }
        if (ready.role() != ZLinkCanonicalRelocationProtocol.SOURCE) {
            return failed(new IllegalArgumentException(
                "canonical relocation ready role is invalid"));
        }
        TargetSlot slot = requireTarget(fence, transportSource);
        validateAcceptance(slot, ready, transportSource);
        CompletionStage<Void> staged;
        synchronized (slot) {
            if (slot.staged() == null) {
                slot.staged(slot.request().thenCompose(target::stage));
            }
            staged = slot.staged();
        }
        return staged.thenCompose(ignored -> {
            ZLinkCanonicalRelocationProtocol.Reserved reserved =
                new ZLinkCanonicalRelocationProtocol.Reserved(
                    ready.id(),
                    ready.attempt(),
                    ready.round(),
                    ready.coordinator(),
                    ready.candidate(),
                    ready.reservationGeneration(),
                    ready.participants());
            return send(
                transportSource,
                ZLinkCanonicalRelocationProtocol.encodeReserved(reserved));
        });
    }

    private CompletionStage<Void> onReserved(
        RoutingId transportSource,
        ZLinkCanonicalRelocationProtocol.Reserved reserved) {
        Fence fence = new Fence(reserved.id(), reserved.attempt());
        SourceSlot slot = requireSource(fence, transportSource);
        if (reserved.reservationGeneration() <= 0
            || !reserved.participants().equals(
                slot.prepare().participants())
            || !reserved.candidate().equals(slot.prepare().candidate())
            || !reserved.coordinator().equals(
                slot.prepare().coordinator())) {
            return failed(new IllegalArgumentException(
                "canonical relocation reservation differs"));
        }
        slot.reserved().complete(null);
        return CompletableFuture.completedFuture(null);
    }

    private CompletionStage<Void> onControl(
        RoutingId transportSource,
        ZLinkCanonicalRelocationProtocol.ControlData control) {
        Fence fence = new Fence(control.id(), control.attempt());
        TargetSlot slot = targets.get(fence);
        if (slot == null) {
            RecoverySlot recovery = recoveries.get(fence);
            return recovery == null
                ? failed(new IllegalStateException(
                    "canonical relocation target slot is unavailable"))
                : onRecoveredControl(
                    fence, recovery, transportSource, control);
        }
        requireTarget(fence, transportSource);
        if (control.senderRole() != ZLinkCanonicalRelocationProtocol.SOURCE
            || !control.coordinator().equals(slot.prepare().coordinator())
            || !control.object().equals(slot.prepare().object())
            || !control.source().nodeRid().equals(transportSource)
            || control.source().nodeGeneration()
                != slot.prepare().sourceNodeGeneration()) {
            return failed(new IllegalArgumentException(
                "canonical relocation control fence differs"));
        }
        CompletionStage<Void> transition;
        synchronized (slot) {
            if (control.phase() == 4 && control.sequence() == 1) {
                if (slot.published() == null) {
                    CompletionStage<Void> publish = slot.staged().thenCompose(
                        ignored -> slot.request().thenCompose(
                            target::publish));
                    slot.published(publish);
                }
                transition = slot.published();
            } else if (control.phase() == 9 && control.sequence() == 2) {
                if (slot.published() != null) {
                    return failed(new IllegalStateException(
                        "published relocation cannot be aborted"));
                }
                if (slot.aborted() == null) {
                    slot.aborted(slot.request().thenCompose(target::abort));
                }
                transition = slot.aborted();
            } else {
                return failed(new IllegalArgumentException(
                    "canonical relocation control phase is invalid"));
            }
        }
        return transition.thenCompose(ignored -> {
            ZLinkCanonicalRelocationProtocol.Ack ack =
                new ZLinkCanonicalRelocationProtocol.Ack(
                    control.id(),
                    control.attempt(),
                    control.coordinator(),
                    ZLinkCanonicalRelocationProtocol.TARGET,
                    control.participantId(),
                    control.sequence());
            return send(
                transportSource,
                ZLinkCanonicalRelocationProtocol.encodeAck(ack));
        });
    }

    private CompletionStage<Void> onRecoveredControl(
        Fence fence,
        RecoverySlot recovery,
        RoutingId transportSource,
        ZLinkCanonicalRelocationProtocol.ControlData control) {
        ZLinkSpotRetireControl.StageRequest request = recovery.request();
        ZLinkSpotRetireControl.ParticipantFence primary =
            primary(request.participants());
        if (control.senderRole() != ZLinkCanonicalRelocationProtocol.SOURCE
            || control.phase() != 4
            || control.sequence() != 1
            || !transportSource.equals(request.sourceNodeRid())
            || !control.source().nodeRid().equals(request.sourceNodeRid())
            || control.source().nodeGeneration()
                != request.sourceNodeGeneration()
            || !control.coordinator().ownerId().equals(
                request.sourceOwnerId())
            || control.coordinator().ownerLeaseGeneration()
                != request.sourceOwnerLeaseGeneration()
            || control.object().kind() != primary.objectKind()
            || !control.object().objectId().equals(primary.objectId())
            || control.object().objectGeneration()
                != primary.objectGeneration()
            || control.object().expectedAuthorityOwnerGeneration()
                != primary.sourceAuthorityOwnerGeneration()) {
            return failed(new IllegalArgumentException(
                "recovered canonical relocation control fence differs"));
        }
        return recovery.ready().thenCompose(ignored -> {
            ZLinkCanonicalRelocationProtocol.Ack ack =
                new ZLinkCanonicalRelocationProtocol.Ack(
                    control.id(),
                    control.attempt(),
                    control.coordinator(),
                    ZLinkCanonicalRelocationProtocol.TARGET,
                    control.participantId(),
                    control.sequence());
            return send(
                transportSource,
                ZLinkCanonicalRelocationProtocol.encodeAck(ack));
        });
    }

    private CompletionStage<Void> onAck(
        RoutingId transportSource,
        ZLinkCanonicalRelocationProtocol.Ack ack) {
        Fence fence = new Fence(ack.id(), ack.attempt());
        SourceSlot slot = requireSource(fence, transportSource);
        if (ack.senderRole() != ZLinkCanonicalRelocationProtocol.TARGET
            || !ack.coordinator().equals(slot.prepare().coordinator())
            || ack.participantId() != primaryParticipant(slot)) {
            return failed(new IllegalArgumentException(
                "canonical relocation ACK fence differs"));
        }
        if (ack.highWater() == 1) {
            slot.publishAck().complete(null);
        } else if (ack.highWater() == 2) {
            slot.abortAck().complete(null);
        } else {
            return failed(new IllegalArgumentException(
                "canonical relocation ACK sequence is invalid"));
        }
        return CompletableFuture.completedFuture(null);
    }

    private CompletionStage<Void> onSeal(
        RoutingId transportSource,
        ZLinkCanonicalRelocationProtocol.Seal seal) {
        Fence fence = new Fence(seal.id(), seal.attempt());
        if (seal.response()) {
            SourceSlot slot = requireSource(fence, transportSource);
            if (seal.senderRole() != ZLinkCanonicalRelocationProtocol.TARGET
                || !seal.coordinator().equals(slot.prepare().coordinator())
                || !seal.participants().equals(terminalHighWater(slot))) {
                return failed(new IllegalArgumentException(
                    "canonical relocation seal response differs"));
            }
            slot.sealAck().complete(null);
            return CompletableFuture.completedFuture(null);
        }
        TargetSlot slot = targets.get(fence);
        if (slot == null) {
            RecoverySlot recovery = recoveries.get(fence);
            if (recovery == null) {
                return failed(new IllegalStateException(
                    "canonical relocation target slot is unavailable"));
            }
            if (seal.senderRole()
                    != ZLinkCanonicalRelocationProtocol.SOURCE
                || !transportSource.equals(
                    recovery.request().sourceNodeRid())
                || !matchesRecoveredCoordinator(
                    seal.coordinator(), recovery.request())) {
                return failed(new IllegalArgumentException(
                    "recovered canonical relocation seal fence differs"));
            }
            return recovery.ready().thenCompose(ignored -> {
                ZLinkCanonicalRelocationProtocol.Seal response =
                    new ZLinkCanonicalRelocationProtocol.Seal(
                        seal.id(),
                        seal.attempt(),
                        seal.coordinator(),
                        ZLinkCanonicalRelocationProtocol.TARGET,
                        true,
                        seal.participants());
                return send(
                    transportSource,
                    ZLinkCanonicalRelocationProtocol.encodeSeal(response));
            });
        }
        requireTarget(fence, transportSource);
        if (seal.senderRole() != ZLinkCanonicalRelocationProtocol.SOURCE
            || !seal.coordinator().equals(slot.prepare().coordinator())
            || slot.published() == null) {
            return failed(new IllegalStateException(
                "canonical relocation cannot be sealed"));
        }
        return slot.published().thenCompose(ignored -> {
            ZLinkCanonicalRelocationProtocol.Seal response =
                new ZLinkCanonicalRelocationProtocol.Seal(
                    seal.id(),
                    seal.attempt(),
                    seal.coordinator(),
                    ZLinkCanonicalRelocationProtocol.TARGET,
                    true,
                    seal.participants());
            return send(
                transportSource,
                ZLinkCanonicalRelocationProtocol.encodeSeal(response));
        });
    }

    private CompletionStage<Void> onComplete(
        RoutingId transportSource,
        ZLinkCanonicalRelocationProtocol.Complete complete) {
        Fence fence = new Fence(complete.id(), complete.attempt());
        TargetSlot slot = targets.get(fence);
        if (slot == null) {
            RecoverySlot recovery = recoveries.get(fence);
            if (recovery == null
                || complete.senderRole()
                    != ZLinkCanonicalRelocationProtocol.SOURCE
                || complete.cleanupState() != 1
                || !transportSource.equals(
                    recovery.request().sourceNodeRid())
                || !complete.source().nodeRid().equals(transportSource)
                || complete.source().nodeGeneration()
                    != recovery.request().sourceNodeGeneration()
                || !matchesRecoveredCoordinator(
                    complete.coordinator(), recovery.request())) {
                return failed(new IllegalArgumentException(
                    "recovered canonical relocation completion fence differs"));
            }
            return recovery.ready().thenCompose(ignored ->
                finalizeRecovery(fence, recovery));
        }
        requireTarget(fence, transportSource);
        if (complete.senderRole() != ZLinkCanonicalRelocationProtocol.SOURCE
            || complete.cleanupState() != 1
            || !complete.coordinator().equals(slot.prepare().coordinator())
            || !complete.source().nodeRid().equals(transportSource)
            || slot.published() == null) {
            return failed(new IllegalArgumentException(
                "canonical relocation completion fence differs"));
        }
        CompletionStage<Void> finalized;
        synchronized (slot) {
            if (slot.finalized() == null) {
                slot.finalized(slot.published().thenCompose(
                    ignored -> slot.request().thenCompose(
                        target::finalizeAfterCompletion)));
            }
            finalized = slot.finalized();
        }
        return finalized.thenRun(() -> targets.remove(fence, slot));
    }

    private CompletionStage<ZLinkCanonicalRelocationProtocol.Prepare>
        sourcePrepare(ZLinkSpotRetireControl.StageRequest request) {
        ZLinkSpotRetireControl.ParticipantFence primary =
            primary(request.participants());
        CompletionStage<ZLinkAuthorityReadResult> authority =
            locations.read(primary.authorityKey(), OPEN);
        CompletionStage<ZLinkAggregateRelocationCoordinator.Root> root =
            coordinator.readRoot(
                request.relocationReference(),
                request.relocationChecksum(),
                OPEN);
        return authority.thenCombine(root, (read, value) -> {
            if (!(read instanceof ZLinkAuthoritySnapshot snapshot)
                || snapshot.objectGeneration() != primary.objectGeneration()
                || snapshot.authorityOwnerGeneration()
                    != primary.sourceAuthorityOwnerGeneration()
                || !snapshot.ownerId().equals(request.sourceOwnerId())
                || snapshot.ownerLeaseGeneration()
                    != request.sourceOwnerLeaseGeneration()) {
                throw new IllegalStateException(
                    "source relocation authority fence is stale");
            }
            ZLinkServiceRelocationEnvelopeCodec.Envelope envelope =
                ZLinkServiceRelocationEnvelopeCodec.decode(value.payload());
            if (envelope.applicationStates().size()
                != request.participants().size()) {
                throw new IllegalStateException(
                    "relocation root participant count differs");
            }
            long requiredBytes = Math.max(1, value.payload().length);
            long requiredMessages = Math.multiplyExact(
                request.participants().size(),
                OFFER_MESSAGES_PER_PARTICIPANT);
            List<ZLinkCanonicalRelocationProtocol.Participant> participants =
                allowances(
                    envelope.applicationStates().stream()
                        .map(ZLinkServiceRelocationEnvelopeCodec
                            .ApplicationState::participantId)
                        .toList(),
                    requiredMessages,
                    requiredBytes);
            if (!request.sessionRoutes().isEmpty()) {
                List<ZLinkCanonicalRelocationProtocol.Participant> enriched =
                    new ArrayList<>(participants);
                for (int index = 0;
                    index < request.participants().size();
                    index++) {
                    ZLinkSpotRetireControl.ParticipantFence participant =
                        request.participants().get(index);
                    ZLinkSpotRetireControl.SessionRouteFence route =
                        request.sessionRoutes().stream()
                            .filter(routeValue -> routeValue.actorId().equals(
                                participant.objectId()))
                            .findFirst()
                            .orElse(null);
                    if (route == null) {
                        continue;
                    }
                    var allowance = enriched.get(index);
                    enriched.set(index,
                        new ZLinkCanonicalRelocationProtocol.Participant(
                            allowance.participantId(),
                            2,
                            route.sessionOwnerNodeRid(),
                            route.sessionOwnerNodeGeneration(),
                            route.sessionOwnerId(),
                            route.sessionOwnerLeaseGeneration(),
                            route.sessionRid(),
                            route.bindingGeneration(),
                            allowance.allowanceMessages(),
                            allowance.allowanceBytes()));
                }
                participants = List.copyOf(enriched);
            }
            return new ZLinkCanonicalRelocationProtocol.Prepare(
                request.fence().aggregateId(),
                request.fence().aggregateGeneration(),
                1,
                new ZLinkCanonicalRelocationProtocol.Coordinator(
                    request.sourceOwnerId(),
                    request.sourceOwnerLeaseGeneration(),
                    request.sourceNodeRid(),
                    request.sourceNodeGeneration(),
                    snapshot.storeVersion()),
                new ZLinkCanonicalRelocationProtocol.Candidate(
                    request.targetNodeRid(),
                    request.targetNodeGeneration(),
                    request.targetOwnerId(),
                    request.targetOwnerLeaseGeneration()),
                ZLinkCanonicalRelocationProtocol.SOURCE,
                new ZLinkCanonicalRelocationProtocol.ObjectFence(
                    primary.objectKind(),
                    primary.objectId(),
                    "",
                    primary.objectGeneration(),
                    primary.sourceAuthorityOwnerGeneration()),
                request.sourceNodeRid(),
                request.sourceNodeGeneration(),
                requiredMessages,
                requiredBytes,
                participants,
                new ZLinkCanonicalRelocationProtocol.Root(
                    request.relocationReference(),
                    request.relocationChecksum()),
                envelope.applicationVersion());
        });
    }

    private ZLinkSpotRetireControl.StageRequest recoveryRequest(
        ZLinkRelocationStartupScanner.Candidate candidate) {
        ZLinkServiceRelocationEnvelopeCodec.Envelope envelope =
            ZLinkServiceRelocationEnvelopeCodec.decode(
                candidate.root().payload());
        if (envelope.relocationHigh()
                != candidate.fence().aggregateId().getMostSignificantBits()
            || envelope.relocationLow()
                != candidate.fence().aggregateId().getLeastSignificantBits()
            || envelope.applicationStates().size()
                != candidate.authorities().size()) {
            throw new IllegalStateException(
                "published relocation recovery inventory differs");
        }
        Map<Long, Boolean> restore = new HashMap<>();
        for (var state : envelope.applicationStates()) {
            restore.put(state.participantId(), state.hasState());
        }
        ZLinkServiceAuthorityPayloadCodec spotCodec =
            new ZLinkServiceAuthorityPayloadCodec();
        ZLinkActorAuthorityPayloadCodec actorCodec =
            new ZLinkActorAuthorityPayloadCodec();
        List<ZLinkSpotRetireControl.ParticipantFence> participants =
            new ArrayList<>(candidate.authorities().size());
        String stableType = null;
        String targetSpotId = entrySpotId;
        boolean restorePrimary = false;
        for (int index = 0; index < candidate.authorities().size(); index++) {
            ZLinkAuthorityEntry entry = candidate.authorities().get(index);
            ZLinkAuthoritySnapshot snapshot = entry.snapshot();
            long participantId = index + 1L;
            boolean hasState = Boolean.TRUE.equals(restore.get(participantId));
            if (!snapshot.ownerId().equals(
                    candidate.targetOwner().ownerId())
                || snapshot.ownerLeaseGeneration()
                    != candidate.targetOwner().leaseGeneration()
                || !snapshot.allocation().descriptor().meshName().equals(
                    meshName)
                || !snapshot.allocation().descriptor().rid().equals(
                    localNodeRid)
                || snapshot.allocation().descriptorLifecycleGeneration()
                    != localNodeGeneration
                || snapshot.authorityOwnerGeneration() <= 1) {
                throw new IllegalStateException(
                    "published relocation recovery authority fence differs");
            }
            byte[] application =
                ZLinkCanonicalRelocationAuthorityStateCodec
                    .applicationPayloadOrOriginal(snapshot.payload());
            int objectKind = snapshot.allocation().objectKind().value();
            String objectId;
            String participantType;
            if (snapshot.allocation().objectKind()
                == systems.zlink.framework.locations
                    .ZLinkPlacementObjectKind.USER_SPOT) {
                var decoded = spotCodec.decode(application)
                    .orElseThrow(() -> new IllegalStateException(
                        "published User Spot recovery payload is invalid"));
                objectId = decoded.spotId();
                participantType = decoded.stableType();
                if (envelope.object().kind() == objectKind
                    && envelope.object().objectId().equals(objectId)) {
                    stableType = participantType;
                    targetSpotId = objectId;
                    restorePrimary = hasState;
                }
            } else if (snapshot.allocation().objectKind()
                == systems.zlink.framework.locations
                    .ZLinkPlacementObjectKind.ACTOR) {
                var decoded = actorCodec.decode(application)
                    .orElseThrow(() -> new IllegalStateException(
                        "published Actor recovery payload is invalid"));
                objectId = decoded.actorId();
                participantType = decoded.stableType();
                if (envelope.object().kind() == objectKind
                    && envelope.object().objectId().equals(objectId)) {
                    stableType = participantType;
                    restorePrimary = hasState;
                }
            } else {
                throw new IllegalStateException(
                    "published relocation recovery object kind is invalid");
            }
            participants.add(new ZLinkSpotRetireControl.ParticipantFence(
                entry.key(),
                objectKind,
                objectId,
                participantType,
                hasState,
                snapshot.objectGeneration(),
                snapshot.authorityOwnerGeneration() - 1));
        }
        if (stableType == null || stableType.isBlank()
            || targetSpotId == null || targetSpotId.isBlank()) {
            throw new IllegalStateException(
                "published relocation recovery primary object is missing");
        }
        return new ZLinkSpotRetireControl.StageRequest(
            new ZLinkSpotRetireControl.Fence(
                candidate.fence().aggregateId(),
                candidate.fence().aggregateGeneration()),
            candidate.sourceNodeRid(),
            candidate.sourceNodeGeneration(),
            candidate.sourceOwnerId(),
            candidate.sourceOwnerLeaseGeneration(),
            candidate.targetNodeRid(),
            candidate.targetNodeGeneration(),
            candidate.targetOwner().ownerId(),
            candidate.targetOwner().leaseGeneration(),
            meshName,
            targetSpotId,
            stableType,
            false,
            restorePrimary,
            candidate.reference(),
            candidate.checksumCrc32c(),
            participants,
            List.of());
    }

    private CompletionStage<ZLinkSpotRetireControl.StageRequest> reconstruct(
        ZLinkCanonicalRelocationProtocol.Prepare prepare) {
        if (prepare.root() == null) {
            return failed(new IllegalArgumentException(
                "canonical relocation root is required"));
        }
        CompletionStage<ZLinkAggregateRelocationCoordinator.Root> root =
            coordinator.readRoot(
                prepare.root().reference(),
                prepare.root().checksum(),
                OPEN);
        CompletionStage<List<ZLinkAuthorityEntry>> inventory =
            prepare.object().kind() == 1
                ? readStandaloneActor(prepare)
                : readUserSpotInventory(prepare);
        return root.thenCombine(inventory, (stored, entries) ->
            stageRequest(prepare, stored.payload(), entries));
    }

    private CompletionStage<List<ZLinkAuthorityEntry>> readStandaloneActor(
        ZLinkCanonicalRelocationProtocol.Prepare prepare) {
        String key = ZLinkAuthorityKeyCodec.actor(prepare.object().objectId());
        return locations.read(key, OPEN).thenApply(read -> {
            if (!(read instanceof ZLinkAuthoritySnapshot snapshot)) {
                throw new IllegalStateException(
                    "standalone Actor authority is missing");
            }
            return List.of(new ZLinkAuthorityEntry(key, snapshot));
        });
    }

    private CompletionStage<List<ZLinkAuthorityEntry>> readUserSpotInventory(
        ZLinkCanonicalRelocationProtocol.Prepare prepare) {
        String key = ZLinkAuthorityKeyCodec.spot(prepare.object().objectId());
        return locations.read(key, OPEN).thenCompose(read -> {
            if (!(read instanceof ZLinkAuthoritySnapshot snapshot)) {
                return failed(new IllegalStateException(
                    "User Spot authority is missing"));
            }
            return scanActors(Optional.empty(), new ArrayList<>())
                .thenApply(actors -> {
                    List<ZLinkAuthorityEntry> values = new ArrayList<>();
                    values.add(new ZLinkAuthorityEntry(key, snapshot));
                    ZLinkActorAuthorityPayloadCodec actorCodec =
                        new ZLinkActorAuthorityPayloadCodec();
                    for (ZLinkAuthorityEntry actor : actors) {
                        var authority = actorCodec.decode(
                            actor.snapshot().payload());
                        if (authority.isPresent()
                            && authority.get().currentSpotId().equals(
                                prepare.object().objectId())) {
                            values.add(actor);
                        }
                    }
                    values.sort(Comparator.comparing(
                        ZLinkAuthorityEntry::key,
                        ZLinkCanonicalRelocationStateMachine::compareUtf8));
                    return List.copyOf(values);
                });
        });
    }

    private CompletionStage<List<ZLinkAuthorityEntry>> scanActors(
        Optional<ZLinkAuthorityScanCursor> cursor,
        List<ZLinkAuthorityEntry> collected) {
        return locations.list(
                ACTOR_AUTHORITY_PREFIX,
                cursor,
                SCAN_PAGE_SIZE,
                OPEN)
            .thenCompose(result -> {
                if (result instanceof ZLinkAuthorityScanExpired) {
                    return failed(new IllegalStateException(
                        "Actor authority scan expired during relocation"));
                }
                ZLinkAuthorityPage page = (ZLinkAuthorityPage) result;
                collected.addAll(page.items());
                return page.nextCursor().isEmpty()
                    ? CompletableFuture.completedFuture(List.copyOf(collected))
                    : scanActors(page.nextCursor(), collected);
            });
    }

    private ZLinkSpotRetireControl.StageRequest stageRequest(
        ZLinkCanonicalRelocationProtocol.Prepare prepare,
        byte[] rootBytes,
        List<ZLinkAuthorityEntry> entries) {
        ZLinkServiceRelocationEnvelopeCodec.Envelope envelope =
            ZLinkServiceRelocationEnvelopeCodec.decode(rootBytes);
        if (envelope.relocationHigh()
                != prepare.id().getMostSignificantBits()
            || envelope.relocationLow()
                != prepare.id().getLeastSignificantBits()
            || envelope.object().kind() != prepare.object().kind()
            || !envelope.object().objectId().equals(
                prepare.object().objectId())
            || envelope.object().objectGeneration()
                != prepare.object().objectGeneration()
            || envelope.object().expectedAuthorityOwnerGeneration()
                != prepare.object().expectedAuthorityOwnerGeneration()
            || envelope.applicationVersion() != prepare.applicationVersion()
            || entries.size() != prepare.participants().size()
            || entries.size() != envelope.applicationStates().size()) {
            throw new IllegalStateException(
                "target relocation inventory differs from root");
        }
        Map<Long, Boolean> restore = new HashMap<>();
        for (var state : envelope.applicationStates()) {
            restore.put(state.participantId(), state.hasState());
        }
        List<ZLinkSpotRetireControl.ParticipantFence> participants =
            new ArrayList<>(entries.size());
        List<ZLinkSpotRetireControl.SessionRouteFence> sessionRoutes =
            new ArrayList<>();
        ZLinkServiceAuthorityPayloadCodec spotCodec =
            new ZLinkServiceAuthorityPayloadCodec();
        ZLinkActorAuthorityPayloadCodec actorCodec =
            new ZLinkActorAuthorityPayloadCodec();
        String stableType = null;
        String targetSpotId = entrySpotId;
        boolean restoreSpot = false;
        ZLinkAuthoritySnapshot primarySnapshot = null;
        for (int index = 0; index < entries.size(); index++) {
            ZLinkAuthorityEntry entry = entries.get(index);
            ZLinkAuthoritySnapshot snapshot = entry.snapshot();
            ZLinkCanonicalRelocationProtocol.Participant wire =
                prepare.participants().get(index);
            if (wire.participantId() != index + 1L) {
                throw new IllegalStateException(
                    "canonical participant identity order differs");
            }
            boolean hasState = Boolean.TRUE.equals(
                restore.get(wire.participantId()));
            if (!snapshot.ownerId().equals(
                    prepare.coordinator().ownerId())
                || snapshot.ownerLeaseGeneration()
                    != prepare.coordinator().ownerLeaseGeneration()
                || !snapshot.allocation().descriptor().meshName().equals(
                    meshName)
                || !snapshot.allocation().descriptor().rid().equals(
                    prepare.sourceNodeRid())
                || snapshot.allocation().descriptorLifecycleGeneration()
                    != prepare.sourceNodeGeneration()) {
                throw new IllegalStateException(
                    "target relocation source authority fence differs");
            }
            if (snapshot.allocation().objectKind()
                == systems.zlink.framework.locations
                    .ZLinkPlacementObjectKind.USER_SPOT) {
                var decoded = spotCodec.decode(snapshot.payload())
                    .orElseThrow(() -> new IllegalStateException(
                        "User Spot authority payload is invalid"));
                participants.add(new ZLinkSpotRetireControl.ParticipantFence(
                    entry.key(),
                    2,
                    decoded.spotId(),
                    decoded.stableType(),
                    hasState,
                    snapshot.objectGeneration(),
                    snapshot.authorityOwnerGeneration()));
                stableType = decoded.stableType();
                targetSpotId = decoded.spotId();
                restoreSpot = hasState;
            } else {
                var decoded = actorCodec.decode(snapshot.payload())
                    .orElseThrow(() -> new IllegalStateException(
                        "Actor authority payload is invalid"));
                participants.add(new ZLinkSpotRetireControl.ParticipantFence(
                    entry.key(),
                    1,
                    decoded.actorId(),
                    decoded.stableType(),
                    hasState,
                    snapshot.objectGeneration(),
                    snapshot.authorityOwnerGeneration()));
                if (prepare.object().kind() == 1) {
                    stableType = decoded.stableType();
                    restoreSpot = hasState;
                }
                if (wire.kind() == 2) {
                    sessionRoutes.add(
                        new ZLinkSpotRetireControl.SessionRouteFence(
                            decoded.actorId(),
                            snapshot.objectGeneration(),
                            snapshot.authorityOwnerGeneration(),
                            snapshot.storeVersion(),
                            wire.sessionOwnerNodeRid(),
                            wire.sessionOwnerNodeGeneration(),
                            wire.sessionOwnerId(),
                            wire.sessionOwnerLeaseGeneration(),
                            wire.sessionRid(),
                            wire.bindingGeneration(),
                            0));
                }
            }
            if (snapshot.allocation().objectKind().value()
                    == prepare.object().kind()
                && participants.getLast().objectId().equals(
                    prepare.object().objectId())) {
                primarySnapshot = snapshot;
            }
        }
        validateResolvedPrimary(prepare, participants);
        if (primarySnapshot == null
            || !primarySnapshot.storeVersion().equals(
                prepare.coordinator().expectedAuthorityStoreVersion())) {
            throw new IllegalStateException(
                "relocation coordinator authority version differs");
        }
        if (prepare.object().kind() == 1
            && (targetSpotId == null || targetSpotId.isBlank())) {
            throw new IllegalStateException(
                "target Entry Spot identity is unavailable");
        }
        return new ZLinkSpotRetireControl.StageRequest(
            new ZLinkSpotRetireControl.Fence(
                prepare.id(), prepare.attempt()),
            prepare.sourceNodeRid(),
            prepare.sourceNodeGeneration(),
            prepare.coordinator().ownerId(),
            prepare.coordinator().ownerLeaseGeneration(),
            prepare.candidate().nodeRid(),
            prepare.candidate().nodeGeneration(),
            prepare.candidate().ownerId(),
            prepare.candidate().ownerLeaseGeneration(),
            meshName,
            targetSpotId,
            stableType,
            false,
            restoreSpot,
            prepare.root().reference(),
            prepare.root().checksum(),
            participants,
            sessionRoutes);
    }

    private static void validateResolvedPrimary(
        ZLinkCanonicalRelocationProtocol.Prepare prepare,
        List<ZLinkSpotRetireControl.ParticipantFence> participants) {
        ZLinkSpotRetireControl.ParticipantFence primary =
            participants.stream()
                .filter(value ->
                    value.objectKind() == prepare.object().kind()
                        && value.objectId().equals(
                            prepare.object().objectId()))
                .findFirst()
                .orElseThrow(() -> new IllegalStateException(
                    "relocation primary authority is missing"));
        if (primary.objectGeneration()
                != prepare.object().objectGeneration()
            || primary.sourceAuthorityOwnerGeneration()
                != prepare.object().expectedAuthorityOwnerGeneration()) {
            throw new IllegalStateException(
                "relocation primary authority generation differs");
        }
    }

    private static List<ZLinkCanonicalRelocationProtocol.Participant>
        allowances(
        List<Long> ids,
        long messages,
        long bytes) {
        long messageBase = messages / ids.size();
        long messageRemainder = messages % ids.size();
        long byteBase = bytes / ids.size();
        long byteRemainder = bytes % ids.size();
        List<ZLinkCanonicalRelocationProtocol.Participant> result =
            new ArrayList<>(ids.size());
        for (int index = 0; index < ids.size(); index++) {
            result.add(ZLinkCanonicalRelocationProtocol.Participant.plain(
                ids.get(index),
                messageBase + (index < messageRemainder ? 1 : 0),
                byteBase + (index < byteRemainder ? 1 : 0)));
        }
        return List.copyOf(result);
    }

    private static List<ZLinkCanonicalRelocationProtocol.Progress> progress(
        ZLinkCanonicalRelocationProtocol.Prepare prepare) {
        return prepare.participants().stream()
            .map(value -> new ZLinkCanonicalRelocationProtocol.Progress(
                value.participantId(), 0, 0))
            .toList();
    }

    private void validateTargetCandidate(
        ZLinkCanonicalRelocationProtocol.Candidate candidate) {
        if (!candidate.nodeRid().equals(localNodeRid)
            || candidate.nodeGeneration() != localNodeGeneration) {
            throw new IllegalArgumentException(
                "canonical relocation target node fence is stale");
        }
    }

    private static void validateOffer(
        ZLinkCanonicalRelocationProtocol.Prepare prepare,
        ZLinkCanonicalRelocationProtocol.Ready ready,
        RoutingId transportSource) {
        if (!ready.candidate().nodeRid().equals(transportSource)
            || !ready.coordinator().equals(prepare.coordinator())
            || !ready.candidate().equals(prepare.candidate())
            || !ready.object().equals(prepare.object())
            || !ready.participants().equals(prepare.participants())
            || !Objects.equals(ready.root(), prepare.root())
            || ready.offeredMessages() < prepare.requiredMessages()
            || ready.offeredBytes() < prepare.requiredBytes()
            || ready.reservationGeneration() <= 0) {
            throw new IllegalArgumentException(
                "canonical relocation offer differs");
        }
    }

    private static void validateAcceptance(
        TargetSlot slot,
        ZLinkCanonicalRelocationProtocol.Ready ready,
        RoutingId transportSource) {
        ZLinkCanonicalRelocationProtocol.Prepare prepare = slot.prepare();
        if (!transportSource.equals(prepare.sourceNodeRid())
            || !ready.coordinator().equals(prepare.coordinator())
            || !ready.candidate().equals(prepare.candidate())
            || !ready.object().equals(prepare.object())
            || !ready.participants().equals(prepare.participants())
            || !Objects.equals(ready.root(), prepare.root())
            || ready.reservationGeneration() != slot.reservationGeneration()
            || ready.sourceNodeGeneration()
                != prepare.sourceNodeGeneration()
            || ready.targetNodeGeneration()
                != prepare.candidate().nodeGeneration()) {
            throw new IllegalArgumentException(
                "canonical relocation acceptance differs");
        }
    }

    private SourceSlot requireSource(Fence fence, RoutingId targetRid) {
        SourceSlot slot = sources.get(fence);
        if (slot == null
            || !slot.request().targetNodeRid().equals(targetRid)) {
            throw new IllegalStateException(
                "canonical source relocation is unavailable");
        }
        return slot;
    }

    private TargetSlot requireTarget(Fence fence, RoutingId sourceRid) {
        TargetSlot slot = targets.get(fence);
        if (slot == null
            || !slot.prepare().sourceNodeRid().equals(sourceRid)) {
            throw new IllegalStateException(
                "canonical target relocation is unavailable");
        }
        return slot;
    }

    private CompletionStage<Void> finalizeRecovery(
        Fence fence,
        RecoverySlot slot) {
        CompletionStage<Void> finalized;
        synchronized (slot) {
            if (slot.finalized() == null) {
                slot.finalized(target.finalizeAfterCompletion(
                    slot.request()));
            }
            finalized = slot.finalized();
        }
        return finalized.whenComplete((ignored, failure) -> {
            if (failure == null) {
                recoveries.remove(fence, slot);
            }
        });
    }

    private CompletionStage<Void> send(RoutingId targetRid, byte[] command) {
        return node.sendCanonicalRelocationControl(targetRid, command);
    }

    private CompletionStage<Void> sendUntilAck(
        RoutingId targetRid,
        byte[] command,
        CompletableFuture<Void> ack,
        Duration timeout,
        String operation) {
        CompletableFuture<Void> result = new CompletableFuture<>();
        long deadline = System.nanoTime() + timeout.toNanos();
        attemptUntilAck(targetRid, command, ack, deadline, result);
        ack.whenComplete((ignored, failure) -> {
            if (failure == null) {
                result.complete(null);
            } else {
                result.completeExceptionally(unwrap(failure));
            }
        });
        CompletableFuture.delayedExecutor(
            timeout.toMillis(),
            java.util.concurrent.TimeUnit.MILLISECONDS)
            .execute(() -> result.completeExceptionally(
                new java.util.concurrent.TimeoutException(
                    operation + " timed out")));
        return result;
    }

    private void attemptUntilAck(
        RoutingId targetRid,
        byte[] command,
        CompletableFuture<Void> ack,
        long deadline,
        CompletableFuture<Void> result) {
        if (result.isDone() || ack.isDone()
            || System.nanoTime() >= deadline) {
            return;
        }
        send(targetRid, command).whenComplete((ignored, failure) -> {
            if (result.isDone() || ack.isDone()) {
                return;
            }
            CompletableFuture.delayedExecutor(
                25,
                java.util.concurrent.TimeUnit.MILLISECONDS)
                .execute(() -> attemptUntilAck(
                    targetRid, command, ack, deadline, result));
        });
    }

    private long nextReservationGeneration() {
        return reservationGeneration.updateAndGet(
            current -> current == Long.MAX_VALUE ? 1 : current + 1);
    }

    private static long primaryParticipant(SourceSlot slot) {
        ZLinkCanonicalRelocationProtocol.ObjectFence object =
            slot.prepare().object();
        List<ZLinkSpotRetireControl.ParticipantFence> participants =
            slot.request().participants();
        for (int index = 0; index < participants.size(); index++) {
            var participant = participants.get(index);
            if (participant.objectKind() == object.kind()
                && participant.objectId().equals(object.objectId())) {
                return slot.prepare().participants().get(index)
                    .participantId();
            }
        }
        throw new IllegalStateException(
            "canonical primary relocation participant is unavailable");
    }

    private static List<ZLinkCanonicalRelocationProtocol.Terminal>
        terminalHighWater(SourceSlot slot) {
        long primary = primaryParticipant(slot);
        return slot.prepare().participants().stream()
            .map(value -> new ZLinkCanonicalRelocationProtocol.Terminal(
                value.participantId(),
                value.participantId() == primary ? 1 : 0))
            .toList();
    }

    private static ZLinkSpotRetireControl.ParticipantFence primary(
        List<ZLinkSpotRetireControl.ParticipantFence> participants) {
        return participants.stream()
            .filter(value -> value.objectKind() == 2)
            .findFirst()
            .orElse(participants.getFirst());
    }

    private static ZLinkCanonicalRelocationProtocol.Source source(
        ZLinkSpotRetireControl.StageRequest request) {
        return new ZLinkCanonicalRelocationProtocol.Source(
            request.sourceNodeRid(),
            request.sourceNodeGeneration(),
            request.sourceOwnerId(),
            request.sourceOwnerLeaseGeneration());
    }

    private static boolean matchesRecoveredCoordinator(
        ZLinkCanonicalRelocationProtocol.Coordinator coordinator,
        ZLinkSpotRetireControl.StageRequest request) {
        return coordinator.ownerId().equals(request.sourceOwnerId())
            && coordinator.ownerLeaseGeneration()
                == request.sourceOwnerLeaseGeneration()
            && coordinator.nodeRid().equals(request.sourceNodeRid())
            && coordinator.nodeGeneration()
                == request.sourceNodeGeneration();
    }

    private static int compareUtf8(String left, String right) {
        return Arrays.compareUnsigned(
            left.getBytes(StandardCharsets.UTF_8),
            right.getBytes(StandardCharsets.UTF_8));
    }

    private static String requireText(String value, String field) {
        if (value == null || value.isBlank()) {
            throw new IllegalArgumentException(field + " is required");
        }
        return value;
    }

    private static void requireTimeout(Duration timeout) {
        Objects.requireNonNull(timeout, "timeout");
        if (timeout.isZero() || timeout.isNegative()) {
            throw new IllegalArgumentException(
                "canonical relocation timeout must be positive");
        }
    }

    private static CompletionStage<Void> timed(
        CompletableFuture<Void> source,
        Duration timeout,
        String operation) {
        CompletableFuture<Void> result = new CompletableFuture<>();
        source.whenComplete((value, failure) -> {
            if (failure == null) {
                result.complete(null);
            } else {
                result.completeExceptionally(unwrap(failure));
            }
        });
        CompletableFuture.delayedExecutor(
            timeout.toMillis(),
            java.util.concurrent.TimeUnit.MILLISECONDS)
            .execute(() -> result.completeExceptionally(
                new java.util.concurrent.TimeoutException(
                    operation + " timed out")));
        return result;
    }

    private static Throwable unwrap(Throwable failure) {
        Throwable current = failure;
        while ((current instanceof java.util.concurrent.CompletionException
            || current instanceof java.util.concurrent.ExecutionException)
            && current.getCause() != null) {
            current = current.getCause();
        }
        return current;
    }

    private static <T> CompletionStage<T> failed(Throwable failure) {
        return CompletableFuture.failedFuture(failure);
    }

    private record Fence(UUID id, long attempt) {
        private Fence {
            Objects.requireNonNull(id, "id");
            if (attempt <= 0) {
                throw new IllegalArgumentException(
                    "relocation attempt must be positive");
            }
        }

        static Fence from(ZLinkSpotRetireControl.Fence value) {
            return new Fence(
                value.aggregateId(), value.aggregateGeneration());
        }
    }

    private record SourceSlot(
        ZLinkSpotRetireControl.StageRequest request,
        ZLinkCanonicalRelocationProtocol.Prepare prepare,
        CompletableFuture<Void> reserved,
        CompletableFuture<Void> publishAck,
        CompletableFuture<Void> abortAck,
        CompletableFuture<Void> sealAck) {
        SourceSlot(
            ZLinkSpotRetireControl.StageRequest request,
            ZLinkCanonicalRelocationProtocol.Prepare prepare) {
            this(
                request,
                prepare,
                new CompletableFuture<>(),
                new CompletableFuture<>(),
                new CompletableFuture<>(),
                new CompletableFuture<>());
        }
    }

    private static final class RecoverySlot {
        private final ZLinkSpotRetireControl.StageRequest request;
        private final CompletableFuture<Void> ready =
            new CompletableFuture<>();
        private CompletionStage<Void> finalized;

        RecoverySlot(ZLinkSpotRetireControl.StageRequest request) {
            this.request = request;
        }

        ZLinkSpotRetireControl.StageRequest request() {
            return request;
        }

        CompletableFuture<Void> ready() {
            return ready;
        }

        CompletionStage<Void> finalized() {
            return finalized;
        }

        void finalized(CompletionStage<Void> value) {
            finalized = value;
        }
    }

    private static final class TargetSlot {
        private final ZLinkCanonicalRelocationProtocol.Prepare prepare;
        private final CompletableFuture<ZLinkSpotRetireControl.StageRequest>
            request = new CompletableFuture<>();
        private final CompletableFuture<ZLinkCanonicalRelocationProtocol.Ready>
            offer = new CompletableFuture<>();
        private volatile long reservationGeneration;
        private CompletionStage<Void> staged;
        private CompletionStage<Void> published;
        private CompletionStage<Void> aborted;
        private CompletionStage<Void> finalized;

        TargetSlot(ZLinkCanonicalRelocationProtocol.Prepare prepare) {
            this.prepare = prepare;
        }

        ZLinkCanonicalRelocationProtocol.Prepare prepare() {
            return prepare;
        }

        CompletableFuture<ZLinkSpotRetireControl.StageRequest> request() {
            return request;
        }

        CompletableFuture<ZLinkCanonicalRelocationProtocol.Ready> offer() {
            return offer;
        }

        long reservationGeneration() {
            return reservationGeneration;
        }

        void reservationGeneration(long value) {
            reservationGeneration = value;
        }

        CompletionStage<Void> staged() {
            return staged;
        }

        void staged(CompletionStage<Void> value) {
            staged = value;
        }

        CompletionStage<Void> published() {
            return published;
        }

        void published(CompletionStage<Void> value) {
            published = value;
        }

        CompletionStage<Void> aborted() {
            return aborted;
        }

        void aborted(CompletionStage<Void> value) {
            aborted = value;
        }

        CompletionStage<Void> finalized() {
            return finalized;
        }

        void finalized(CompletionStage<Void> value) {
            finalized = value;
        }
    }
}
