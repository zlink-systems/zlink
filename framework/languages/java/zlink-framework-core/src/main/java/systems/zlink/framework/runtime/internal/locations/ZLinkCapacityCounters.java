package systems.zlink.framework.runtime.internal.locations;

import systems.zlink.framework.locationprovider.*;
import systems.zlink.framework.locations.ZLinkPlacementObjectKind;

import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;

/** Builds the atomic counter changes belonging to an authority write. */
final class ZLinkCapacityCounters {
    private final Map<ZLinkStoreKey, Long> deltas = new LinkedHashMap<>();

    static ZLinkStoreKey key(
            ZLinkMeshNodeDescriptorKey descriptor, String dimension, String state) {
        return new ZLinkStoreKey(prefix(descriptor) + dimension + '\0' + state);
    }

    static String prefix(ZLinkMeshNodeDescriptorKey descriptor) {
        return ZLinkOpaqueRecordKey.of(
                                "zlink:v12:capacity",
                                descriptor.meshName(),
                                descriptor.rid().toHex())
                        .value()
                + '\0';
    }

    static String spotType(ZLinkPlacementObjectKind kind, String stableType) {
        return "spot-type\0"
                + switch (kind) {
                    case USER_SPOT -> "userSpot";
                    case INSTANCE_SPOT -> "instanceSpot";
                    case ACTOR ->
                            throw new IllegalArgumentException("Actor is not a Spot capacity type");
                }
                + "\0"
                + stableType;
    }

    void change(
            ZLinkMeshNodeDescriptorKey descriptor,
            ZLinkPlacementCapacityBundle bundle,
            int pending,
            int active) {
        change(descriptor, "actors", bundle.actorSlots(), pending, active);
        change(descriptor, "spots", bundle.spotSlots(), pending, active);
        bundle.spotType()
                .ifPresent(
                        type ->
                                change(
                                        descriptor,
                                        spotType(type.objectKind(), type.stableType()),
                                        type.slots(),
                                        pending,
                                        active));
    }

    private void change(
            ZLinkMeshNodeDescriptorKey descriptor,
            String dimension,
            int slots,
            int pending,
            int active) {
        if (slots == 0) return;
        if (pending != 0)
            deltas.merge(
                    key(descriptor, dimension, "pending"), (long) slots * pending, Math::addExact);
        if (active != 0)
            deltas.merge(
                    key(descriptor, dimension, "active"), (long) slots * active, Math::addExact);
    }

    void appendTo(List<ZLinkStoreMutation> mutations) {
        deltas.forEach(
                (key, delta) -> {
                    if (delta != 0) mutations.add(new ZLinkStoreAdd(key, delta));
                });
    }

    static boolean reserve(
            ZLinkMeshNodeDescriptor descriptor,
            ZLinkPlacementCapacityBundle bundle,
            List<ZLinkStoreCondition> conditions,
            List<ZLinkStoreMutation> mutations) {
        var target = new ZLinkMeshNodeDescriptorKey(descriptor.meshName(), descriptor.rid());
        var spotType = bundle.spotType().orElse(null);
        int typeLimit = 0;
        if (spotType != null) {
            var capability =
                    descriptor.objectCapabilities().stream()
                            .filter(
                                    value ->
                                            value.objectKind() == spotType.objectKind()
                                                    && value.stableType()
                                                            .equals(spotType.stableType()))
                            .findFirst()
                            .orElse(null);
            if (capability == null) return false;
            typeLimit = capability.spotLimit();
        }
        limit(
                conditions,
                target,
                "actors",
                bundle.actorSlots(),
                descriptor.capacity().actors().limit());
        limit(
                conditions,
                target,
                "spots",
                bundle.spotSlots(),
                descriptor.capacity().spots().limit());
        if (spotType != null) {
            limit(
                    conditions,
                    target,
                    spotType(spotType.objectKind(), spotType.stableType()),
                    spotType.slots(),
                    typeLimit);
        }
        var deltas = new ZLinkCapacityCounters();
        deltas.change(target, bundle, 1, 0);
        deltas.appendTo(mutations);
        return true;
    }

    private static void limit(
            List<ZLinkStoreCondition> conditions,
            ZLinkMeshNodeDescriptorKey descriptor,
            String dimension,
            int slots,
            int limit) {
        if (slots != 0 && limit != 0) {
            conditions.add(
                    new ZLinkStoreSumAtMostCondition(
                            List.of(
                                    key(descriptor, dimension, "active"),
                                    key(descriptor, dimension, "pending")),
                            slots,
                            limit));
        }
    }
}
