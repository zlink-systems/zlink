package systems.zlink.framework.runtime.spots;

import java.io.ByteArrayInputStream;
import java.io.ByteArrayOutputStream;
import java.io.DataInputStream;
import java.io.DataOutputStream;
import java.io.EOFException;
import java.io.IOException;
import java.nio.ByteBuffer;
import java.nio.charset.CharacterCodingException;
import java.nio.charset.CodingErrorAction;
import java.nio.charset.StandardCharsets;
import java.security.MessageDigest;
import java.security.NoSuchAlgorithmException;
import java.time.Duration;
import java.util.Arrays;
import java.util.List;
import java.util.Objects;
import java.util.UUID;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionException;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.ConcurrentHashMap;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.runtime.internal.backend.ZLinkInternalMeshNode;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceRelocationWireCodec;

/**
 * Infrastructure-only request/reply bridge for User Spot relocation control.
 * The immutable payload remains in Relocation Store; commands carry only the
 * exact aggregate, node, owner and root fences needed by the target.
 */
final class ZLinkSpotRetireControl {
    private static final int MAGIC = 0x5a4c5243; // ZLRC
    private static final int VERSION = 1;
    private static final int STAGE = 1;
    private static final int PUBLISH = 2;
    private static final int ABORT = 3;
    private static final int FINALIZE = 4;
    private static final int RELAY_REPLY = 5;
    private static final int RELAY_ACK = 126;
    private static final int ACK = 127;
    private static final int MAX_TEXT_BYTES = 4096;
    private static final int MAX_COMMAND_BYTES = 1024 * 1024;
    private static final int MAX_PARTICIPANTS = 1024;

    private ZLinkSpotRetireControl() {
    }

    static Client client(ZLinkInternalMeshNode node) {
        return new Client(node);
    }

    static Target install(
        ZLinkInternalMeshNode node,
        TargetEndpoint endpoint) {
        Target target = new Target(endpoint);
        node.setRelocationControlHandler(target::handle);
        return target;
    }

    static final class Client implements ZLinkRelocationTransitionClient {
        private final ZLinkInternalMeshNode node;

        private Client(ZLinkInternalMeshNode node) {
            this.node = Objects.requireNonNull(node, "node");
        }

        @Override
        public CompletionStage<Void> stage(
            RoutingId targetNodeRid,
            StageRequest request,
            Duration timeout) {
            return invoke(
                targetNodeRid,
                request.fence(),
                encodeStage(request),
                timeout);
        }

        @Override
        public CompletionStage<Void> publish(
            RoutingId targetNodeRid,
            Fence fence,
            Duration timeout) {
            return invoke(
                targetNodeRid,
                fence,
                encodeFence(PUBLISH, fence),
                timeout);
        }

        @Override
        public CompletionStage<Void> abort(
            RoutingId targetNodeRid,
            Fence fence,
            Duration timeout) {
            return invoke(
                targetNodeRid,
                fence,
                encodeFence(ABORT, fence),
                timeout);
        }

        @Override
        public CompletionStage<Void> finalizeAfterCompletion(
            RoutingId targetNodeRid,
            Fence fence,
            Duration timeout) {
            return invoke(
                targetNodeRid,
                fence,
                encodeFence(FINALIZE, fence),
                timeout);
        }

        CompletionStage<ZLinkSpotRelocationReplyRoutes.Ack> relayReply(
            RoutingId sourceNodeRid,
            Fence fence,
            ZLinkSpotRelocationReplyRoutes.Relay relay,
            Duration timeout) {
            return node.requestRelocationControl(
                    sourceNodeRid,
                    encodeRelay(fence, relay),
                    timeout)
                .thenApply(reply -> decodeRelayAck(reply, fence));
        }

        CompletionStage<byte[]> relayCanonicalReply(
            RoutingId sourceNodeRid,
            ZLinkServiceRelocationWireCodec.RequestSourceFence expectedSource,
            byte[] command33,
            List<byte[]> payload,
            Duration timeout) {
            return node.requestRelocationReplyRelay(
                sourceNodeRid, expectedSource, command33, payload, timeout);
        }

