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
        ZLinkLocationOwnerToken targetOwner,
        String representativeKey) {
    public ZLinkAggregatePrepareRequest(
            UUID aggregateId,
            long aggregateGeneration,
            List<ZLinkAggregateParticipant> participants,
            byte[] inventoryDigest,
            ZLinkMeshNodeDescriptorKey targetDescriptor,
            long targetDescriptorLifecycleGeneration,
            ZLinkPlacementCapacityBundle capacityBundle,
            ZLinkLocationOwnerToken targetOwner) {
        this(
                aggregateId,
                aggregateGeneration,
                participants,
                inventoryDigest,
                targetDescriptor,
                targetDescriptorLifecycleGeneration,
                capacityBundle,
                targetOwner,
                null);
    }

    public ZLinkAggregatePrepareRequest(
            UUID aggregateId,
            long aggregateGeneration,
            List<ZLinkAggregateParticipant> participants,
            ZLinkMeshNodeDescriptorKey targetDescriptor,
            long targetDescriptorLifecycleGeneration,
            ZLinkPlacementCapacityBundle capacityBundle,
            ZLinkLocationOwnerToken targetOwner) {
        this(
                aggregateId,
                aggregateGeneration,
                participants,
                ZLinkAggregateInventoryStore.digest(participants),
                targetDescriptor,
                targetDescriptorLifecycleGeneration,
                capacityBundle,
                targetOwner);
    }

    public ZLinkAggregatePrepareRequest {
        Objects.requireNonNull(aggregateId, "aggregateId");
        if (aggregateId.getMostSignificantBits() == 0L
                && aggregateId.getLeastSignificantBits() == 0L) {
            throw new IllegalArgumentException("aggregateId must not be zero");
        }
        if (aggregateGeneration <= 0) {
            throw new IllegalArgumentException("aggregateGeneration must be positive");
        }
        participants = List.copyOf(Objects.requireNonNull(participants, "participants"));
        inventoryDigest = Objects.requireNonNull(inventoryDigest, "inventoryDigest").clone();
        if (inventoryDigest.length != 32) {
            throw new IllegalArgumentException("inventoryDigest must contain exactly 32 bytes");
        }
        Objects.requireNonNull(targetDescriptor, "targetDescriptor");
        if (targetDescriptorLifecycleGeneration == 0) {
            throw new IllegalArgumentException(
                    "targetDescriptorLifecycleGeneration must be non-zero");
        }
        Objects.requireNonNull(capacityBundle, "capacityBundle");
        Objects.requireNonNull(targetOwner, "targetOwner");
        byte[] previous = null;
        for (ZLinkAggregateParticipant participant : participants) {
            Objects.requireNonNull(participant, "participant");
            byte[] current = participant.authorityKey().getBytes(StandardCharsets.UTF_8);
            if (previous != null && Arrays.compareUnsigned(previous, current) >= 0) {
                throw new IllegalArgumentException(
                        "participants must be sorted by UTF-8 key bytes and unique");
            }
            previous = current;
        }
    }

    @Override
    public byte[] inventoryDigest() {
        return inventoryDigest.clone();
    }
}
