package systems.zlink.framework.runtime.internal.service;

import java.util.ArrayList;
import java.util.Comparator;
import java.util.HashMap;
import java.util.HashSet;
import java.util.List;
import java.util.Map;
import java.util.Objects;
import java.util.Optional;
import java.util.Set;
import java.util.function.BiConsumer;
import java.util.function.Predicate;
import systems.zlink.contracts.core.RoutingId;

/**
 * Owns immutable peer snapshots and rejects updates from stale physical
 * connections.
 */
public final class ZLinkServiceTopologyRegistry {
    private static final int PRECOMPUTE_STEP_LIMIT = 100_000;
    private static final long PRECOMPUTE_TIME_LIMIT_NANOS = 10_000_000L;

    private final Map<RoutingId, Peer> peers = new HashMap<>();
    private final Map<RoutingId, String> readyConnections = new HashMap<>();
    private final Map<String, ChannelSelectionPlan> channelPlans = new HashMap<>();
    private final Map<String, ChannelSelectionPlan> readyChannelPlans =
        new HashMap<>();
    private final Map<String, Map<String, Long>> dynamicSelectionCurrents =
        new HashMap<>();
    private final Map<String, Long> placementSelectionCursors = new HashMap<>();
    private ZLinkServiceNodeDescriptor local;

    public ZLinkServiceTopologyRegistry(ZLinkServiceNodeDescriptor local) {
        this.local = Objects.requireNonNull(local, "local");
    }

    private synchronized <T> T inStateLane(
        java.util.function.Supplier<T> work) {
        return work.get();
    }

    public ZLinkServiceNodeDescriptor localDescriptor() {
        return inStateLane(() -> local);
    }

    public void publishLocal(ZLinkServiceNodeDescriptor descriptor) {
        inStateLane(() -> {
            publishLocalOnLane(descriptor);
            return null;
        });
    }

    private void publishLocalOnLane(ZLinkServiceNodeDescriptor descriptor) {
        Objects.requireNonNull(descriptor, "descriptor");
        if (!descriptor.meshName().equals(local.meshName())
            || !descriptor.nodeRoutingId().equals(local.nodeRoutingId())
            || descriptor.lifecycleGeneration() != local.lifecycleGeneration()) {
            throw new IllegalArgumentException("published descriptor changes local identity");
        }
        if (descriptor.descriptorRevision() <= local.descriptorRevision()) {
            throw new IllegalArgumentException(
                "published descriptor revision must increase");
        }
        if (!immutableFieldsMatch(local, descriptor)) {
            throw new IllegalArgumentException(
                "published descriptor changes immutable fields");
        }
        local = descriptor;
    }

    public AdmissionResult admit(
        ZLinkServiceNodeDescriptor descriptor,
        String connectionId) {
        return admit(
            descriptor,
            new Connection(
                connectionId,
                ZLinkServiceAdmissionGuard.ConnectionDirection.OUTBOUND,
                connectionId));
    }

    public AdmissionResult admit(
        ZLinkServiceNodeDescriptor descriptor,
        Connection connection) {
        return inStateLane(() -> admitOnLane(descriptor, connection));
    }