        private CompletionStage<Void> invoke(
            RoutingId targetNodeRid,
            Fence expectedFence,
            byte[] command,
            Duration timeout) {
            return node.requestRelocationControl(
                    targetNodeRid,
                    command,
                    timeout)
                .thenApply(reply -> {
                    if (!decodeAck(reply).equals(expectedFence)) {
                        throw new IllegalArgumentException(
                            "relocation acknowledgment fence differs");
                    }
                    return null;
                });
        }
    }

    static final class Target {
        private final TargetEndpoint endpoint;
        private final ConcurrentHashMap<Fence, Slot> slots =
            new ConcurrentHashMap<>();

        private Target(TargetEndpoint endpoint) {
            this.endpoint = Objects.requireNonNull(endpoint, "endpoint");
        }

        CompletionStage<byte[]> handle(
            RoutingId transportSource,
            byte[] encoded) {
            Command command = decode(encoded);
            if (command instanceof RelayReplyCommand relay) {
                return endpoint.relayReply(
                        transportSource,
                        relay.relay())
                    .thenApply(ack -> encodeRelayAck(
                        relay.fence(), ack));
            }
            if (command instanceof StageCommand stage) {
                if (!stage.request().sourceNodeRid().equals(transportSource)) {
                    return failed(new IllegalArgumentException(
                        "relocation source RID does not match transport"));
                }
                return stage(stage, encoded);
            }
            Slot slot = slots.get(command.fence());
            if (slot == null
                || !slot.request.sourceNodeRid().equals(transportSource)) {
                return failed(new IllegalStateException(
                    "relocation target stage is unavailable"));
            }
            if (command instanceof PublishCommand) {
                return publish(slot);
            }
            if (command instanceof FinalizeCommand) {
                return finalizeAfterCompletion(slot);
            }
            return abort(slot);
        }

        private CompletionStage<byte[]> stage(
            StageCommand command,
            byte[] encoded) {
            byte[] digest = sha256(encoded);
            Slot candidate = new Slot(command.request(), digest);
            Slot slot = slots.putIfAbsent(command.fence(), candidate);
            if (slot == null) {
                slot = candidate;
                try {
                    endpoint.stage(command.request())
                        .whenComplete((ignored, failure) -> {
                            if (failure == null) {
                                candidate.staged.complete(null);
                            } else {
                                slots.remove(command.fence(), candidate);
                                candidate.staged.completeExceptionally(
                                    unwrap(failure));
                            }
                        });
                } catch (RuntimeException failure) {
                    slots.remove(command.fence(), candidate);
                    candidate.staged.completeExceptionally(failure);
                }
            } else if (!Arrays.equals(slot.stageDigest, digest)) {
                return failed(new IllegalArgumentException(
                    "duplicate relocation stage payload differs"));
            }
            synchronized (slot) {
                if (slot.aborted) {
                    return failed(new IllegalStateException(
                        "aborted relocation cannot be staged again"));
                }
            }
            return slot.staged.thenApply(ignored -> encodeAck(command.fence()));
        }

        private CompletionStage<byte[]> publish(Slot slot) {
            synchronized (slot) {
                if (slot.aborted) {
                    return failed(new IllegalStateException(
                        "aborted relocation cannot be published"));
                }
                if (slot.published != null) {
                    return slot.published;
                }
                slot.published = slot.staged
                    .thenCompose(ignored -> endpoint.publish(slot.request))
                    .thenApply(ignored -> encodeAck(slot.request.fence()));
                return slot.published;
            }
        }

        private CompletionStage<byte[]> abort(Slot slot) {
            synchronized (slot) {
                if (slot.published != null) {
                    return failed(new IllegalStateException(
                        "published relocation cannot roll back to source"));
                }
                if (slot.aborted) {
                    return CompletableFuture.completedFuture(
                        encodeAck(slot.request.fence()));
                }
                slot.aborted = true;
            }
            return slot.staged.handle((ignored, stageFailure) -> null)
                .thenCompose(ignored -> endpoint.abort(slot.request))
                .thenApply(ignored -> encodeAck(slot.request.fence()));
        }

