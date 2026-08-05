package systems.zlink.e2e.kotlin.automaticturn;

import java.util.ArrayList;
import java.util.List;

public final class PlayEvidenceStore {
    private final List<EvidenceEntry> entries = new ArrayList<>();

    public synchronized void record(
        String requestId,
        String marker,
        String value) {
        entries.add(new EvidenceEntry(requestId, marker, value));
    }

    public synchronized Contracts.EvidenceRes evidence(String requestId) {
        List<String> markers = entries.stream()
            .filter(entry -> entry.requestId().equals(requestId))
            .map(entry -> entry.marker() + "|" + entry.value())
            .toList();
        return new Contracts.EvidenceRes(requestId, markers);
    }

    private record EvidenceEntry(String requestId, String marker, String value) {
    }
}