    private AdmissionResult admitOnLane(
        ZLinkServiceNodeDescriptor descriptor,
        Connection connection) {
        String connectionId =
            connection == null ? null : connection.connectionId();
        if (descriptor == null || connectionId == null || connectionId.isBlank()) {
            return AdmissionResult.INVALID_DESCRIPTOR;
        }
        if (!descriptor.meshName().equals(local.meshName())) {
            return AdmissionResult.MESH_MISMATCH;
        }
        if (descriptor.nodeRoutingId().equals(local.nodeRoutingId())) {
            return AdmissionResult.INVALID_DESCRIPTOR;
        }
        Peer current = peers.get(descriptor.nodeRoutingId());
        if (current != null
            && current.connection().connectionId().equals(connectionId)) {
            return admitDescriptorUpdate(current, descriptor);
        }
        if (current != null
            && current.descriptor().lifecycleGeneration()
                == descriptor.lifecycleGeneration()
            && descriptor.descriptorRevision()
                > current.descriptor().descriptorRevision()
            && !immutableFieldsMatch(current.descriptor(), descriptor)) {
            return AdmissionResult.INVALID_DESCRIPTOR;
        }
        if (current != null
            && current.descriptor().lifecycleGeneration()
                == descriptor.lifecycleGeneration()
            && (current.descriptor().descriptorRevision()
                    > descriptor.descriptorRevision()
                || current.descriptor().descriptorRevision()
                        == descriptor.descriptorRevision()
                    && !current.descriptor().equals(descriptor))) {
            return AdmissionResult.STALE_DESCRIPTOR;
        }
        if (current != null
            && current.descriptor().lifecycleGeneration()
                == descriptor.lifecycleGeneration()) {
            var duplicate =
                ZLinkServiceAdmissionGuard.selectConnection(
                    local.nodeRoutingId(),
                    descriptor.nodeRoutingId(),
                    current.descriptor().lifecycleGeneration(),
                    current.connection().direction(),
                    current.connection().discriminator(),
                    descriptor.lifecycleGeneration(),
                    connection.direction(),
                    connection.discriminator());
            if (duplicate
                == ZLinkServiceAdmissionGuard
                    .DuplicateConnectionDecision.KEEP_CURRENT) {
                return AdmissionResult.DUPLICATE_REJECTED;
            }
        }
        peers.put(
            descriptor.nodeRoutingId(),
            new Peer(descriptor, connection));
        readyConnections.remove(descriptor.nodeRoutingId());
        rebuildChannelPlansOnLane();
        return AdmissionResult.ADMITTED;
    }

    private AdmissionResult admitDescriptorUpdate(
        Peer current,
        ZLinkServiceNodeDescriptor descriptor) {
        if (current.descriptor().lifecycleGeneration()
                != descriptor.lifecycleGeneration()
            || current.descriptor().descriptorRevision()
                > descriptor.descriptorRevision()
            || current.descriptor().descriptorRevision()
                    == descriptor.descriptorRevision()
                && !current.descriptor().equals(descriptor)) {
            return AdmissionResult.STALE_DESCRIPTOR;
        }
        if (!immutableFieldsMatch(current.descriptor(), descriptor)) {
            return AdmissionResult.INVALID_DESCRIPTOR;
        }
        peers.put(
            descriptor.nodeRoutingId(),
            new Peer(descriptor, current.connection()));
        rebuildChannelPlansOnLane();
        return AdmissionResult.ADMITTED;
    }

    private static boolean immutableFieldsMatch(
        ZLinkServiceNodeDescriptor current,
        ZLinkServiceNodeDescriptor incoming) {
        if (!current.meshName().equals(incoming.meshName())
            || !current.nodeRoutingId().equals(incoming.nodeRoutingId())
            || current.lifecycleGeneration() != incoming.lifecycleGeneration()
            || !current.advertisedEndpoint().equals(
                incoming.advertisedEndpoint())
            || !current.securityIdentity().equals(incoming.securityIdentity())
            || current.applicationVersion() != incoming.applicationVersion()
            || !current.protocolCapabilities().equals(
                incoming.protocolCapabilities())
            || current.objectRole() != incoming.objectRole()
            || current.activeCapacityLimit() != incoming.activeCapacityLimit()
            || current.pendingCapacityLimit()
                != incoming.pendingCapacityLimit()
            || current.channels().size() != incoming.channels().size()) {
            return false;
        }
        for (int index = 0; index < current.channels().size(); index++) {
            if (!current.channels().get(index).name().equals(
                    incoming.channels().get(index).name())) {
                return false;
            }
        }
        return true;
    }

    public boolean disconnect(
        RoutingId nodeRoutingId,
        String connectionId) {
        return inStateLane(() -> disconnectOnLane(nodeRoutingId, connectionId));
    }

    private boolean disconnectOnLane(
        RoutingId nodeRoutingId,
        String connectionId) {
        Peer current = peers.get(nodeRoutingId);
        if (current == null
            || !current.connection().connectionId().equals(connectionId)) {
            return false;
        }
        peers.remove(nodeRoutingId);
        readyConnections.remove(nodeRoutingId);
        rebuildChannelPlansOnLane();
        return true;
    }

    public Optional<Peer> peer(RoutingId nodeRoutingId) {
        return inStateLane(() -> Optional.ofNullable(peers.get(nodeRoutingId)));
    }

    public List<Peer> peers() {
        return inStateLane(this::peersOnLane);
    }

    private List<Peer> peersOnLane() {
        return peers.values().stream()
            .sorted(Comparator.comparing(
                peer -> peer.descriptor().nodeRoutingId().toString()))
            .toList();
    }

