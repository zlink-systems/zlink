package systems.zlink.framework.runtime.internal.locations;

import java.nio.charset.StandardCharsets;
import java.util.Arrays;
import java.util.List;
import java.util.Objects;
import java.util.UUID;

public record ZLinkAggregatePrepareRequest(
    UUID aggregateId,
    long aggregateGeneration,
    List<ZLinkAggregateParticipant> participants,
    byte[] inventoryDigest,
    ZLinkMeshNodeDescriptorKey targetDescriptor,
    long targetDescriptorLifecycleGeneration,
    ZLinkPlacementCapacityBundle capacityBundle,
    ZLinkLocationOwnerToken targetOwner) {
    public ZLinkAggregatePrepareRequest {
        Objects.requireNonNull(aggregateId, "aggregateId");
        if (aggregateId.getMostSignificantBits() == 0L
            && aggregateId.getLeastSignificantBits() == 0L) {
            throw new IllegalArgumentException(
                "aggregateId must not be zero");
        }
        if (aggregateGeneration <= 0) {
            throw new IllegalArgumentException(
                "aggregateGeneration must be positive");
        }
        participants = List.copyOf(
            Objects.requireNonNull(participants, "participants"));
        if (participants.isEmpty()) {
            throw new IllegalArgumentException(
                "participants must contain at least one entry");
        }
        inventoryDigest = Objects.requireNonNull(
            inventoryDigest,
            "inventoryDigest").clone();
        if (inventoryDigest.length != 32) {
            throw new IllegalArgumentException(
                "inventoryDigest must contain exactly 32 bytes");
        }
        Objects.requireNonNull(targetDescriptor, "targetDescriptor");
        if (targetDescriptorLifecycleGeneration <= 0) {
            throw new IllegalArgumentException(
                "targetDescriptorLifecycleGeneration must be positive");
        }
        Objects.requireNonNull(capacityBundle, "capacityBundle");
        Objects.requireNonNull(targetOwner, "targetOwner");
        byte[] previous = null;
        for (ZLinkAggregateParticipant participant : participants) {
            Objects.requireNonNull(participant, "participant");
            byte[] current = participant.authorityKey()
                .getBytes(StandardCharsets.UTF_8);
            if (previous != null
                && Arrays.compareUnsigned(previous, current) >= 0) {
                throw new IllegalArgumentException(
                    "participants must be sorted by UTF-8 key bytes and unique");
            }
            previous = current;
            if (participant.authorityPayload().length > 1024 * 1024
                || participant.membershipMutation().length > 1024 * 1024) {
                throw new IllegalArgumentException(
                    "aggregate participant value exceeds 1 MiB");
            }
        }
    }

    @Override
    public byte[] inventoryDigest() {
        return inventoryDigest.clone();
    }
}
