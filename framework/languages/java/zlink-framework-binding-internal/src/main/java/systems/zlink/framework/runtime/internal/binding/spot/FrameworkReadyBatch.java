package systems.zlink.framework.runtime.internal.binding.spot;

import java.util.ArrayList;
import java.util.List;

/** Framework-owned reusable ready-owner storage. */
final class FrameworkReadyBatch implements ReadyBatch {
    private final int capacity;
    private final List<Entry> entries = new ArrayList<>();
    private boolean closed;

    FrameworkReadyBatch(int capacity) {
        if (capacity <= 0) {
            throw new IllegalArgumentException(
                "ready batch capacity must be positive");
        }
        this.capacity = capacity;
    }

    @Override
    public int count() {
        ensureOpen();
        return entries.size();
    }

    @Override
    public ReadyRecord at(int index) {
        ensureOpen();
        return entries.get(index).record();
    }

    @Override
    public void reset() {
        ensureOpen();
        entries.clear();
    }

    @Override
    public Claim takeClaim(int index) {
        ensureOpen();
        Entry entry = entries.get(index);
        if (entry.claim() == null) {
            throw new IllegalStateException("ready claim was already taken");
        }
        entries.set(index, new Entry(entry.record(), null));
        return entry.claim();
    }

    @Override
    public void close() {
        closed = true;
        entries.stream()
            .map(Entry::claim)
            .filter(java.util.Objects::nonNull)
            .forEach(Claim::close);
        entries.clear();
    }

    boolean tryAdd(ReadyRecord record, Claim claim) {
        ensureOpen();
        if (entries.size() == capacity) {
            return false;
        }
        entries.add(new Entry(record, claim));
        return true;
    }

    private void ensureOpen() {
        if (closed) {
            throw new IllegalStateException("ready batch is closed");
        }
    }

    private record Entry(ReadyRecord record, Claim claim) {
    }
}
