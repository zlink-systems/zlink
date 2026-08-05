package systems.zlink.e2e.spotservice.shared;

import java.util.ArrayList;
import java.util.List;

public final class ScenarioState {
    private final String nodeRid;
    private final List<Contracts.EvidenceEntry> entries = new ArrayList<>();

    public ScenarioState(String nodeRid) {
        this.nodeRid = nodeRid;
    }

    public String nodeRid() {
        return nodeRid;
    }

    public synchronized void record(String marker, String spotRid, String value) {
        entries.add(new Contracts.EvidenceEntry(marker, nodeRid, spotRid, value));
    }

    public synchronized Contracts.EvidenceSnapshot snapshot() {
        return new Contracts.EvidenceSnapshot(nodeRid, List.copyOf(entries));
    }

    public Contracts.EvidenceSnapshot waitFor(
        List<String> fragments,
        int timeoutMilliseconds) {
        long deadline = System.nanoTime() + timeoutMilliseconds * 1_000_000L;
        while (System.nanoTime() < deadline) {
            Contracts.EvidenceSnapshot current = snapshot();
            if (containsAll(current, fragments)) {
                return current;
            }
            try {
                Thread.sleep(50);
            } catch (InterruptedException error) {
                Thread.currentThread().interrupt();
                throw new IllegalStateException("interrupted while waiting for evidence", error);
            }
        }
        Contracts.EvidenceSnapshot current = snapshot();
        if (containsAll(current, fragments)) {
            return current;
        }
        throw new IllegalStateException(
            "timed out waiting for evidence fragments: "
                + fragments
                + " current="
                + evidenceLines(current));
    }

    private static boolean containsAll(
        Contracts.EvidenceSnapshot snapshot,
        List<String> fragments) {
        List<String> lines = snapshot.entries().stream()
            .<String>map(entry -> entry.marker()
                + "|" + entry.nodeRid()
                + "|" + entry.spotRid()
                + "|" + entry.value())
            .toList();
        return fragments.stream().allMatch(fragment ->
            lines.stream().anyMatch(line -> line.contains(fragment)));
    }

    private static List<String> evidenceLines(Contracts.EvidenceSnapshot snapshot) {
        return snapshot.entries().stream()
            .<String>map(entry -> entry.marker()
                + "|"
                + entry.nodeRid()
                + "|"
                + entry.spotRid()
                + "|"
                + entry.value())
            .toList();
    }
}