        private CompletionStage<byte[]> finalizeAfterCompletion(Slot slot) {
            synchronized (slot) {
                if (slot.aborted) {
                    return failed(new IllegalStateException(
                        "aborted relocation cannot be finalized"));
                }
                if (slot.published == null) {
                    return failed(new IllegalStateException(
                        "unpublished relocation cannot be finalized"));
                }
                if (slot.finalized != null) {
                    return slot.finalized;
                }
                slot.finalized = slot.published
                    .thenCompose(ignored -> endpoint.finalizeAfterCompletion(
                        slot.request))
                    .thenApply(ignored -> encodeAck(slot.request.fence()));
                return slot.finalized;
            }
        }
    }

    interface TargetEndpoint {
        CompletionStage<Void> stage(StageRequest request);

        CompletionStage<Void> publish(StageRequest request);

        CompletionStage<Void> abort(StageRequest request);

        CompletionStage<Void> finalizeAfterCompletion(StageRequest request);

        default CompletionStage<ZLinkSpotRelocationReplyRoutes.Ack> relayReply(
            RoutingId transportSource,
            ZLinkSpotRelocationReplyRoutes.Relay relay) {
            return CompletableFuture.failedFuture(new IllegalStateException(
                "relocation reply relay is unavailable"));
        }
    }

    record Fence(UUID aggregateId, long aggregateGeneration) {
        Fence {
            Objects.requireNonNull(aggregateId, "aggregateId");
            if (aggregateGeneration <= 0) {
                throw new IllegalArgumentException(
                    "aggregate generation must be positive");
            }
        }
    }

    record StageRequest(
        Fence fence,
        RoutingId sourceNodeRid,
        long sourceNodeGeneration,
        String sourceOwnerId,
        long sourceOwnerLeaseGeneration,
        RoutingId targetNodeRid,
        long targetNodeGeneration,
        String targetOwnerId,
        long targetOwnerLeaseGeneration,
        String meshName,
        String spotId,
        String stableType,
        boolean instanceSpot,
        boolean restoreSpotSnapshot,
        String relocationReference,
        long relocationChecksum,
        List<ParticipantFence> participants,
        List<SessionRouteFence> sessionRoutes) {
        StageRequest(
            Fence fence,
            RoutingId sourceNodeRid,
            long sourceNodeGeneration,
            String sourceOwnerId,
            long sourceOwnerLeaseGeneration,
            RoutingId targetNodeRid,
            long targetNodeGeneration,
            String targetOwnerId,
            long targetOwnerLeaseGeneration,
            String meshName,
            String spotId,
            String stableType,
            boolean instanceSpot,
            boolean restoreSpotSnapshot,
            String relocationReference,
            long relocationChecksum,
            List<ParticipantFence> participants) {
            this(
                fence,
                sourceNodeRid,
                sourceNodeGeneration,
                sourceOwnerId,
                sourceOwnerLeaseGeneration,
                targetNodeRid,
                targetNodeGeneration,
                targetOwnerId,
                targetOwnerLeaseGeneration,
                meshName,
                spotId,
                stableType,
                instanceSpot,
                restoreSpotSnapshot,
                relocationReference,
                relocationChecksum,
                participants,
                List.of());
        }

        StageRequest {
            Objects.requireNonNull(fence, "fence");
            Objects.requireNonNull(sourceNodeRid, "sourceNodeRid");
            Objects.requireNonNull(targetNodeRid, "targetNodeRid");
            requireText(sourceOwnerId, "sourceOwnerId");
            requireText(targetOwnerId, "targetOwnerId");
            requireText(meshName, "meshName");
            requireText(spotId, "spotId");
            requireText(stableType, "stableType");
            requireText(relocationReference, "relocationReference");
            participants = List.copyOf(Objects.requireNonNull(
                participants, "participants"));
            sessionRoutes = List.copyOf(Objects.requireNonNull(
                sessionRoutes, "sessionRoutes"));
            if (sourceNodeGeneration <= 0
                || sourceOwnerLeaseGeneration <= 0
                || targetNodeGeneration <= 0
                || targetOwnerLeaseGeneration <= 0
                || relocationChecksum < 0
                || relocationChecksum > 0xffff_ffffL) {
                throw new IllegalArgumentException(
                    "relocation stage contains an invalid generation or checksum");
            }
            if (participants.isEmpty()
                || participants.size() > MAX_PARTICIPANTS) {
                throw new IllegalArgumentException(
                    "relocation participant count is invalid");
            }
            String previous = null;
            for (ParticipantFence participant : participants) {
                if (previous != null
                    && compareUtf8(previous, participant.authorityKey()) >= 0) {
                    throw new IllegalArgumentException(
                        "relocation participants are not canonical");
                }
                previous = participant.authorityKey();
            }
            previous = null;
            for (SessionRouteFence route : sessionRoutes) {
                if (previous != null
                    && compareUtf8(previous, route.actorId()) >= 0) {
                    throw new IllegalArgumentException(
                        "Session routes are not canonical");
                }
                boolean actorMatches = participants.stream().anyMatch(
                    participant -> participant.objectKind() == 1
                        && participant.objectId().equals(route.actorId())
                        && participant.objectGeneration()
                            == route.actorObjectGeneration()
                        && participant.sourceAuthorityOwnerGeneration()
                            == route.sourceAuthorityOwnerGeneration());
                if (!actorMatches) {
                    throw new IllegalArgumentException(
                        "Session route is outside Actor inventory");
                }
                previous = route.actorId();
            }
        }
    }

