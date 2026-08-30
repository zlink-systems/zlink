package systems.zlink.framework.runtime.internal.service;

import java.time.Duration;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.List;
import java.util.Map;
import java.util.Objects;
import java.util.Optional;
import java.util.concurrent.CompletionException;
import java.util.function.Supplier;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.runtime.internal.execution.ZLinkStateLane;

/**
 * Tracks probe round trips on a monotonic clock. Application traffic never
 * extends a peer deadline.
 */
public final class ZLinkServiceLivenessRegistry {
    public static final Duration DEFAULT_PROBE_INTERVAL = Duration.ofSeconds(5);
    public static final Duration DEFAULT_PEER_TIMEOUT = Duration.ofSeconds(15);
    //  A connection that has never completed a probe round trip gates every
    //  outbound bound-session send (isReady == false). The very first probe
    //  can be lost while the freshly admitted route is still settling; waiting
    //  a full probe interval to retransmit leaves a multi-second not-ready
    //  window in which one-way sends exhaust their bounded submit deadline.
    //  Retry the outstanding probe quickly until the first ACK arrives.
    static final Duration NOT_READY_PROBE_RETRY = Duration.ofMillis(250);

    private final long probeIntervalNanos;
    private final long notReadyProbeRetryNanos;
    private final long peerTimeoutNanos;
    private final ZLinkStateLane stateLane = new ZLinkStateLane();
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
        notReadyProbeRetryNanos = Math.min(
            probeIntervalNanos, NOT_READY_PROBE_RETRY.toNanos());
        peerTimeoutNanos = peerTimeout.toNanos();
    }

    private <T> T inStateLane(Supplier<T> work) {
        try {
            return stateLane.runAsync(work).toCompletableFuture().join();
        } catch (CompletionException failure) {
            Throwable cause = failure.getCause();
            if (cause instanceof RuntimeException runtimeFailure) {
                throw runtimeFailure;
            }
            if (cause instanceof Error error) {
                throw error;
            }
            throw failure;
        }
    }

    public void admit(
        RoutingId nodeRoutingId,
        String connectionId,
        long nowNanos) {
        inStateLane(() -> {
            admitCore(nodeRoutingId, connectionId, nowNanos);
            return null;
        });
    }

    private void admitCore(
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
                false,
                false,
                false,
                false,
                false));
    }

    /** Requests the first probe immediately after a new connection is admitted. */
    public boolean requestProbe(
        RoutingId nodeRoutingId,
        String connectionId,
        long nowNanos) {
        return inStateLane(() -> requestProbeCore(
            nodeRoutingId, connectionId, nowNanos));
    }

    private boolean requestProbeCore(
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

    /** Schedules prompt pair revalidation after a physical candidate appears. */
    public boolean requestValidationProbe(
        RoutingId nodeRoutingId,
        String connectionId,
        long nowNanos) {
        return inStateLane(() -> requestValidationProbeCore(
            nodeRoutingId, connectionId, nowNanos));
    }

    private boolean requestValidationProbeCore(
        RoutingId nodeRoutingId,
        String connectionId,
        long nowNanos) {
        PeerState state = peers.get(nodeRoutingId);
        if (state == null
            || !state.connectionId.equals(connectionId)
            || state.validationPending) {
            return false;
        }
        state.validationPending = true;
        state.ready = false;
        if (state.outstandingProbe == 0) {
            state.nextProbeNanos = Math.min(
                state.nextProbeNanos, nowNanos);
        }
        return true;
    }

    public boolean disconnect(
        RoutingId nodeRoutingId,
        String connectionId) {
        return inStateLane(() -> disconnectCore(nodeRoutingId, connectionId));
    }

    private boolean disconnectCore(
        RoutingId nodeRoutingId,
        String connectionId) {
        PeerState current = peers.get(nodeRoutingId);
        if (current == null || !current.connectionId.equals(connectionId)) {
            return false;
        }
        peers.remove(nodeRoutingId);
        return true;
    }

    public Optional<Probe> acknowledgeProbe(
        RoutingId nodeRoutingId,
        String connectionId,
        long probeId) {
        return inStateLane(() -> acknowledgeProbeCore(
            nodeRoutingId, connectionId, probeId));
    }

    private Optional<Probe> acknowledgeProbeCore(
        RoutingId nodeRoutingId,
        String connectionId,
        long probeId) {
        PeerState state = peers.get(nodeRoutingId);
        if (state == null
            || !state.connectionId.equals(connectionId)
            || probeId == 0) {
            return Optional.empty();
        }
        return Optional.of(new Probe(
            nodeRoutingId, connectionId, probeId, false));
    }

    public boolean acknowledge(
        RoutingId nodeRoutingId,
        String connectionId,
        long probeId,
        long nowNanos) {
        return inStateLane(() -> acknowledgeCore(
            nodeRoutingId, connectionId, probeId, nowNanos));
    }

    private boolean acknowledgeCore(
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
        boolean selectedPair = state.outstandingSelectedPair;
        boolean validation = state.outstandingValidation;
        state.outstandingProbe = 0;
        state.outstandingSelectedPair = false;
        state.outstandingValidation = false;
        if (!selectedPair) {
            state.bootstrapComplete = true;
        }
        if (state.validationPending) {
            // This probe began before the replacement signal (or was the RID
            // bootstrap), so its ACK cannot satisfy the requested pair check.
            // Do not let it extend the deadline; issue a fresh exact probe.
            state.nextProbeNanos = nowNanos;
            state.ready = false;
        } else {
            state.deadlineNanos = addExact(nowNanos, peerTimeoutNanos);
            state.nextProbeNanos = addExact(nowNanos, probeIntervalNanos);
            state.ready = !selectedPair || validation || state.ready;
        }
        return true;
    }

    public boolean isReady(
        RoutingId nodeRoutingId,
        String connectionId) {
        return inStateLane(() -> isReadyCore(nodeRoutingId, connectionId));
    }

    private boolean isReadyCore(
        RoutingId nodeRoutingId,
        String connectionId) {
        PeerState state = peers.get(nodeRoutingId);
        return state != null
            && state.connectionId.equals(connectionId)
            && state.ready;
    }

    public Tick tick(long nowNanos) {
        return inStateLane(() -> tickCore(nowNanos));
    }

    private Tick tickCore(long nowNanos) {
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
                state.outstandingSelectedPair = state.bootstrapComplete;
                state.outstandingValidation =
                    state.validationPending
                        && state.outstandingSelectedPair;
                if (state.outstandingValidation) {
                    state.validationPending = false;
                }
            }
            state.nextProbeNanos = addExact(
                nowNanos,
                state.ready ? probeIntervalNanos : notReadyProbeRetryNanos);
            probes.add(new Probe(
                entry.getKey(),
                state.connectionId,
                state.outstandingProbe,
                state.outstandingSelectedPair));
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
        long probeId,
        boolean selectedPair) {
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
        private boolean outstandingSelectedPair;
        private boolean outstandingValidation;
        private boolean bootstrapComplete;
        private boolean ready;
        private boolean validationPending;

        private PeerState(
            String connectionId,
            long deadlineNanos,
            long nextProbeNanos,
            long outstandingProbe,
            boolean outstandingSelectedPair,
            boolean outstandingValidation,
            boolean bootstrapComplete,
            boolean ready,
            boolean validationPending) {
            this.connectionId = connectionId;
            this.deadlineNanos = deadlineNanos;
            this.nextProbeNanos = nextProbeNanos;
            this.outstandingProbe = outstandingProbe;
            this.outstandingSelectedPair = outstandingSelectedPair;
            this.outstandingValidation = outstandingValidation;
            this.bootstrapComplete = bootstrapComplete;
            this.ready = ready;
            this.validationPending = validationPending;
        }
    }
}
