package systems.zlink.e2e.pubsub.publisher.Infrastructure;

import java.util.ArrayList;
import java.util.List;

public final class EvidenceStore {
    private final List<String> entries = new ArrayList<>();

    public synchronized void record(String entry) {
        entries.add(entry);
    }

    public synchronized List<String> snapshot() {
        return List.copyOf(entries);
    }
}
