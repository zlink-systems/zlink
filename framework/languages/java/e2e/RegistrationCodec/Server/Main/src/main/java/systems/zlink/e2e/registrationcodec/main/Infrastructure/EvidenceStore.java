package Infrastructure;

import systems.zlink.e2e.registrationcodec.main.Infrastructure;
import java.util.ArrayList;
import java.util.List;
import java.util.concurrent.atomic.AtomicInteger;
import systems.zlink.e2e.registrationcodec.shared.Contracts;

public final class EvidenceStore {
    private final List<Contracts.EvidenceEntry> entries = new ArrayList<>();
    private final AtomicInteger diDisposeCount = new AtomicInteger();

    public synchronized void record(String marker, String packetName, String value) {
        entries.add(new Contracts.EvidenceEntry(marker, packetName, value));
    }

    public synchronized Contracts.EvidenceSnapshot snapshot() {
        return new Contracts.EvidenceSnapshot(List.copyOf(entries));
    }

    public int incrementDiDisposeCount() {
        return diDisposeCount.incrementAndGet();
    }

    public int diDisposeCount() {
        return diDisposeCount.get();
    }
}
