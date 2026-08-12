package systems.zlink.e2e.registrymessaging.workflow.Infrastructure;
import java.util.concurrent.TimeUnit;

import java.util.ArrayList;
import java.util.List;
import java.util.function.Predicate;
import systems.zlink.e2e.registrymessaging.shared.Contracts;

public final class ScenarioState {
    private final List<Contracts.EvidenceEntry> entries = new ArrayList<>();
    private final String providerRid;
    private final String instanceId;

    public ScenarioState(String providerRid, String instanceId) {
        this.providerRid = providerRid;
        this.instanceId = instanceId;
    }

    public String providerRid() {
        return providerRid;
    }

    public String instanceId() {
        return instanceId;
    }

    public synchronized void record(String marker, String value) {
        entries.add(new Contracts.EvidenceEntry(marker, providerRid, value));
        notifyAll();
    }

    public synchronized Contracts.EvidenceSnapshot snapshot() {
        return new Contracts.EvidenceSnapshot(providerRid, List.copyOf(entries));
    }

    public synchronized List<String> lines() {
        return entries.stream()
            .map(entry -> entry.marker() + "|rid=" + entry.providerRid() + "|value=" + entry.value())
            .toList();
    }

    public synchronized void clear() {
        entries.clear();
    }

    public synchronized List<String> waitUntil(Predicate<String> predicate, long timeoutMillis) {
        long deadline = System.nanoTime() + TimeUnit.MILLISECONDS.toNanos(timeoutMillis);
        while (true) {
            List<String> current = lines();
            if (current.stream().anyMatch(predicate)) {
                return current;
            }
            long remaining = deadline - System.nanoTime();
            if (remaining <= 0) {
                return current;
            }
            try {
                wait(Math.min(100, TimeUnit.NANOSECONDS.toMillis(remaining)));
            } catch (InterruptedException error) {
                Thread.currentThread().interrupt();
                throw new IllegalStateException("interrupted while waiting for evidence", error);
            }
        }
    }
}
