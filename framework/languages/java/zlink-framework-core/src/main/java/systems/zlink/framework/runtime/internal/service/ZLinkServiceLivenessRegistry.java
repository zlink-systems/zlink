package systems.zlink.framework.runtime.internal.service;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.runtime.internal.ZLinkCompletionBridge;
import systems.zlink.framework.runtime.internal.execution.ZLinkStateLane;

import java.time.Duration;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.List;
import java.util.Map;
import java.util.Objects;
import java.util.Optional;
import java.util.concurrent.atomic.AtomicLong;
import java.util.function.LongSupplier;
import java.util.function.Supplier;

/** Tracks received records and independent periodic probes on a monotonic clock. */
public final class ZLinkServiceLivenessRegistry {
    public static final Duration DEFAULT_PROBE_INTERVAL = Duration.ofSeconds(5);
    public static final Duration DEFAULT_PEER_TIMEOUT = Duration.ofSeconds(15);
    private final long probeIntervalNanos;
    private final long peerTimeoutNanos;
    private final ZLinkStateLane stateLane = new ZLinkStateLane();
    private final Map<RoutingId, PeerState> peers = new HashMap<>();
    private long nextProbeId = 1;

    public ZLinkServiceLivenessRegistry() {
        this(DEFAULT_PROBE_INTERVAL, DEFAULT_PEER_TIMEOUT);
    }

    public ZLinkServiceLivenessRegistry(Duration probeInterval, Duration peerTimeout) {
        Objects.requireNonNull(probeInterval, "probeInterval");
        Objects.requireNonNull(peerTimeout, "peerTimeout");
        if (probeInterval.isZero()
                || probeInterval.isNegative()
                || peerTimeout.compareTo(probeInterval) <= 0) {
            throw new IllegalArgumentException(
                    "peer timeout must be larger than a positive probe interval");
        }
        probeIntervalNanos = probeInterval.toNanos();
        peerTimeoutNanos = peerTimeout.toNanos();
    }

    private <T> T inStateLane(Supplier<T> work) {
        return ZLinkCompletionBridge.await(stateLane.runNowOrQueue(work));
    }

    public PeerState admit(RoutingId nodeRoutingId, String connectionId, long nowNanos) {
        return inStateLane(() -> admitCore(nodeRoutingId, connectionId, nowNanos));
    }

    private PeerState admitCore(RoutingId nodeRoutingId, String connectionId, long nowNanos) {
        requireConnection(nodeRoutingId, connectionId);
        PeerState current = peers.get(nodeRoutingId);
        if (current != null && current.connectionId.equals(connectionId)) {
            return current;
        }
        PeerState admitted =
                new PeerState(connectionId, nowNanos, probeIntervalNanos, peerTimeoutNanos);
        peers.put(nodeRoutingId, admitted);
        return admitted;
    }

    public PeerState connection(RoutingId nodeRoutingId) {
        return inStateLane(() -> peers.get(nodeRoutingId));
    }

    public boolean disconnect(RoutingId nodeRoutingId, String connectionId) {
        return inStateLane(() -> disconnectCore(nodeRoutingId, connectionId));
    }

    private boolean disconnectCore(RoutingId nodeRoutingId, String connectionId) {
        PeerState current = peers.get(nodeRoutingId);
        if (current == null || !current.connectionId.equals(connectionId)) {
            return false;
        }
        peers.remove(nodeRoutingId);
        return true;
    }

    public Optional<Probe> acknowledgeProbe(
            RoutingId nodeRoutingId, String connectionId, long probeId) {
        return inStateLane(() -> acknowledgeProbeCore(nodeRoutingId, connectionId, probeId));
    }

    private Optional<Probe> acknowledgeProbeCore(
            RoutingId nodeRoutingId, String connectionId, long probeId) {
        PeerState state = peers.get(nodeRoutingId);
        if (state == null || !state.connectionId.equals(connectionId) || probeId == 0) {
            return Optional.empty();
        }
        return Optional.of(new Probe(nodeRoutingId, connectionId, probeId));
    }

