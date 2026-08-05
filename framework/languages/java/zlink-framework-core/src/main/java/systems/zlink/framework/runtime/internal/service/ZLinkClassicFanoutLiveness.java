package systems.zlink.framework.runtime.internal.service;

import java.time.Duration;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.HashMap;
import java.util.List;
import java.util.Map;
import java.util.Objects;
import systems.zlink.contracts.core.RoutingId;

/**
 * Tracks classic fanout receive activity and the publisher beacon schedule.
 *
 * <p>Each subscriber connection is isolated by publisher and connection ID.
 * A malformed record on the reserved topic removes only that connection.
 */
public final class ZLinkClassicFanoutLiveness {
    public static final Duration DEFAULT_BEACON_INTERVAL =
        Duration.ofSeconds(5);
    public static final Duration DEFAULT_PUBLISHER_TIMEOUT =
        Duration.ofSeconds(15);

    private static final byte[] RESERVED_TOPIC =
        new byte[] {0x01, 0x5a, 0x4c, 0x46, 0x31};
    private static final byte[] BEACON_PAYLOAD =
        new byte[] {0x5a, 0x46, 0x01, 0x01};

    private final long beaconIntervalNanos;
    private final long publisherTimeoutNanos;
    private final Map<RoutingId, PublisherState> publishers = new HashMap<>();
    private long nextBeaconNanos;

    public ZLinkClassicFanoutLiveness() {
        this(System.nanoTime());
    }

    public ZLinkClassicFanoutLiveness(long nowNanos) {
        this(
            DEFAULT_BEACON_INTERVAL,
            DEFAULT_PUBLISHER_TIMEOUT,
            nowNanos);
    }

    ZLinkClassicFanoutLiveness(
        Duration beaconInterval,
        Duration publisherTimeout,
        long nowNanos) {
        Objects.requireNonNull(beaconInterval, "beaconInterval");
        Objects.requireNonNull(publisherTimeout, "publisherTimeout");
        if (beaconInterval.isZero()
            || beaconInterval.isNegative()
            || publisherTimeout.compareTo(beaconInterval) <= 0) {
            throw new IllegalArgumentException(
                "publisher timeout must be larger than a positive beacon interval");
        }
        beaconIntervalNanos = beaconInterval.toNanos();
        publisherTimeoutNanos = publisherTimeout.toNanos();
        nextBeaconNanos = addExact(nowNanos, beaconIntervalNanos);
    }

    public static byte[] reservedTopic() {
        return RESERVED_TOPIC.clone();
    }

    public static List<byte[]> beaconRecord() {
        return List.of(RESERVED_TOPIC.clone(), BEACON_PAYLOAD.clone());
    }

    public static boolean isReservedTopic(byte[] topic) {
        return Arrays.equals(RESERVED_TOPIC, topic);
    }

    public synchronized void connect(
        RoutingId publisherRoutingId,
        String connectionId,
        long nowNanos) {
        requireConnection(publisherRoutingId, connectionId);
        publishers.put(
            publisherRoutingId,
            new PublisherState(
                connectionId,
                false,
                addExact(nowNanos, publisherTimeoutNanos)));
    }

    public synchronized ReceiveKind receive(
        RoutingId publisherRoutingId,
        String connectionId,
        List<byte[]> frames,
        long nowNanos) {
        Objects.requireNonNull(frames, "frames");
        PublisherState current = publishers.get(publisherRoutingId);
        if (current == null || !current.connectionId.equals(connectionId)) {
            return ReceiveKind.STALE_CONNECTION;
        }
        if (frames.isEmpty() || frames.getFirst() == null) {
            throw new IllegalArgumentException(
                "a fanout record requires a topic frame");
        }

        boolean reserved = isReservedTopic(frames.getFirst());
        if (reserved
            && (frames.size() != 2
                || frames.get(1) == null
                || !Arrays.equals(BEACON_PAYLOAD, frames.get(1)))) {
            publishers.remove(publisherRoutingId);
            throw new ZLinkServiceWireException(
                "malformed classic fanout liveness beacon");
        }

        current.ready = true;
        current.deadlineNanos =
            addExact(nowNanos, publisherTimeoutNanos);
        return reserved ? ReceiveKind.BEACON : ReceiveKind.APPLICATION;
    }

    public synchronized boolean disconnect(
        RoutingId publisherRoutingId,
        String connectionId) {
        PublisherState current = publishers.get(publisherRoutingId);
        if (current == null || !current.connectionId.equals(connectionId)) {
            return false;
        }
        publishers.remove(publisherRoutingId);
        return true;
    }

    public synchronized boolean isReady(RoutingId publisherRoutingId) {
        PublisherState current = publishers.get(publisherRoutingId);
        return current != null && current.ready;
    }

    public synchronized boolean beaconDue(long nowNanos) {
        if (nowNanos < nextBeaconNanos) {
            return false;
        }
        nextBeaconNanos = addExact(nowNanos, beaconIntervalNanos);
        return true;
    }

    public synchronized List<RoutingId> expire(long nowNanos) {
        List<RoutingId> expired = new ArrayList<>();
        for (Map.Entry<RoutingId, PublisherState> entry
            : publishers.entrySet()) {
            if (nowNanos >= entry.getValue().deadlineNanos) {
                expired.add(entry.getKey());
            }
        }
        expired.forEach(publishers::remove);
        return List.copyOf(expired);
    }

    public synchronized int size() {
        return publishers.size();
    }

    private static void requireConnection(
        RoutingId publisherRoutingId,
        String connectionId) {
        Objects.requireNonNull(publisherRoutingId, "publisherRoutingId");
        if (connectionId == null || connectionId.isBlank()) {
            throw new IllegalArgumentException("connectionId is required");
        }
    }

    private static long addExact(long left, long right) {
        return Math.addExact(left, right);
    }

    public enum ReceiveKind {
        APPLICATION,
        BEACON,
        STALE_CONNECTION
    }

    private static final class PublisherState {
        private final String connectionId;
        private boolean ready;
        private long deadlineNanos;

        private PublisherState(
            String connectionId,
            boolean ready,
            long deadlineNanos) {
            this.connectionId = connectionId;
            this.ready = ready;
            this.deadlineNanos = deadlineNanos;
        }
    }
}
