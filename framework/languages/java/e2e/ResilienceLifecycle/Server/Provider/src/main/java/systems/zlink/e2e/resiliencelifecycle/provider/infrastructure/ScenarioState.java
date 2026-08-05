package systems.zlink.e2e.resiliencelifecycle.provider.infrastructure;

import java.util.ArrayList;
import java.util.List;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import systems.zlink.e2e.resiliencelifecycle.shared.Contracts;

public final class ScenarioState {
    private final List<Contracts.EvidenceEntry> entries = new ArrayList<>();
    private final CountDownLatch slowStarted = new CountDownLatch(1);
    private final CountDownLatch slowRelease = new CountDownLatch(1);
    private final String providerRid;
    private int weight = 100;
    private boolean grayFailure;
    private boolean observerThrows;

    public ScenarioState(String providerRid) {
        this.providerRid = providerRid;
    }

    public String providerRid() {
        return providerRid;
    }

    public synchronized void weight(int value) {
        weight = value;
    }

    public synchronized int weight() {
        return weight;
    }

    public synchronized void grayFailure(boolean value) {
        grayFailure = value;
    }

    public synchronized boolean grayFailure() {
        return grayFailure;
    }

    public synchronized void observerThrows(boolean value) {
        observerThrows = value;
    }

    public synchronized boolean observerThrows() {
        return observerThrows;
    }

    public void releaseSlow() {
        slowRelease.countDown();
    }

    public void awaitSlowRelease() {
        slowStarted.countDown();
        try {
            if (!slowRelease.await(20, TimeUnit.SECONDS)) {
                throw new IllegalStateException("slow request was not released");
            }
        } catch (InterruptedException error) {
            Thread.currentThread().interrupt();
            throw new IllegalStateException("interrupted", error);
        }
    }

    public synchronized void record(String marker, String value) {
        entries.add(new Contracts.EvidenceEntry(marker, providerRid, value));
    }

    public synchronized Contracts.EvidenceSnapshot snapshot() {
        return new Contracts.EvidenceSnapshot(providerRid, weight, List.copyOf(entries));
    }
}