    record SessionRouteFence(
        String actorId,
        long actorObjectGeneration,
        long sourceAuthorityOwnerGeneration,
        String sourceAuthorityStoreVersion,
        RoutingId sessionOwnerNodeRid,
        long sessionOwnerNodeGeneration,
        String sessionOwnerId,
        long sessionOwnerLeaseGeneration,
        RoutingId sessionRid,
        long bindingGeneration,
        long lastAcceptedSessionSequence) {
        SessionRouteFence {
            requireText(actorId, "actorId");
            requireText(
                sourceAuthorityStoreVersion, "sourceAuthorityStoreVersion");
            Objects.requireNonNull(
                sessionOwnerNodeRid, "sessionOwnerNodeRid");
            requireText(sessionOwnerId, "sessionOwnerId");
            Objects.requireNonNull(sessionRid, "sessionRid");
            positive(actorObjectGeneration, "actorObjectGeneration");
            positive(
                sourceAuthorityOwnerGeneration,
                "sourceAuthorityOwnerGeneration");
            positive(
                sessionOwnerNodeGeneration,
                "sessionOwnerNodeGeneration");
            positive(
                sessionOwnerLeaseGeneration,
                "sessionOwnerLeaseGeneration");
            positive(bindingGeneration, "bindingGeneration");
            if (lastAcceptedSessionSequence < 0) {
                throw new IllegalArgumentException(
                    "lastAcceptedSessionSequence must not be negative");
            }
        }
    }

    record ParticipantFence(
        String authorityKey,
        int objectKind,
        String objectId,
        String stableType,
        boolean restoreSnapshot,
        long objectGeneration,
        long sourceAuthorityOwnerGeneration) {
        ParticipantFence {
            requireText(authorityKey, "authorityKey");
            if (objectKind != 1 && objectKind != 2) {
                throw new IllegalArgumentException(
                    "participant objectKind must be Actor or User Spot");
            }
            requireText(objectId, "objectId");
            requireText(stableType, "stableType");
            positive(objectGeneration, "objectGeneration");
            positive(
                sourceAuthorityOwnerGeneration,
                "sourceAuthorityOwnerGeneration");
        }
    }

    private sealed interface Command permits
        StageCommand, PublishCommand, AbortCommand, FinalizeCommand,
        RelayReplyCommand {
        Fence fence();
    }

    private record StageCommand(StageRequest request) implements Command {
        @Override public Fence fence() { return request.fence(); }
    }

    private record PublishCommand(Fence fence) implements Command {
    }

    private record AbortCommand(Fence fence) implements Command {
    }

    private record FinalizeCommand(Fence fence) implements Command {
    }

    private record RelayReplyCommand(
        Fence fence,
        ZLinkSpotRelocationReplyRoutes.Relay relay) implements Command {
    }

