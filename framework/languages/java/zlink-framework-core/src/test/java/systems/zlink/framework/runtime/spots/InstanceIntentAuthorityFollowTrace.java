package systems.zlink.framework.runtime.spots;

import systems.zlink.framework.runtime.diagnostics.ZLinkMessageFlowTracer;
import systems.zlink.framework.runtime.diagnostics.ZLinkTelemetryTestFence;

import java.time.Instant;
import java.util.Comparator;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.ConcurrentLinkedQueue;
import java.util.logging.Handler;
import java.util.logging.LogRecord;
import java.util.logging.Logger;
import java.util.stream.Collectors;

final class InstanceIntentAuthorityFollowTrace extends Handler implements AutoCloseable {
    private record Event(Instant time, String description) {}

    private final Logger logger = Logger.getLogger(ZLinkMessageFlowTracer.class.getName());
    private final String spotId;
    private final String spotField;
    private final ConcurrentLinkedQueue<Event> events = new ConcurrentLinkedQueue<>();

    private Instant requestStarted;

    InstanceIntentAuthorityFollowTrace(String spotId) {
        this.spotId = spotId;
        spotField = " spot=" + spotId + " ";
        logger.addHandler(this);
    }

    void beginRequest() {
        requestStarted = Instant.now();
        events.add(new Event(requestStarted, "logical request started"));
    }

    void record(String description) {
        events.add(new Event(Instant.now(), description));
    }

    // The fixture reuses this signal across the stale refusal and the real submission.
    CompletableFuture<Void> admissionSignal() {
        return new CompletableFuture<>() {
            @Override
            public boolean complete(Void value) {
                record("backend admission signal, already completed=" + isDone());
                return super.complete(value);
            }
        };
    }

    CompletionStage<?> observeSubmission(int attempt, CompletionStage<?> submission) {
        String prefix = "attempt " + attempt;
        record(prefix + " backend submission returned");
        submission.whenComplete(
                (reply, failure) -> {
                    if (failure == null) record(prefix + " backend reply");
                    else record(prefix + " backend failure " + failure);
                });
        return submission;
    }

    void attachTo(Throwable failure) {
        try {
            ZLinkTelemetryTestFence.awaitIdle();
        } catch (AssertionError fenceFailure) {
            failure.addSuppressed(fenceFailure);
        }
        failure.addSuppressed(
                new AssertionError(
                        "Authority follow trace for Spot "
                                + spotId
                                + "\n"
                                + events.stream()
                                        .filter(event -> !event.time().isBefore(requestStarted))
                                        .sorted(Comparator.comparing(Event::time))
                                        .map(event -> event.time() + " " + event.description())
                                        .collect(Collectors.joining("\n"))));
    }

    @Override
    public void publish(LogRecord record) {
        String message = record.getMessage();
        if (message.contains(spotField)) {
            events.add(new Event(record.getInstant(), record.getMessage()));
        }
    }

    @Override
    public void flush() {}

    @Override
    public void close() {
        logger.removeHandler(this);
    }
}