    public Optional<Peer> selectChannel(String channelName) {
        String requiredChannelName = requireChannelName(channelName);
        return inStateLane(() -> selectPreparedOnLane(
            channelPlans, requiredChannelName));
    }

    public Optional<Peer> selectChannel(
        String channelName,
        Predicate<Peer> isReady) {
        String requiredChannelName = requireChannelName(channelName);
        Objects.requireNonNull(isReady, "isReady");
        return inStateLane(() -> selectOnLane(
            "channel:" + requiredChannelName,
            eligibleChannelTargets(
                requiredChannelName, isReady, peersOnLane())));
    }

    /** Selects from the topology/liveness snapshot prepared at change time. */
    public Optional<Peer> selectReadyChannel(String channelName) {
        return selectReadyChannel(channelName, null);
    }

    public Optional<Peer> selectReadyChannel(
        String channelName,
        BiConsumer<String, ChannelSelectionFailure> onUnavailable) {
        String requiredChannelName = requireChannelName(channelName);
        return inStateLane(() -> {
            ChannelSelectionPlan plan = readyChannelPlans.get(requiredChannelName);
            Optional<Peer> selected = plan == null ? Optional.empty() : plan.next();
            if (selected.isEmpty() && onUnavailable != null) {
                onUnavailable.accept(requiredChannelName, plan == null
                    ? ChannelSelectionFailure.NO_MEMBER : plan.unavailableReason);
            }
            return selected;
        });
    }

    public enum ChannelSelectionFailure { NO_MEMBER, NOT_READY, DRAINING }

    /**
     * Projects the authoritative connection readiness into the selector. The
     * projection is rebuilt only when topology or liveness changes; calls read
     * the resulting plan and advance one cursor.
     */
    public boolean setChannelReady(
        RoutingId nodeRoutingId,
        String connectionId,
        boolean ready) {
        Objects.requireNonNull(nodeRoutingId, "nodeRoutingId");
        return inStateLane(() -> setChannelReadyOnLane(
            nodeRoutingId, connectionId, ready));
    }

    private boolean setChannelReadyOnLane(
        RoutingId nodeRoutingId,
        String connectionId,
        boolean ready) {
        Peer peer = peers.get(nodeRoutingId);
        if (peer == null
            || connectionId == null
            || !peer.connectionId().equals(connectionId)) {
            return false;
        }
        boolean changed;
        if (ready) {
            changed = !connectionId.equals(
                readyConnections.put(nodeRoutingId, connectionId));
        } else {
            changed = readyConnections.remove(nodeRoutingId, connectionId);
        }
        if (changed) {
            rebuildChannelPlansOnLane();
        }
        return changed;
    }

    /**
     * Checks RouteMesh channel readiness without advancing the weighted
     * selection cursor. The local descriptor is intentionally not a
     * candidate: RouteMesh selection uses only remote Server memberships
     * published through admitted peer descriptors.
     */
    public boolean hasSelectableChannel(String channelName) {
        String requiredChannelName = requireChannelName(channelName);
        return inStateLane(() -> {
            ChannelSelectionPlan plan = channelPlans.get(requiredChannelName);
            return plan != null && !plan.isEmpty();
        });
    }

    public boolean hasReadyChannel(String channelName) {
        return readyChannelMemberCount(channelName) > 0;
    }

    /** Reads the selector's current candidates without advancing its cursor. */
    public long readyChannelMemberCount(String channelName) {
        String requiredChannelName = requireChannelName(channelName);
        return inStateLane(() -> {
            ChannelSelectionPlan plan = readyChannelPlans.get(
                requiredChannelName);
            return plan == null ? 0L : (long) plan.candidates.size();
        });
    }

    public boolean hasSelectableChannel(
        String channelName,
        Predicate<Peer> isReady) {
        String requiredChannelName = requireChannelName(channelName);
        Objects.requireNonNull(isReady, "isReady");
        return inStateLane(() -> !eligibleChannelTargets(
            requiredChannelName, isReady, peersOnLane()).isEmpty());
    }

    public Optional<Peer> selectPlacement() {
        return selectPlacement(ignored -> true);
    }

