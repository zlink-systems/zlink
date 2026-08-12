package systems.zlink.e2e.pubsub.subscriber.Infrastructure;

import java.util.ArrayList;
import java.util.Comparator;
import java.util.List;
import java.util.Set;
import java.util.function.Predicate;
import systems.zlink.e2e.pubsub.shared.Contracts;
import systems.zlink.e2e.pubsub.subscriber.Configuration.SubscriberOptions;

public final class EvidenceStore {
    private final SubscriberOptions options;
    private final List<Contracts.EvidenceEntry> entries = new ArrayList<>();

    public EvidenceStore(SubscriberOptions options) {
        this.options = options;
    }

    public String subscriberRid() {
        return options.rid();
    }

    public boolean accepts(String topic) {
        return options.topics().contains("*") || options.topics().contains(topic);
    }

    public void delayIfConfigured(String scenario) {
        if (options.delay().delayMillis() <= 0 || !"ps-b1".equals(scenario)) {
            return;
        }
        try {
            Thread.sleep(options.delay().delayMillis());
        } catch (InterruptedException error) {
            Thread.currentThread().interrupt();
            throw new IllegalStateException("subscriber delay interrupted", error);
        }
    }

    public synchronized void record(
        String marker,
        String topic,
        String scenario,
        int sequence,
        String value) {
        entries.add(new Contracts.EvidenceEntry(
            marker,
            options.rid(),
            topic,
            scenario,
            sequence,
            value));
        notifyAll();
    }

    public synchronized Contracts.EvidenceSnapshot snapshot() {
        return new Contracts.EvidenceSnapshot(options.rid(), List.copyOf(entries));
    }

    public synchronized Contracts.EvidenceSnapshot waitFor(
        Predicate<Contracts.EvidenceEntry> predicate,
        long timeoutMillis) {
        long deadline = System.nanoTime() + Math.max(timeoutMillis, 0L) * 1_000_000L;
        while (entries.stream().noneMatch(predicate)) {
            long remainingNanos = deadline - System.nanoTime();
            if (remainingNanos <= 0) {
                throw new IllegalStateException("timed out waiting for subscriber evidence");
            }
            try {
                wait(Math.max(1L, remainingNanos / 1_000_000L));
            } catch (InterruptedException error) {
                Thread.currentThread().interrupt();
                throw new IllegalStateException("interrupted while waiting for subscriber evidence", error);
            }
        }
        return snapshot();
    }

    public synchronized Contracts.EvidenceSnapshot waitForContiguousRun(
        String scenario,
        int minLength,
        long timeoutMillis) {
        long deadline = System.nanoTime() + Math.max(timeoutMillis, 0L) * 1_000_000L;
        while (!hasContiguousRun(scenario, minLength)) {
            long remainingNanos = deadline - System.nanoTime();
            if (remainingNanos <= 0) {
                throw new IllegalStateException("timed out waiting for contiguous subscriber evidence");
            }
            try {
                wait(Math.max(1L, remainingNanos / 1_000_000L));
            } catch (InterruptedException error) {
                Thread.currentThread().interrupt();
                throw new IllegalStateException("interrupted while waiting for subscriber evidence", error);
            }
        }
        return snapshot();
    }

    private boolean hasContiguousRun(String scenario, int minLength) {
        List<Integer> sequences = entries.stream()
            .filter(entry -> "Event".equals(entry.marker()))
            .filter(entry -> scenario.equals(entry.scenario()))
            .map(Contracts.EvidenceEntry::sequence)
            .distinct()
            .sorted(Comparator.naturalOrder())
            .toList();
        int run = 0;
        int previous = Integer.MIN_VALUE;
        for (int sequence : sequences) {
            run = sequence == previous + 1 ? run + 1 : 1;
            if (run >= minLength) {
                return true;
            }
            previous = sequence;
        }
        return false;
    }

    public Set<String> topics() {
        return options.topics();
    }
}
