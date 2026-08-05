package systems.zlink.e2e.automaticturn.shared;

import java.util.ArrayList;
import java.util.List;

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
    }

    public synchronized Contracts.EvidenceSnapshot snapshot() {
        return new Contracts.EvidenceSnapshot(nodeRid, List.copyOf(entries));
    }
}
