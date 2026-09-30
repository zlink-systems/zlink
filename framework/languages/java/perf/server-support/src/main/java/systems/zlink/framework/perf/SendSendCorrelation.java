package systems.zlink.framework.perf;

import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.ConcurrentHashMap;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.TimeoutException;
import java.util.concurrent.atomic.AtomicInteger;

// The harness correlation of a send/send operation (§13): the first public send and the return send are two one-way
// calls, tied together only by the correlationId in the DTO. The first result of a correlation stands; a reply that
// arrives after that is only counted (duplicate, late or unknown). The table clears with the window at reset.
//
// Order of one operation: register (right before the first public send, fixing the expiry deadline), then
// firstSendEnded with that send's terminal, then completeAsync for the final result. The return handler calls reply.
public final class SendSendCorrelation {
    private static final int PENDING = 0;
    private static final int SUCCEEDED = 1;
    private static final int FAILED = 2;
    private static final int EXPIRED = 3;

    /** The final result of one correlation: the failure (null on success) and when the result was fixed. */
    public record Result(Throwable error, long completedTicks) {}

    public static final class Entry {
        private final PerfEchoRequest request;
        private final long startedTicks;
        private final long expiresAtTicks;
        private final CompletableFuture<Throwable> result = new CompletableFuture<>();
        private final AtomicInteger state = new AtomicInteger(PENDING);
        private volatile long closedTicks;

        private Entry(PerfEchoRequest request, long startedTicks, long expiresAtTicks) {
            this.request = request;
            this.startedTicks = startedTicks;
            this.expiresAtTicks = expiresAtTicks;
        }

        public long startedTicks() {
            return startedTicks;
        }

        private boolean close(int newState, Throwable error) {
            if (!state.compareAndSet(PENDING, newState)) {
                return false;
            }
            closedTicks = PerfClock.now();
            result.complete(error);
            return true;
        }
    }

    private final ConcurrentHashMap<String, Entry> entries = new ConcurrentHashMap<>();
    private final Measurement measurement;
    private final ScenarioMetrics metrics;

    public SendSendCorrelation(Measurement measurement, ScenarioMetrics metrics) {
        this.measurement = measurement;
        this.metrics = metrics.counters("messages.admitted", "messages.expired", "messages.duplicateReply",
                "messages.lateReply", "messages.unknownCorrelation");
        metrics.onReset(entries::clear);
    }

    public Entry register(PerfEchoRequest request, long startedTicks) {
        Entry entry = new Entry(request, startedTicks,
                PerfClock.now() + measurement.config().workload().correlationExpiryMs() * 1_000_000L);
        if (entries.putIfAbsent(request.correlationId(), entry) != null) {
            throw new PerfValidationException("IdentityMismatch", "A correlationId was issued twice.");
        }
        return entry;
    }

    public Entry find(String correlationId) {
        return entries.get(correlationId);
    }

    /** The terminal of the first public send: a normal admission is counted; a failure is the final result unless the echo was already fixed first. */
    public void firstSendEnded(Entry entry, Throwable error) {
        if (error == null) {
            if (!"setup".equals(measurement.phase())) {
                metrics.count("messages.admitted");
            }
        } else {
            entry.close(FAILED, Measurement.unwrap(error));
        }
    }

    /** The return handler's one call: the reply's identity and payload decide the first result. */
    public void reply(PerfEchoReply reply) {
        Entry entry = entries.get(reply.correlationId());
        if (entry == null) {
            metrics.count("messages.unknownCorrelation");
            return;
        }
        Throwable invalid = null;
        try {
            PayloadPattern.validateIdentity(entry.request, reply);
            measurement.pattern().validate(reply.payload());
        } catch (PerfValidationException error) {
            invalid = error;
        }
        if (!entry.close(invalid == null ? SUCCEEDED : FAILED, invalid)) {
            metrics.count(entry.state.get() == SUCCEEDED ? "messages.duplicateReply" : "messages.lateReply");
        }
    }

    /**
     * The final result once the first send has ended: the first result of the correlation, or its expiry. The time is
     * when that result was fixed, so an echo seen before the first send's terminal keeps its own time.
     */
    public CompletionStage<Result> completeAsync(Entry entry) {
        long remaining = Math.max(0, entry.expiresAtTicks - PerfClock.now());
        CompletableFuture<Throwable> timed = entry.result.copy().orTimeout(remaining, TimeUnit.NANOSECONDS);
        return timed.handle((error, thrown) -> {
            if (thrown != null && Measurement.unwrap(thrown) instanceof TimeoutException
                    && entry.close(EXPIRED, new PerfValidationException("CorrelationExpired",
                            "No return send arrived before the correlation deadline."))) {
                metrics.count("messages.expired");
            }
            return (Void) null;
        }).thenCompose(ignored -> entry.result)
                // The result is fixed inside the return handler's turn; the operation loop continues on its own thread,
                // like a task continuation, so it never issues its next send from inside that handler.
                .thenApplyAsync(error -> new Result(error, entry.closedTicks));
    }
}