    private static final class Slot {
        private final StageRequest request;
        private final byte[] stageDigest;
        private final CompletableFuture<Void> staged = new CompletableFuture<>();
        private CompletionStage<byte[]> published;
        private CompletionStage<byte[]> finalized;
        private boolean aborted;

        private Slot(StageRequest request, byte[] stageDigest) {
            this.request = request;
            this.stageDigest = stageDigest;
        }
    }

    private static byte[] encodeStage(StageRequest request) {
        return write(STAGE, output -> {
            writeFence(output, request.fence());
            writeRid(output, request.sourceNodeRid());
            output.writeLong(request.sourceNodeGeneration());
            writeText(output, request.sourceOwnerId());
            output.writeLong(request.sourceOwnerLeaseGeneration());
            writeRid(output, request.targetNodeRid());
            output.writeLong(request.targetNodeGeneration());
            writeText(output, request.targetOwnerId());
            output.writeLong(request.targetOwnerLeaseGeneration());
            writeText(output, request.meshName());
            writeText(output, request.spotId());
            writeText(output, request.stableType());
            output.writeBoolean(request.instanceSpot());
            output.writeBoolean(request.restoreSpotSnapshot());
            writeText(output, request.relocationReference());
            output.writeInt((int) request.relocationChecksum());
            output.writeInt(request.participants().size());
            for (ParticipantFence participant : request.participants()) {
                writeText(output, participant.authorityKey());
                output.writeByte(participant.objectKind());
                writeText(output, participant.objectId());
                writeText(output, participant.stableType());
                output.writeBoolean(participant.restoreSnapshot());
                output.writeLong(participant.objectGeneration());
                output.writeLong(
                    participant.sourceAuthorityOwnerGeneration());
            }
            output.writeInt(request.sessionRoutes().size());
            for (SessionRouteFence route : request.sessionRoutes()) {
                writeText(output, route.actorId());
                output.writeLong(route.actorObjectGeneration());
                output.writeLong(route.sourceAuthorityOwnerGeneration());
                writeText(output, route.sourceAuthorityStoreVersion());
                writeRid(output, route.sessionOwnerNodeRid());
                output.writeLong(route.sessionOwnerNodeGeneration());
                writeText(output, route.sessionOwnerId());
                output.writeLong(route.sessionOwnerLeaseGeneration());
                writeRid(output, route.sessionRid());
                output.writeLong(route.bindingGeneration());
                output.writeLong(route.lastAcceptedSessionSequence());
            }
        });
    }

    private static byte[] encodeFence(int kind, Fence fence) {
        return write(kind, output -> writeFence(output, fence));
    }

    private static byte[] encodeRelay(
        Fence fence,
        ZLinkSpotRelocationReplyRoutes.Relay relay) {
        return write(RELAY_REPLY, output -> {
            writeFence(output, fence);
            output.writeLong(relay.operation().high());
            output.writeLong(relay.operation().low());
            output.writeLong(relay.replyRouteId());
            writeText(output, relay.spotId());
            output.writeLong(relay.objectGeneration());
            writeText(output, relay.sourceOwnerId());
            output.writeLong(relay.sourceOwnerLeaseGeneration());
            writeRid(output, relay.sourceNodeRid());
            output.writeLong(relay.sourceNodeGeneration());
            output.writeLong(relay.targetNodeGeneration());
            output.writeLong(relay.targetAuthorityOwnerGeneration());
            output.writeInt(relay.hopCount());
            output.writeInt(relay.parts().size());
            for (byte[] part : relay.parts()) {
                output.writeInt(part.length);
                output.write(part);
            }
        });
    }

    private static byte[] encodeAck(Fence fence) {
        return write(ACK, output -> writeFence(output, fence));
    }

