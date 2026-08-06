package systems.zlink.e2e.automaticturn.shared;

import java.util.ArrayList;
import java.util.List;
import java.time.Duration;

public final class EvidenceStore {
    private final List<Contracts.EvidenceEntry> entries = new ArrayList<>();
    private final String nodeRid;
    private long nextSequence;

    public EvidenceStore(String nodeRid) {
        this.nodeRid = nodeRid;
    }

    public String nodeRid() {
        return nodeRid;
    }

    public synchronized void record(String marker, String subject, String value) {
        entries.add(new Contracts.EvidenceEntry(++nextSequence, marker, subject, value));
        notifyAll();
    }

    public synchronized Contracts.EvidenceSnapshot snapshot() {
        return new Contracts.EvidenceSnapshot(nodeRid, List.copyOf(entries));
    }

    public synchronized boolean awaitMarker(
        String marker,
        String subject,
        Duration timeout) throws InterruptedException {
        long remainingNanos = timeout.toNanos();
        while (!contains(marker, subject)) {
            if (remainingNanos <= 0) {
                return false;
            }
            long waitMillis = Math.max(1L, remainingNanos / 1_000_000L);
            int waitNanos = (int) Math.min(
                999_999L,
                Math.max(0L, remainingNanos - waitMillis * 1_000_000L));
            long started = System.nanoTime();
            wait(waitMillis, waitNanos);
            remainingNanos -= Math.max(1L, System.nanoTime() - started);
        }
        return true;
    }

    private boolean contains(String marker, String subject) {
        for (Contracts.EvidenceEntry entry : entries) {
            if (marker.equals(entry.marker()) && subject.equals(entry.subject())) {
                return true;
            }
        }
        return false;
    }
}
