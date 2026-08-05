package systems.zlink.framework.runtime.internal.service;

import java.time.Duration;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.List;
import java.util.Map;
import java.util.Objects;
import java.util.Optional;
import systems.zlink.contracts.core.RoutingId;

/**
 * Tracks probe round trips on a monotonic clock. Application traffic never
 * extends a peer deadline.
 */
public final class ZLinkServiceLivenessRegistry {
    public static final Duration DEFAULT_PROBE_INTERVAL = Duration.ofSeconds(5);
    public static final Duration DEFAULT_PEER_TIMEOUT = Duration.ofSeconds(15);

    private final long probeIntervalNanos;
    private final long peerTimeoutNanos;
    private final Map<RoutingId, PeerState> peers = new HashMap<>();
    private long nextProbeId = 1;

    public ZLinkServiceLivenessRegistry() {
        this(DEFAULT_PROBE_INTERVAL, DEFAULT_PEER_TIMEOUT);
    }

    public ZLinkServiceLivenessRegistry(
        Duration probeInterval,
        Duration peerTimeout) {
        Objects.requireNonNull(probeInterval, "probeInterval");
        Objects.requireNonNull(peerTimeout, "peerTimeout");
        if (probeInterval.isZero() || probeInterval.isNegative()
            || peerTimeout.compareTo(probeInterval) <= 0) {
            throw new IllegalArgumentException(
                "peer timeout must be larger than a positive probe interval");
        }
        probeIntervalNanos = probeInterval.toNanos();
        peerTimeoutNanos = peerTimeout.toNanos();
    }

    public synchronized void admit(
        RoutingId nodeRoutingId,
        String connectionId,
        long nowNanos) {
        requireConnection(nodeRoutingId, connectionId);
        PeerState current = peers.get(nodeRoutingId);
        if (current != null && current.connectionId.equals(connectionId)) {
            return;
        }
        peers.put(
            nodeRoutingId,
            new PeerState(
                connectionId,
                addExact(nowNanos, peerTimeoutNanos),
                addExact(nowNanos, probeIntervalNanos),
                0,
                false));
    }

    /** Requests the first probe immediately after a new connection is admitted. */
    public synchronized boolean requestProbe(
        RoutingId nodeRoutingId,
        String connectionId,
        long nowNanos) {
        PeerState state = peers.get(nodeRoutingId);
        if (state == null
            || !state.connectionId.equals(connectionId)
            || state.ready
            || state.outstandingProbe != 0
            || state.nextProbeNanos <= nowNanos) {
            return false;
        }
        state.nextProbeNanos = Math.min(state.nextProbeNanos, nowNanos);
        return true;
    }

    public synchronized boolean disconnect(
        RoutingId nodeRoutingId,
        String connectionId) {
        PeerState current = peers.get(nodeRoutingId);
        if (current == null || !current.connectionId.equals(connectionId)) {
            return false;
        }
        peers.remove(nodeRoutingId);
        return true;
    }

    public synchronized Optional<Probe> acknowledgeProbe(
        RoutingId nodeRoutingId,
        String connectionId,
        long probeId) {
        PeerState state = peers.get(nodeRoutingId);
        if (state == null
            || !state.connectionId.equals(connectionId)
            || probeId == 0) {
            return Optional.empty();
        }
        return Optional.of(new Probe(nodeRoutingId, connectionId, probeId));
    }

    public synchronized boolean acknowledge(
        RoutingId nodeRoutingId,
        String connectionId,
        long probeId,
        long nowNanos) {
        PeerState state = peers.get(nodeRoutingId);
        if (state == null
            || !state.connectionId.equals(connectionId)
            || state.outstandingProbe != probeId
            || probeId == 0) {
            return false;
        }
        state.outstandingProbe = 0;
        state.deadlineNanos = addExact(nowNanos, peerTimeoutNanos);
        state.nextProbeNanos = addExact(nowNanos, probeIntervalNanos);
        state.ready = true;
        return true;
    }

    public synchronized boolean isReady(
        RoutingId nodeRoutingId,
        String connectionId) {
        PeerState state = peers.get(nodeRoutingId);
        return state != null
            && state.connectionId.equals(connectionId)
            && state.ready;
    }

    public synchronized Tick tick(long nowNanos) {
        List<Probe> probes = new ArrayList<>();
        List<RoutingId> timedOut = new ArrayList<>();
        for (Map.Entry<RoutingId, PeerState> entry : peers.entrySet()) {
            PeerState state = entry.getValue();
            if (nowNanos >= state.deadlineNanos) {
                timedOut.add(entry.getKey());
                continue;
            }
            if (nowNanos < state.nextProbeNanos) {
                continue;
            }
            if (state.outstandingProbe == 0) {
                state.outstandingProbe = allocateProbeId();
            }
            state.nextProbeNanos = addExact(nowNanos, probeIntervalNanos);
            probes.add(new Probe(
                entry.getKey(), state.connectionId, state.outstandingProbe));
        }
        timedOut.forEach(peers::remove);
        return new Tick(List.copyOf(probes), List.copyOf(timedOut));
    }

    public synchronized int size() {
        return peers.size();
    }

    private long allocateProbeId() {
        if (nextProbeId <= 0) {
            throw new IllegalStateException("liveness probe id is exhausted");
        }
        return nextProbeId++;
    }

    private static void requireConnection(
        RoutingId nodeRoutingId,
        String connectionId) {
        Objects.requireNonNull(nodeRoutingId, "nodeRoutingId");
        if (connectionId == null || connectionId.isBlank()) {
            throw new IllegalArgumentException("connectionId is required");
        }
    }

    private static long addExact(long left, long right) {
        return Math.addExact(left, right);
    }

    public record Probe(
        RoutingId nodeRoutingId,
        String connectionId,
        long probeId) {
    }

    public record Tick(
        List<Probe> probes,
        List<RoutingId> timedOutNodes) {
    }

    private static final class PeerState {
        private final String connectionId;
        private long deadlineNanos;
        private long nextProbeNanos;
        private long outstandingProbe;
        private boolean ready;

        private PeerState(
            String connectionId,
            long deadlineNanos,
            long nextProbeNanos,
            long outstandingProbe,
            boolean ready) {
            this.connectionId = connectionId;
            this.deadlineNanos = deadlineNanos;
            this.nextProbeNanos = nextProbeNanos;
            this.outstandingProbe = outstandingProbe;
            this.ready = ready;
        }
    }
}
