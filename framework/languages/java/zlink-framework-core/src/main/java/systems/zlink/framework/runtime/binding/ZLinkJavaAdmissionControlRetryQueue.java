package systems.zlink.framework.runtime.binding;

import java.util.ArrayDeque;
import java.util.HashMap;
import java.util.HashSet;
import java.util.List;
import java.util.Map;
import java.util.Objects;
import java.util.function.Function;
import systems.zlink.contracts.core.RoutingId;

/**
 * Bounded retry state for completion-lane admission controls.
 *
 * <p>The queue owns a complete multipart record and only retries it after the
 * socket reports that sending may be possible. A retry is fenced by the
 * physical connection identity so a record created for an old pipe cannot be
 * sent to a replacement pipe with the same routing id.
 */
final class ZLinkJavaAdmissionControlRetryQueue {
    static final int DEFAULT_MAXIMUM_RECORDS = 4_096;
    static final long DEFAULT_MAXIMUM_BYTES = 4L * 1024L * 1024L;

    enum RetryResult {
        ACCEPTED,
        BACKPRESSURED,
        STALE,
        PERMANENT_FAILURE
    }

    private final Object gate = new Object();
    private final int maximumRecords;
    private final long maximumBytes;
    private final Map<RetryKey, PendingState> pending = new HashMap<>();
    private final ArrayDeque<RetryKey> ready = new ArrayDeque<>();
    private long pendingBytes;
    private long version;
    private long capacityRejectionCount;

    ZLinkJavaAdmissionControlRetryQueue() {
        this(DEFAULT_MAXIMUM_RECORDS, DEFAULT_MAXIMUM_BYTES);
    }

    ZLinkJavaAdmissionControlRetryQueue(
        int maximumRecords,
        long maximumBytes) {
        if (maximumRecords <= 0) {
            throw new IllegalArgumentException(
                "maximumRecords must be positive");
        }
        if (maximumBytes <= 0) {
            throw new IllegalArgumentException(
                "maximumBytes must be positive");
        }
        this.maximumRecords = maximumRecords;
        this.maximumBytes = maximumBytes;
    }

    int count() {
        synchronized (gate) {
            return pending.size();
        }
    }

    long bytes() {
        synchronized (gate) {
            return pendingBytes;
        }
    }

    long capacityRejectionCount() {
        synchronized (gate) {
            return capacityRejectionCount;
        }
    }

    long nextIntentVersion() {
        synchronized (gate) {
            version = Math.addExact(version, 1L);
            return version;
        }
    }

    boolean remember(
        RoutingId target,
        String connectionId,
        int command,
        List<byte[]> frames,
        long intentVersion) {
        Objects.requireNonNull(target, "target");
        Objects.requireNonNull(frames, "frames");
        if (frames.isEmpty() || intentVersion <= 0) {
            return false;
        }
        List<byte[]> copy = copyFrames(frames);
        long frameBytes = frameBytes(copy);
        if (frameBytes > maximumBytes) {
            synchronized (gate) {
                capacityRejectionCount = Math.addExact(
                    capacityRejectionCount,
                    1L);
            }
            return false;
        }
        RetryKey key = new RetryKey(
            target,
            normalizeConnectionId(connectionId),
            command);
        synchronized (gate) {
            long actualVersion = Math.max(version, intentVersion);
            version = actualVersion;
            PendingState current = pending.get(key);
            if (current != null) {
                long nextBytes = Math.addExact(
                    pendingBytes - current.bytes(),
                    frameBytes);
                if (nextBytes > maximumBytes) {
                    capacityRejectionCount = Math.addExact(
                        capacityRejectionCount,
                        1L);
                    return false;
                }
                pending.put(
                    key,
                    new PendingState(
                        copy,
                        actualVersion,
                        frameBytes,
                        true));
                pendingBytes = nextBytes;
                if (!current.queued()) {
                    ready.addLast(key);
                }
                return true;
            }
            if (pending.size() >= maximumRecords
                || pendingBytes > maximumBytes - frameBytes) {
                capacityRejectionCount = Math.addExact(
                    capacityRejectionCount,
                    1L);
                return false;
            }
            pending.put(
                key,
                new PendingState(
                    copy,
                    actualVersion,
                    frameBytes,
                    true));
            pendingBytes = Math.addExact(pendingBytes, frameBytes);
            ready.addLast(key);
            return true;
        }
    }