    private static Command decode(byte[] encoded) {
        byte[] bytes = Objects.requireNonNull(encoded, "encoded");
        if (bytes.length == 0 || bytes.length > MAX_COMMAND_BYTES) {
            throw new IllegalArgumentException(
                "relocation command exceeds its size bound");
        }
        try {
            DataInputStream input = new DataInputStream(
                new ByteArrayInputStream(bytes));
            if (input.readInt() != MAGIC || input.readUnsignedByte() != VERSION) {
                throw new IllegalArgumentException(
                    "relocation command prefix is invalid");
            }
            int kind = input.readUnsignedByte();
            Command command;
            if (kind == STAGE) {
                Fence fence = readFence(input);
                RoutingId sourceNode = readRid(input);
                long sourceNodeGeneration = positive(
                    input.readLong(), "sourceNodeGeneration");
                String sourceOwner = readText(input);
                long sourceOwnerGeneration = positive(
                    input.readLong(), "sourceOwnerLeaseGeneration");
                RoutingId targetNode = readRid(input);
                long targetNodeGeneration = positive(
                    input.readLong(), "targetNodeGeneration");
                String targetOwner = readText(input);
                long targetOwnerGeneration = positive(
                    input.readLong(), "targetOwnerLeaseGeneration");
                String meshName = readText(input);
                String spotId = readText(input);
                String stableType = readText(input);
                boolean instanceSpot = input.readBoolean();
                boolean restoreSpotSnapshot = input.readBoolean();
                String reference = readText(input);
                long checksum = Integer.toUnsignedLong(input.readInt());
                int participantCount = input.readInt();
                if (participantCount < 1
                    || participantCount > MAX_PARTICIPANTS) {
                    throw new IllegalArgumentException(
                        "relocation participant count is invalid");
                }
                java.util.ArrayList<ParticipantFence> participants =
                    new java.util.ArrayList<>(participantCount);
                for (int index = 0; index < participantCount; index++) {
                    participants.add(new ParticipantFence(
                        readText(input),
                        input.readUnsignedByte(),
                        readText(input),
                        readText(input),
                        input.readBoolean(),
                        positive(input.readLong(), "objectGeneration"),
                        positive(
                            input.readLong(),
                            "sourceAuthorityOwnerGeneration")));
                }
                int sessionRouteCount = input.readInt();
                if (sessionRouteCount < 0
                    || sessionRouteCount > MAX_PARTICIPANTS) {
                    throw new IllegalArgumentException(
                        "Session route count is invalid");
                }
                java.util.ArrayList<SessionRouteFence> sessionRoutes =
                    new java.util.ArrayList<>(sessionRouteCount);
                for (int index = 0; index < sessionRouteCount; index++) {
                    sessionRoutes.add(new SessionRouteFence(
                        readText(input),
                        positive(input.readLong(), "actorObjectGeneration"),
                        positive(
                            input.readLong(),
                            "sourceAuthorityOwnerGeneration"),
                        readText(input),
                        readRid(input),
                        positive(
                            input.readLong(),
                            "sessionOwnerNodeGeneration"),
                        readText(input),
                        positive(
                            input.readLong(),
                            "sessionOwnerLeaseGeneration"),
                        readRid(input),
                        positive(input.readLong(), "bindingGeneration"),
                        nonnegative(
                            input.readLong(),
                            "lastAcceptedSessionSequence")));
                }
                command = new StageCommand(new StageRequest(
                    fence,
                    sourceNode,
                    sourceNodeGeneration,
                    sourceOwner,
                    sourceOwnerGeneration,
                    targetNode,
                    targetNodeGeneration,
                    targetOwner,
                    targetOwnerGeneration,
                    meshName,
                    spotId,
                    stableType,
                    instanceSpot,
                    restoreSpotSnapshot,
                    reference,
                    checksum,
                    participants,
                    sessionRoutes));
            } else if (kind == PUBLISH) {
                command = new PublishCommand(readFence(input));
            } else if (kind == ABORT) {
                command = new AbortCommand(readFence(input));
            } else if (kind == FINALIZE) {
                command = new FinalizeCommand(readFence(input));
            } else if (kind == RELAY_REPLY) {
                Fence fence = readFence(input);
                var operation = new ZLinkSpotRelocationReplyRoutes.OperationId(
                    input.readLong(), input.readLong());
                long replyRouteId = positive(
                    input.readLong(), "replyRouteId");
                String spotId = readText(input);
                long objectGeneration = positive(
                    input.readLong(), "objectGeneration");
                String sourceOwnerId = readText(input);
                long sourceOwnerLeaseGeneration = positive(
                    input.readLong(), "sourceOwnerLeaseGeneration");
                RoutingId sourceNodeRid = readRid(input);
                long sourceNodeGeneration = positive(
                    input.readLong(), "sourceNodeGeneration");
                long targetNodeGeneration = positive(
                    input.readLong(), "targetNodeGeneration");
                long targetAuthorityOwnerGeneration = positive(
                    input.readLong(), "targetAuthorityOwnerGeneration");
                int hopCount = input.readInt();
                int partCount = input.readInt();
                if (partCount < 1 || partCount > 64) {
                    throw new IllegalArgumentException(
                        "relocation reply part count is invalid");
                }
                java.util.ArrayList<byte[]> parts =
                    new java.util.ArrayList<>(partCount);
                for (int index = 0; index < partCount; index++) {
                    int length = input.readInt();
                    if (length < 0 || length > MAX_COMMAND_BYTES) {
                        throw new IllegalArgumentException(
                            "relocation reply part is too large");
                    }
                    byte[] part = input.readNBytes(length);
                    if (part.length != length) {
                        throw new EOFException();
                    }
                    parts.add(part);
                }
                command = new RelayReplyCommand(fence,
                    new ZLinkSpotRelocationReplyRoutes.Relay(
                        operation,
                        replyRouteId,
                        spotId,
                        objectGeneration,
                        sourceOwnerId,
                        sourceOwnerLeaseGeneration,
                        sourceNodeRid,
                        sourceNodeGeneration,
                        targetNodeGeneration,
                        targetAuthorityOwnerGeneration,
                        hopCount,
                        parts));
            } else {
                throw new IllegalArgumentException(
                    "relocation command kind is invalid");
            }
            if (input.available() != 0) {
                throw new IllegalArgumentException(
                    "relocation command contains trailing bytes");
            }
            return command;
        } catch (EOFException failure) {
            throw new IllegalArgumentException(
                "relocation command is truncated", failure);
        } catch (IOException failure) {
            throw new IllegalArgumentException(
                "relocation command is invalid", failure);
        }
    }