    public Optional<Peer> selectPlacement(
        Predicate<Peer> isReady) {
        Objects.requireNonNull(isReady, "isReady");
        List<WeightedPeer> eligible = peers().stream()
            .filter(peer -> peer.descriptor().acceptsPlacement())
            .filter(isReady)
            .map(peer -> new WeightedPeer(
                peer, peer.descriptor().placementWeight()))
            .toList();
        return inStateLane(() -> selectRangeOnLane("placement", eligible));
    }

    private Optional<Peer> selectRangeOnLane(
        String key,
        List<WeightedPeer> eligible) {
        long total = 0;
        for (WeightedPeer candidate : eligible) {
            total = Math.addExact(total, candidate.weight());
        }
        if (total == 0) {
            placementSelectionCursors.remove(key);
            return Optional.empty();
        }
        long cursor = placementSelectionCursors.getOrDefault(key, 0L);
        placementSelectionCursors.put(
            key,
            cursor == Long.MAX_VALUE ? 0L : cursor + 1);
        long selected = Math.floorMod(cursor, total);
        long offset = 0;
        for (WeightedPeer value : eligible) {
            offset = Math.addExact(offset, value.weight());
            if (selected < offset) {
                return Optional.of(value.peer());
            }
        }
        throw new IllegalStateException("weighted placement did not select a peer");
    }

    private Optional<Peer> selectOnLane(String key, List<WeightedPeer> eligible) {
        long total = 0;
        for (WeightedPeer candidate : eligible) {
            total = Math.addExact(total, candidate.weight());
        }
        if (total == 0) {
            dynamicSelectionCurrents.remove(key);
            return Optional.empty();
        }

        List<WeightedPeer> ordered = eligible.stream()
            .sorted(Comparator.comparing(
                value -> value.peer().descriptor().nodeRoutingId().toString()))
            .toList();
        Map<String, Long> currentByNode = dynamicSelectionCurrents.computeIfAbsent(
            key,
            ignored -> new HashMap<>());
        Set<String> eligibleIds = ordered.stream()
            .map(value -> value.peer().descriptor().nodeRoutingId().toString())
            .collect(java.util.stream.Collectors.toSet());
        currentByNode.keySet().removeIf(id -> !eligibleIds.contains(id));

        WeightedPeer selected = null;
        long selectedCurrent = Long.MIN_VALUE;
        for (WeightedPeer candidate : ordered) {
            String identity = candidate.peer().descriptor().nodeRoutingId().toString();
            long current = Math.addExact(
                currentByNode.getOrDefault(identity, 0L),
                candidate.weight());
            currentByNode.put(identity, current);
            if (selected == null
                || current > selectedCurrent
                || current == selectedCurrent
                    && identity.compareTo(
                        selected.peer().descriptor().nodeRoutingId().toString()) < 0) {
                selected = candidate;
                selectedCurrent = current;
            }
        }
        if (selected == null) {
            throw new IllegalStateException("weighted selection did not select a peer");
        }
        String selectedId = selected.peer().descriptor().nodeRoutingId().toString();
        currentByNode.put(
            selectedId,
            Math.subtractExact(currentByNode.get(selectedId), total));
        return Optional.of(selected.peer());
    }

    private Optional<Peer> selectPreparedOnLane(
        Map<String, ChannelSelectionPlan> plans,
        String channelName) {
        ChannelSelectionPlan plan = plans.get(channelName);
        return plan == null ? Optional.empty() : plan.next();
    }

    private void rebuildChannelPlansOnLane() {
        rebuildChannelPlansOnLane(channelPlans, ignored -> true);
        rebuildChannelPlansOnLane(
            readyChannelPlans,
            peer -> peer.connectionId().equals(
                readyConnections.get(peer.descriptor().nodeRoutingId())));
    }

    private void rebuildChannelPlansOnLane(
        Map<String, ChannelSelectionPlan> plans,
        Predicate<Peer> eligiblePeer) {
        Set<String> channelNames = new HashSet<>();
        for (Peer peer : peers.values()) {
            for (ZLinkServiceNodeDescriptor.Channel channel
                : peer.descriptor().channels()) {
                channelNames.add(channel.name());
            }
        }
        plans.keySet().removeIf(channel -> !channelNames.contains(channel));
        List<Peer> snapshot = peersOnLane();
        for (String channelName : channelNames) {
            List<WeightedPeer> eligible = eligibleChannelTargets(
                channelName, eligiblePeer, snapshot);
            ChannelSelectionPlan previous = plans.get(channelName);
            Map<String, Long> currents = previous == null
                ? Map.of()
                : previous.currentSnapshot();
            plans.put(
                channelName,
                eligible.isEmpty()
                    ? unavailableChannelPlan(channelName, snapshot)
                    : ChannelSelectionPlan.prepare(eligible, currents));
        }
    }

