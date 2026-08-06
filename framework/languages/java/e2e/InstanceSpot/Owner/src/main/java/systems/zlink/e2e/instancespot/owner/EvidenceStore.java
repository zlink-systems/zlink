package systems.zlink.e2e.instancespot.owner;

import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.time.Duration;
import java.util.ArrayList;
import java.util.List;
import java.util.concurrent.atomic.AtomicLong;
import systems.zlink.e2e.instancespot.shared.Contracts;
import systems.zlink.e2e.instancespot.shared.Wait;

public final class EvidenceStore {
    private final OwnerOptions options;
    private final AtomicLong nextSequence = new AtomicLong();
    private final List<Contracts.EvidenceEvent> events = new ArrayList<>();

    public EvidenceStore(OwnerOptions options) {
        this.options = options;
    }

    public String rid() {
        return options.rid();
    }

    public String lifecycleId() {
        return options.lifecycleId();
    }

    public synchronized Contracts.EvidenceEvent record(
        String kind,
        String spotId,
        String operationId,
        String payload,
        long generation,
        int activeHandlers,
        String detail) {
        Contracts.EvidenceEvent event = new Contracts.EvidenceEvent(
            nextSequence.incrementAndGet(),
            kind,
            spotId == null ? "" : spotId,
            operationId == null ? "" : operationId,
            payload == null ? "" : payload,
            options.rid(),
            options.lifecycleId(),
            generation,
            activeHandlers,
            detail == null ? "" : detail);
        events.add(event);
        append(event);
        return event;
    }

    public synchronized Contracts.EvidenceSnapshot snapshot() {
        return new Contracts.EvidenceSnapshot(
            options.rid(), options.lifecycleId(), List.copyOf(events));
    }

    public Contracts.EvidenceWaitResult waitFor(
        Contracts.EvidenceWaitRequest request) {
        Contracts.EvidenceSnapshot result = Wait.until(
            Duration.ofMillis(request.timeoutMilliseconds()),
            "timed out waiting for Instance Spot evidence kind=" + request.kind()
                + " operation=" + request.operationId(),
            () -> {
                Contracts.EvidenceSnapshot current = snapshot();
                boolean found = current.events().stream().anyMatch(event ->
                    (request.kind().isBlank() || event.kind().equals(request.kind()))
                        && (request.operationId().isBlank()
                            || event.operationId().equals(request.operationId())));
                return found ? current : null;
            });
        return new Contracts.EvidenceWaitResult(true, result);
    }

    private void append(Contracts.EvidenceEvent event) {
        if (options.evidenceFile().isBlank()) {
            return;
        }
        try {
            Path path = Path.of(options.evidenceFile());
            Path parent = path.getParent();
            if (parent != null) {
                Files.createDirectories(parent);
            }
            Files.writeString(
                path,
                event.sequence() + "|" + event.kind() + "|"
                    + event.spotId() + "|" + event.operationId() + "|"
                    + event.payload() + "|" + event.objectGeneration() + "|"
                    + event.activeHandlers() + System.lineSeparator(),
                StandardCharsets.UTF_8,
                java.nio.file.StandardOpenOption.CREATE,
                java.nio.file.StandardOpenOption.APPEND);
        } catch (IOException error) {
            throw new IllegalStateException("failed to write Instance Spot evidence", error);
        }
    }
}