    private static Fence decodeAck(byte[] encoded) {
        try {
            DataInputStream input = new DataInputStream(
                new ByteArrayInputStream(encoded));
            if (input.readInt() != MAGIC
                || input.readUnsignedByte() != VERSION
                || input.readUnsignedByte() != ACK) {
                throw new IllegalArgumentException(
                    "relocation command acknowledgment is invalid");
            }
            Fence fence = readFence(input);
            if (input.available() != 0) {
                throw new IllegalArgumentException(
                    "relocation acknowledgment contains trailing bytes");
            }
            return fence;
        } catch (IOException failure) {
            throw new IllegalArgumentException(
                "relocation acknowledgment is invalid", failure);
        }
    }

    private static byte[] encodeRelayAck(
        Fence fence,
        ZLinkSpotRelocationReplyRoutes.Ack ack) {
        return write(RELAY_ACK, output -> {
            writeFence(output, fence);
            output.writeByte(ack.ordinal());
        });
    }

    private static ZLinkSpotRelocationReplyRoutes.Ack decodeRelayAck(
        byte[] encoded,
        Fence expectedFence) {
        try {
            DataInputStream input = new DataInputStream(
                new ByteArrayInputStream(encoded));
            if (input.readInt() != MAGIC
                || input.readUnsignedByte() != VERSION
                || input.readUnsignedByte() != RELAY_ACK
                || !readFence(input).equals(expectedFence)) {
                throw new IllegalArgumentException(
                    "relocation reply acknowledgment is invalid");
            }
            int state = input.readUnsignedByte();
            if (state >= ZLinkSpotRelocationReplyRoutes.Ack.values().length
                || input.available() != 0) {
                throw new IllegalArgumentException(
                    "relocation reply acknowledgment state is invalid");
            }
            return ZLinkSpotRelocationReplyRoutes.Ack.values()[state];
        } catch (IOException failure) {
            throw new IllegalArgumentException(
                "relocation reply acknowledgment is invalid", failure);
        }
    }

