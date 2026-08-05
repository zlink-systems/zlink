package systems.zlink.e2e.kotlin.toactormessaging.shared;

import java.util.ArrayList;
import java.util.List;

public final class EvidenceStore {
    private final List<Contracts.ActorEvidence> entries = new ArrayList<>();

    public synchronized void append(Contracts.ActorEvidence entry) {
        entries.add(entry);
    }

    public synchronized List<Contracts.ActorEvidence> all() {
        return List.copyOf(entries);
    }
}