    public boolean acknowledge(
            RoutingId nodeRoutingId, String connectionId, long probeId, long nowNanos) {
        return inStateLane(() -> acknowledgeCore(nodeRoutingId, connectionId, probeId, nowNanos));
    }

    private boolean acknowledgeCore(
            RoutingId nodeRoutingId, String connectionId, long probeId, long nowNanos) {
        PeerState state = peers.get(nodeRoutingId);
        if (state == null || !state.connectionId.equals(connectionId)) return false;
        return state.acknowledge(probeId, nowNanos);
    }

    public Tick tick(long nowNanos) {
        return inStateLane(() -> tickCore(nowNanos));
    }

    private Tick tickCore(long nowNanos) {
        List<Probe> probes = new ArrayList<>();
        List<RoutingId> timedOut = new ArrayList<>();
        for (Map.Entry<RoutingId, PeerState> entry : peers.entrySet()) {
            PeerState state = entry.getValue();
            if (state.isExpired(nowNanos)) {
                timedOut.add(entry.getKey());
                continue;
            }
            long probeId = state.tryGetProbe(nowNanos, this::allocateProbeId);
            if (probeId != 0) probes.add(new Probe(entry.getKey(), state.connectionId, probeId));
        }
        timedOut.forEach(peers::remove);
        return new Tick(List.copyOf(probes), List.copyOf(timedOut));
    }

    public int size() {
        return inStateLane(this::sizeCore);
    }

    private int sizeCore() {
        return peers.size();
    }

    private long allocateProbeId() {
        if (nextProbeId <= 0) {
            throw new IllegalStateException("liveness probe id is exhausted");
        }
        return nextProbeId++;
    }

    private static void requireConnection(RoutingId nodeRoutingId, String connectionId) {
        Objects.requireNonNull(nodeRoutingId, "nodeRoutingId");
        if (connectionId == null || connectionId.isBlank()) {
            throw new IllegalArgumentException("connectionId is required");
        }
    }

    private static long addExact(long left, long right) {
        return Math.addExact(left, right);
    }

    public record Probe(RoutingId nodeRoutingId, String connectionId, long probeId) {}

    public record Tick(List<Probe> probes, List<RoutingId> timedOutNodes) {}

    public static final class PeerState implements Runnable {
        private final String connectionId;
        private final AtomicLong deadlineNanos;
        private final long probeIntervalNanos;
        private final long peerTimeoutNanos;
        private long nextProbeNanos;
        private long outstandingProbe;

        public PeerState(String connectionId, long admittedNanos) {
            this(
                    connectionId,
                    admittedNanos,
                    DEFAULT_PROBE_INTERVAL.toNanos(),
                    DEFAULT_PEER_TIMEOUT.toNanos());
        }

        private PeerState(
                String connectionId,
                long admittedNanos,
                long probeIntervalNanos,
                long peerTimeoutNanos) {
            this.connectionId = connectionId;
            this.probeIntervalNanos = probeIntervalNanos;
            this.peerTimeoutNanos = peerTimeoutNanos;
            this.deadlineNanos = new AtomicLong(addExact(admittedNanos, peerTimeoutNanos));
            this.nextProbeNanos = admittedNanos;
        }

        public void recordReceived(long nowNanos) {
            deadlineNanos.accumulateAndGet(addExact(nowNanos, peerTimeoutNanos), Math::max);
        }

        @Override
        public void run() {
            recordReceived(System.nanoTime());
        }

        public boolean isExpired(long nowNanos) {
            return nowNanos >= deadlineNanos.get();
        }

        public long tryGetProbe(long nowNanos, LongSupplier allocateProbeId) {
            if (nowNanos < nextProbeNanos) return 0;
            if (outstandingProbe == 0) outstandingProbe = allocateProbeId.getAsLong();
            nextProbeNanos = addExact(nowNanos, probeIntervalNanos);
            return outstandingProbe;
        }

        public boolean acknowledge(long probeId, long nowNanos) {
            recordReceived(nowNanos);
            if (probeId == 0 || outstandingProbe != probeId) return false;
            outstandingProbe = 0;
            return true;
        }
    }
}
