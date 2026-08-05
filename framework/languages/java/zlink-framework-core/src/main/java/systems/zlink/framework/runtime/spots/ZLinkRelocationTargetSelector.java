package systems.zlink.framework.runtime.spots;

import java.util.List;
import java.util.concurrent.ThreadLocalRandom;
import java.util.function.Predicate;
import systems.zlink.framework.runtime.host.ZLinkFrameworkRelocationReason;
import systems.zlink.framework.runtime.internal.locations.ZLinkMeshNodeDescriptor;

final class ZLinkRelocationTargetSelector {
    private ZLinkRelocationTargetSelector() {
    }

    static ZLinkMeshNodeDescriptor select(
        List<ZLinkMeshNodeDescriptor> descriptors,
        ZLinkRelocationTargetPolicy policy,
        Predicate<ZLinkMeshNodeDescriptor> base,
        Predicate<ZLinkMeshNodeDescriptor> capability,
        Predicate<ZLinkMeshNodeDescriptor> capacity,
        String unavailableMessage) {
        List<ZLinkMeshNodeDescriptor> candidates =
            descriptors.stream().filter(base).toList();
        requireCandidates(
            candidates,
            unavailableMessage,
            ZLinkFrameworkRelocationReason.TARGET_UNAVAILABLE);
        candidates = candidates.stream()
            .filter(policy::acceptsVersion)
            .toList();
        requireCandidates(
            candidates,
            unavailableMessage,
            ZLinkFrameworkRelocationReason.TARGET_UNAVAILABLE);
        candidates = candidates.stream().filter(capability).toList();
        requireCandidates(
            candidates,
            unavailableMessage,
            ZLinkFrameworkRelocationReason.STATE_INCOMPATIBLE);
        candidates = candidates.stream()
            .filter(policy::acceptsWave)
            .toList();
        requireCandidates(
            candidates,
            unavailableMessage,
            ZLinkFrameworkRelocationReason.TARGET_UNAVAILABLE);
        candidates = candidates.stream().filter(capacity).toList();
        requireCandidates(
            candidates,
            unavailableMessage,
            ZLinkFrameworkRelocationReason.TARGET_UNAVAILABLE);
        candidates = candidates.stream()
            .filter(candidate -> candidate.placementWeight() > 0)
            .toList();
        requireCandidates(
            candidates,
            unavailableMessage,
            ZLinkFrameworkRelocationReason.TARGET_UNAVAILABLE);
        long total = candidates.stream()
            .mapToLong(ZLinkMeshNodeDescriptor::placementWeight)
            .reduce(0L, Math::addExact);
        long selected = ThreadLocalRandom.current().nextLong(total);
        for (ZLinkMeshNodeDescriptor candidate : candidates) {
            selected -= candidate.placementWeight();
            if (selected < 0) {
                return candidate;
            }
        }
        return candidates.getLast();
    }

    private static void requireCandidates(
        List<ZLinkMeshNodeDescriptor> candidates,
        String message,
        ZLinkFrameworkRelocationReason reason) {
        if (candidates.isEmpty()) {
            throw new ZLinkUserSpotRetireRuntime.RelocationBlockedException(
                reason, message);
        }
    }
}