    int flush(Function<Pending, RetryResult> sender) {
        Objects.requireNonNull(sender, "sender");
        int initialCount;
        synchronized (gate) {
            initialCount = pending.size();
        }
        int attempted = 0;
        while (attempted < initialCount) {
            Pending pendingRecord;
            synchronized (gate) {
                pendingRecord = takeNextLocked();
            }
            if (pendingRecord == null) {
                break;
            }
            attempted++;
            RetryResult result = Objects.requireNonNull(
                sender.apply(pendingRecord),
                "sender result");
            synchronized (gate) {
                RetryKey key = new RetryKey(
                    pendingRecord.target(),
                    pendingRecord.connectionId(),
                    pendingRecord.command());
                PendingState current = pending.get(key);
                if (current == null
                    || current.version() != pendingRecord.version()) {
                    continue;
                }
                switch (result) {
                    case ACCEPTED, STALE, PERMANENT_FAILURE ->
                        removeLocked(key);
                    case BACKPRESSURED -> {
                        pending.put(
                            key,
                            new PendingState(
                                current.frames(),
                                current.version(),
                                current.bytes(),
                                true));
                        ready.addLast(key);
                    }
                }
            }
        }
        return attempted;
    }

    void removeTarget(RoutingId target) {
        Objects.requireNonNull(target, "target");
        synchronized (gate) {
            var keys = pending.keySet().stream()
                .filter(key -> key.target().equals(target))
                .toList();
            keys.forEach(this::removeLocked);
            compactReadyLocked();
        }
    }

    void removeTargetConnection(RoutingId target, String connectionId) {
        Objects.requireNonNull(target, "target");
        String normalized = normalizeConnectionId(connectionId);
        synchronized (gate) {
            var keys = pending.keySet().stream()
                .filter(key -> key.target().equals(target)
                    && key.connectionId().equals(normalized))
                .toList();
            keys.forEach(this::removeLocked);
            compactReadyLocked();
        }
    }

    void removeUpTo(
        RoutingId target,
        String connectionId,
        int command,
        long intentVersion) {
        if (target == null || intentVersion <= 0) {
            return;
        }
        RetryKey key = new RetryKey(
            target,
            normalizeConnectionId(connectionId),
            command);
        synchronized (gate) {
            PendingState current = pending.get(key);
            if (current != null && current.version() <= intentVersion) {
                removeLocked(key);
                compactReadyLocked();
            }
        }
    }

    void clear() {
        synchronized (gate) {
            pending.clear();
            ready.clear();
            pendingBytes = 0L;
        }
    }

    private Pending takeNextLocked() {
        RetryKey key;
        while ((key = ready.pollFirst()) != null) {
            PendingState current = pending.get(key);
            if (current == null || !current.queued()) {
                continue;
            }
            PendingState inFlight = new PendingState(
                current.frames(),
                current.version(),
                current.bytes(),
                false);
            pending.put(key, inFlight);
            return new Pending(
                key.target(),
                key.connectionId(),
                key.command(),
                current.frames(),
                current.version());
        }
        return null;
    }

    private void removeLocked(RetryKey key) {
        PendingState removed = pending.remove(key);
        if (removed != null) {
            pendingBytes -= removed.bytes();
        }
    }

    private void compactReadyLocked() {
        if (ready.isEmpty()) {
            return;
        }
        var retained = new HashSet<RetryKey>();
        var entries = ready.stream().toList();
        ready.clear();
        for (RetryKey key : entries) {
            PendingState current = pending.get(key);
            if (current != null
                && current.queued()
                && retained.add(key)) {
                ready.addLast(key);
            }
        }
    }

    private static List<byte[]> copyFrames(List<byte[]> frames) {
        return frames.stream()
            .map(frame -> Objects.requireNonNull(frame, "frame").clone())
            .toList();
    }

    private static long frameBytes(List<byte[]> frames) {
        long total = 0L;
        for (byte[] frame : frames) {
            total = Math.addExact(total, frame.length);
        }
        return total;
    }

    private static String normalizeConnectionId(String connectionId) {
        return connectionId == null ? "" : connectionId;
    }

    record Pending(
        RoutingId target,
        String connectionId,
        int command,
        List<byte[]> frames,
        long version) {
    }

    private record RetryKey(
        RoutingId target,
        String connectionId,
        int command) {
    }

    private record PendingState(
        List<byte[]> frames,
        long version,
        long bytes,
        boolean queued) {
    }
}
