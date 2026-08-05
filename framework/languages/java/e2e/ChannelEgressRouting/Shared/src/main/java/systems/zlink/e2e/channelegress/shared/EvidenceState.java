package systems.zlink.e2e.channelegress.shared;

import java.util.ArrayList;
import java.util.List;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;

public final class EvidenceState {
    private final String role;
    private final String rid;
    private final List<Contracts.EvidenceEntry> entries = new ArrayList<>();
    private final CompletableFuture<Void> release = new CompletableFuture<>();
    private boolean held;

    public EvidenceState(String role, String rid) {
        this.role = role;
        this.rid = rid;
    }

    public String role() {
        return role;
    }

    public String rid() {
        return rid;
    }

    public synchronized void add(String marker, String value) {
        entries.add(new Contracts.EvidenceEntry(marker, role, rid, value));
    }

    public synchronized Contracts.EvidenceSnapshot snapshot() {
        return new Contracts.EvidenceSnapshot(role, rid, entries);
    }

    public synchronized void hold() {
        held = true;
        add("request-held", "role=" + role);
    }

    public synchronized boolean isHeld() {
        return held;
    }

    public void awaitRelease() {
        awaitReleaseAsync().toCompletableFuture().join();
    }

    public CompletionStage<Void> awaitReleaseAsync() {
        return release.copy().orTimeout(20, java.util.concurrent.TimeUnit.SECONDS);
    }

    public void release() {
        release.complete(null);
        add("request-released", "role=" + role);
    }

    public synchronized boolean contains(String fragment) {
        return entries.stream().anyMatch(entry -> evidenceLine(entry).contains(fragment));
    }

    private static String evidenceLine(Contracts.EvidenceEntry entry) {
        return entry.marker() + "|role=" + entry.role() + "|rid=" + entry.rid()
            + "|" + entry.value();
    }
}
