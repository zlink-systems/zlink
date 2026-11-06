package systems.zlink.framework.runtime.internal.locations;

import java.time.Duration;
import java.util.HashMap;
import java.util.Map;
import java.util.Objects;
import java.util.function.LongSupplier;

/** Remembers the first monotonic observation of an invalid owner token for each Location row. */
public final class ZLinkOwnerLeaseWaitTracker {
    private final LongSupplier monotonicNanos;
    private final long waitNanos;
    private final Map<String, Observation> observations = new HashMap<>();

    public ZLinkOwnerLeaseWaitTracker(LongSupplier monotonicNanos, Duration fencingMargin) {
        this.monotonicNanos = Objects.requireNonNull(monotonicNanos, "monotonicNanos");
        Duration margin = Objects.requireNonNull(fencingMargin, "fencingMargin");
        if (margin.isNegative())
            throw new IllegalArgumentException("fencingMargin must be nonnegative");
        this.waitNanos = margin.multipliedBy(3).toNanos();
    }

    /** Returns whether this invalid token is still inside its three-margin wait. */
    public synchronized boolean shouldWait(
            String recordKey, ZLinkLocationOwnerToken owner, boolean invalid) {
        Objects.requireNonNull(recordKey, "recordKey");
        Objects.requireNonNull(owner, "owner");
        if (!invalid) {
            clear(recordKey, owner);
            return false;
        }
        long now = monotonicNanos.getAsLong();
        Observation current = observations.get(recordKey);
        if (current == null || !current.owner().equals(owner)) {
            observations.put(recordKey, new Observation(owner, now));
            return waitNanos > 0;
        }
        return now - current.firstObservedNanos() < waitNanos;
    }

    public synchronized Observation snapshot(String recordKey) {
        return observations.get(Objects.requireNonNull(recordKey, "recordKey"));
    }

    public synchronized void clear(String recordKey, ZLinkLocationOwnerToken owner) {
        Objects.requireNonNull(recordKey, "recordKey");
        Objects.requireNonNull(owner, "owner");
        Observation current = observations.get(recordKey);
        if (current != null && current.owner().equals(owner)) clearIfCurrent(recordKey, current);
    }

    public synchronized void clearIfUnchanged(String recordKey, Observation observed) {
        Objects.requireNonNull(recordKey, "recordKey");
        clearIfCurrent(recordKey, observed);
    }

    private void clearIfCurrent(String recordKey, Observation observed) {
        if (observations.get(recordKey) == observed) observations.remove(recordKey);
    }

    public static String authorityRecordKey(String authorityKey) {
        return "authority:" + Objects.requireNonNull(authorityKey, "authorityKey");
    }

    public static String descriptorRecordKey(String descriptorKey) {
        return "descriptor:" + Objects.requireNonNull(descriptorKey, "descriptorKey");
    }

    public static final class Observation {
        private final ZLinkLocationOwnerToken owner;
        private final long firstObservedNanos;

        private Observation(ZLinkLocationOwnerToken owner, long firstObservedNanos) {
            this.owner = owner;
            this.firstObservedNanos = firstObservedNanos;
        }

        public ZLinkLocationOwnerToken owner() {
            return owner;
        }

        public long firstObservedNanos() {
            return firstObservedNanos;
        }
    }
}