    private static byte[] write(int kind, IoWriter writer) {
        try {
            ByteArrayOutputStream bytes = new ByteArrayOutputStream();
            DataOutputStream output = new DataOutputStream(bytes);
            output.writeInt(MAGIC);
            output.writeByte(VERSION);
            output.writeByte(kind);
            writer.write(output);
            output.flush();
            byte[] result = bytes.toByteArray();
            if (result.length > MAX_COMMAND_BYTES) {
                throw new IllegalArgumentException(
                    "relocation command exceeds its size bound");
            }
            return result;
        } catch (IOException failure) {
            throw new IllegalStateException(
                "relocation command could not be encoded", failure);
        }
    }

    private static void writeFence(DataOutputStream output, Fence fence)
        throws IOException {
        output.writeLong(fence.aggregateId().getMostSignificantBits());
        output.writeLong(fence.aggregateId().getLeastSignificantBits());
        output.writeLong(fence.aggregateGeneration());
    }

    private static Fence readFence(DataInputStream input) throws IOException {
        return new Fence(
            new UUID(input.readLong(), input.readLong()),
            positive(input.readLong(), "aggregateGeneration"));
    }

    private static void writeRid(DataOutputStream output, RoutingId rid)
        throws IOException {
        byte[] bytes = rid.toBytes();
        output.writeShort(bytes.length);
        output.write(bytes);
    }

    private static RoutingId readRid(DataInputStream input)
        throws IOException {
        int length = input.readUnsignedShort();
        if (length < 1 || length > 255) {
            throw new IllegalArgumentException(
                "relocation RID length is invalid");
        }
        byte[] bytes = input.readNBytes(length);
        if (bytes.length != length) {
            throw new EOFException();
        }
        return RoutingId.from(bytes);
    }

    private static void writeText(DataOutputStream output, String value)
        throws IOException {
        byte[] bytes = requireText(value, "value")
            .getBytes(StandardCharsets.UTF_8);
        output.writeShort(bytes.length);
        output.write(bytes);
    }

    private static String readText(DataInputStream input) throws IOException {
        int length = input.readUnsignedShort();
        if (length < 1 || length > MAX_TEXT_BYTES) {
            throw new IllegalArgumentException(
                "relocation text length is invalid");
        }
        byte[] bytes = input.readNBytes(length);
        if (bytes.length != length) {
            throw new EOFException();
        }
        try {
            return StandardCharsets.UTF_8.newDecoder()
                .onMalformedInput(CodingErrorAction.REPORT)
                .onUnmappableCharacter(CodingErrorAction.REPORT)
                .decode(ByteBuffer.wrap(bytes))
                .toString();
        } catch (CharacterCodingException failure) {
            throw new IllegalArgumentException(
                "relocation text is not strict UTF-8", failure);
        }
    }

    private static String requireText(String value, String name) {
        if (value == null || value.isBlank()) {
            throw new IllegalArgumentException(name + " is required");
        }
        int length = value.getBytes(StandardCharsets.UTF_8).length;
        if (length > MAX_TEXT_BYTES) {
            throw new IllegalArgumentException(
                name + " exceeds its UTF-8 bound");
        }
        return value;
    }

    private static long positive(long value, String name) {
        if (value <= 0) {
            throw new IllegalArgumentException(name + " must be positive");
        }
        return value;
    }

    private static long nonnegative(long value, String name) {
        if (value < 0) {
            throw new IllegalArgumentException(name + " must not be negative");
        }
        return value;
    }

    private static int compareUtf8(String left, String right) {
        return Arrays.compareUnsigned(
            left.getBytes(StandardCharsets.UTF_8),
            right.getBytes(StandardCharsets.UTF_8));
    }

    private static byte[] sha256(byte[] value) {
        try {
            return MessageDigest.getInstance("SHA-256").digest(value);
        } catch (NoSuchAlgorithmException failure) {
            throw new IllegalStateException("SHA-256 is unavailable", failure);
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

    private static <T> CompletionStage<T> failed(Throwable failure) {
        return CompletableFuture.failedFuture(failure);
    }

    @FunctionalInterface
    private interface IoWriter {
        void write(DataOutputStream output) throws IOException;
    }
}