    private static ChannelSelectionPlan unavailableChannelPlan(
        String channelName, List<Peer> snapshot) {
        // Prepared with the selector at topology-change time, never on a send.
        for (Peer peer : snapshot) {
            for (ZLinkServiceNodeDescriptor.Channel channel : peer.descriptor().channels()) {
                if (channel.name().equals(channelName)) {
                    switch (peer.descriptor().state()) {
                        case RETIRING, DRAINING, STOPPED -> { }
                        default -> { return ChannelSelectionPlan.EMPTY; }
                    }
                }
            }
        }
        return ChannelSelectionPlan.DRAINING;
    }

    private List<WeightedPeer> eligibleChannelTargets(
        String channelName,
        Predicate<Peer> isReady,
        List<Peer> candidates) {
        String requiredChannelName = requireChannelName(channelName);
        Objects.requireNonNull(isReady, "isReady");
        List<WeightedPeer> eligible = new ArrayList<>();
        for (Peer peer : candidates) {
            if (!isReady.test(peer)) {
                continue;
            }
            for (ZLinkServiceNodeDescriptor.Channel channel
                : peer.descriptor().channels()) {
                if (channel.name().equals(requiredChannelName)
                    && channel.weight() > 0
                    && peer.descriptor().state()
                        == ZLinkServiceNodeDescriptor.State.SERVING) {
                    eligible.add(new WeightedPeer(peer, channel.weight()));
                    break;
                }
            }
        }
        return eligible;
    }

    private static String requireChannelName(String channelName) {
        if (channelName == null || channelName.isBlank()) {
            throw new IllegalArgumentException("channelName is required");
        }
        return channelName;
    }

    public record Peer(
        ZLinkServiceNodeDescriptor descriptor,
        Connection connection) {
        public Peer {
            Objects.requireNonNull(descriptor, "descriptor");
            Objects.requireNonNull(connection, "connection");
        }

        public String connectionId() {
            return connection.connectionId();
        }
    }

    public record Connection(
        String connectionId,
        ZLinkServiceAdmissionGuard.ConnectionDirection direction,
        String discriminator) {
        public Connection {
            Objects.requireNonNull(direction, "direction");
            if (connectionId == null || connectionId.isBlank()) {
                throw new IllegalArgumentException("connectionId is required");
            }
            if (discriminator == null || discriminator.isBlank()) {
                throw new IllegalArgumentException(
                    "connection discriminator is required");
            }
        }
    }

    public enum AdmissionResult {
        ADMITTED,
        MESH_MISMATCH,
        INVALID_DESCRIPTOR,
        STALE_DESCRIPTOR,
        DUPLICATE_REJECTED
    }

    private record WeightedPeer(Peer peer, int weight) {
    }

    private static final class ChannelSelectionPlan {
        private static final ChannelSelectionPlan EMPTY =
            new ChannelSelectionPlan(
                List.of(), new int[0], new long[0][], 0, null,
                ChannelSelectionFailure.NOT_READY);
        private static final ChannelSelectionPlan DRAINING =
            new ChannelSelectionPlan(
                List.of(), new int[0], new long[0][], 0, null,
                ChannelSelectionFailure.DRAINING);

        private final List<WeightedPeer> candidates;
        private final ChannelSelectionFailure unavailableReason;
        private final int[] winners;
        private final long[][] statesBefore;
        private final int cycleStart;
        private final long[] fallbackCurrents;
        private int cursor;

        private ChannelSelectionPlan(
            List<WeightedPeer> candidates,
            int[] winners,
            long[][] statesBefore,
            int cycleStart,
            long[] fallbackCurrents,
            ChannelSelectionFailure unavailableReason) {
            this.candidates = candidates;
            this.unavailableReason = unavailableReason;
            this.winners = winners;
            this.statesBefore = statesBefore;
            this.cycleStart = cycleStart;
            this.fallbackCurrents = fallbackCurrents;
        }

