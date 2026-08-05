package systems.zlink.framework.runtime.spots;

import java.util.HashMap;
import java.util.Map;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.locations.ZLinkCapacityUsage;
import systems.zlink.framework.locations.ZLinkPlacementObjectKind;
import systems.zlink.framework.runtime.internal.locations.ZLinkMeshNodeDescriptor;

/**
 * Accounts for capacity consumed by every unit in one host relocation
 * preflight without changing provider state.
 */
final class ZLinkRelocationCapacityPlan {
    private final Map<NodeKey, Reserved> reservations = new HashMap<>();
    private final Map<ObjectKey, ZLinkMeshNodeDescriptor> targets = new HashMap<>();

    boolean canReserveUserSpot(
        ZLinkMeshNodeDescriptor candidate,
        String stableType,
        int actorCount) {
        Reserved reserved = reservations.getOrDefault(
            NodeKey.of(candidate), Reserved.EMPTY);
        return fits(candidate.capacity().spots(), reserved.spots(), 1)
            && fits(
                candidate.capacity().actors(),
                reserved.actors(),
                actorCount)
            && fitsSpotType(
                candidate,
                stableType,
                reserved.spotTypes().getOrDefault(stableType, 0));
    }

    void reserveUserSpot(
        String spotId,
        ZLinkMeshNodeDescriptor candidate,
        String stableType,
        int actorCount) {
        NodeKey key = NodeKey.of(candidate);
        Reserved current = reservations.getOrDefault(key, Reserved.EMPTY);
        Map<String, Integer> spotTypes =
            new HashMap<>(current.spotTypes());
        spotTypes.merge(stableType, 1, Math::addExact);
        reservations.put(key, new Reserved(
            Math.addExact(current.actors(), actorCount),
            Math.addExact(current.spots(), 1),
            Map.copyOf(spotTypes)));
        targets.put(new ObjectKey(ObjectKind.SPOT, spotId), candidate);
    }

    boolean canReserveActor(ZLinkMeshNodeDescriptor candidate) {
        Reserved reserved = reservations.getOrDefault(
            NodeKey.of(candidate), Reserved.EMPTY);
        return fits(candidate.capacity().actors(), reserved.actors(), 1);
    }

    void reserveActor(String actorId, ZLinkMeshNodeDescriptor candidate) {
        NodeKey key = NodeKey.of(candidate);
        Reserved current = reservations.getOrDefault(key, Reserved.EMPTY);
        reservations.put(key, new Reserved(
            Math.addExact(current.actors(), 1),
            current.spots(),
            current.spotTypes()));
        targets.put(new ObjectKey(ObjectKind.ACTOR, actorId), candidate);
    }

    ZLinkMeshNodeDescriptor userSpotTarget(String spotId) {
        return requireTarget(ObjectKind.SPOT, spotId);
    }

    ZLinkMeshNodeDescriptor actorTarget(String actorId) {
        return requireTarget(ObjectKind.ACTOR, actorId);
    }


    private ZLinkMeshNodeDescriptor requireTarget(
        ObjectKind kind, String objectId) {
        ZLinkMeshNodeDescriptor target = targets.get(new ObjectKey(kind, objectId));
        if (target == null) {
            throw new IllegalStateException(
                "Relocation target was not pinned during preflight: " + objectId);
        }
        return target;
    }

    private static boolean fits(
        ZLinkCapacityUsage usage,
        int alreadyReserved,
        int required) {
        return usage.limit() == 0
            || (long) usage.active()
                + usage.reserved()
                + alreadyReserved
                + required
                <= usage.limit();
    }

    private static boolean fitsSpotType(
        ZLinkMeshNodeDescriptor candidate,
        String stableType,
        int alreadyReserved) {
        return candidate.capacity().spotTypes().stream()
            .filter(type ->
                type.objectKind() == ZLinkPlacementObjectKind.USER_SPOT
                    && type.stableType().equals(stableType))
            .findFirst()
            .map(type -> fits(type.usage(), alreadyReserved, 1))
            .orElse(true);
    }

    private record NodeKey(
        String meshName,
        RoutingId rid,
        long lifecycleGeneration) {
        static NodeKey of(ZLinkMeshNodeDescriptor descriptor) {
            return new NodeKey(
                descriptor.meshName(),
                descriptor.rid(),
                descriptor.lifecycleGeneration());
        }
    }

    private enum ObjectKind { SPOT, ACTOR }

    private record ObjectKey(ObjectKind kind, String objectId) {
    }

    private record Reserved(
        int actors,
        int spots,
        Map<String, Integer> spotTypes) {
        private static final Reserved EMPTY =
            new Reserved(0, 0, Map.of());
    }
}