        static ChannelSelectionPlan prepare(
            List<WeightedPeer> eligible,
            Map<String, Long> previousCurrents) {
            if (eligible.isEmpty()) {
                return EMPTY;
            }
            List<WeightedPeer> ordered = eligible.stream()
                .sorted((left, right) -> compareRoutingIds(
                    left.peer().descriptor().nodeRoutingId(),
                    right.peer().descriptor().nodeRoutingId()))
                .toList();
            long[] initial = new long[ordered.size()];
            for (int index = 0; index < ordered.size(); index++) {
                initial[index] = previousCurrents.getOrDefault(
                    identity(ordered.get(index)), 0L);
            }
            long[] currents = initial.clone();
            Map<StateVector, Integer> visited = new HashMap<>();
            List<Integer> preparedWinners = new ArrayList<>();
            List<long[]> preparedStates = new ArrayList<>();
            long startedAt = System.nanoTime();
            for (int step = 0; step < PRECOMPUTE_STEP_LIMIT; step++) {
                StateVector state = new StateVector(currents);
                Integer repeatedAt = visited.putIfAbsent(
                    state, preparedWinners.size());
                if (repeatedAt != null) {
                    return new ChannelSelectionPlan(
                        ordered,
                        preparedWinners.stream().mapToInt(Integer::intValue)
                            .toArray(),
                        preparedStates.toArray(long[][]::new),
                        repeatedAt,
                        null, ChannelSelectionFailure.NOT_READY);
                }
                preparedStates.add(currents.clone());
                preparedWinners.add(selectAndAdvance(ordered, currents));
                if (System.nanoTime() - startedAt
                    >= PRECOMPUTE_TIME_LIMIT_NANOS) {
                    break;
                }
            }
            return new ChannelSelectionPlan(
                ordered, new int[0], new long[0][], 0, initial,
                ChannelSelectionFailure.NOT_READY);
        }

        Optional<Peer> next() {
            if (candidates.isEmpty()) {
                return Optional.empty();
            }
            if (fallbackCurrents != null) {
                return Optional.of(candidates.get(
                    selectAndAdvance(candidates, fallbackCurrents)).peer());
            }
            int selected = winners[cursor];
            cursor++;
            if (cursor == winners.length) {
                cursor = cycleStart;
            }
            return Optional.of(candidates.get(selected).peer());
        }

        boolean isEmpty() {
            return candidates.isEmpty();
        }

        Map<String, Long> currentSnapshot() {
            if (candidates.isEmpty()) {
                return Map.of();
            }
            long[] currents = fallbackCurrents != null
                ? fallbackCurrents
                : statesBefore[cursor];
            Map<String, Long> snapshot = new HashMap<>();
            for (int index = 0; index < candidates.size(); index++) {
                snapshot.put(identity(candidates.get(index)), currents[index]);
            }
            return snapshot;
        }

        private static int selectAndAdvance(
            List<WeightedPeer> candidates,
            long[] currents) {
            long total = 0;
            int selected = -1;
            long selectedCurrent = Long.MIN_VALUE;
            for (int index = 0; index < candidates.size(); index++) {
                WeightedPeer candidate = candidates.get(index);
                total = Math.addExact(total, candidate.weight());
                long current = Math.addExact(
                    currents[index], candidate.weight());
                currents[index] = current;
                if (selected < 0 || current > selectedCurrent) {
                    selected = index;
                    selectedCurrent = current;
                }
            }
            currents[selected] = Math.subtractExact(
                currents[selected], total);
            return selected;
        }

        private static String identity(WeightedPeer peer) {
            return peer.peer().descriptor().nodeRoutingId().toString();
        }
    }

    private static int compareRoutingIds(RoutingId left, RoutingId right) {
        byte[] leftBytes = left.toBytes();
        byte[] rightBytes = right.toBytes();
        int length = Math.min(leftBytes.length, rightBytes.length);
        for (int index = 0; index < length; index++) {
            int compared = Integer.compare(
                Byte.toUnsignedInt(leftBytes[index]),
                Byte.toUnsignedInt(rightBytes[index]));
            if (compared != 0) {
                return compared;
            }
        }
        return Integer.compare(leftBytes.length, rightBytes.length);
    }

    private static final class StateVector {
        private final long[] values;
        private final int hash;

        private StateVector(long[] values) {
            this.values = values.clone();
            hash = java.util.Arrays.hashCode(this.values);
        }

        @Override
        public boolean equals(Object value) {
            return value instanceof StateVector other
                && java.util.Arrays.equals(values, other.values);
        }

        @Override
        public int hashCode() {
            return hash;
        }
    }
}
