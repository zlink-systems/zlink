package systems.zlink.framework.perf;

import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.ConcurrentHashMap;
import java.util.concurrent.TimeUnit;

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

    public record Result(Throwable error, long completedTicks) {}

    public static final class Entry {
        private final PerfEchoRequest request;
        private final long startedTicks;
        private final long expiresAtTicks;
        private final CompletableFuture<Throwable> result = new CompletableFuture<>();
        private int state = PENDING;
        private volatile long closedTicks;

        private Entry(PerfEchoRequest request, long startedTicks, long expiresAtTicks) {
            this.request = request;
            this.startedTicks = startedTicks;
            this.expiresAtTicks = expiresAtTicks;
        }

        public long startedTicks() {
            return startedTicks;
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
        CompletableFuture.delayedExecutor(Math.max(0, entry.expiresAtTicks - PerfClock.now()), TimeUnit.NANOSECONDS)
                .execute(() -> expireIfDue(entry));
        return entry;
    }

    public Entry find(String correlationId) {
        return entries.get(correlationId);
    }

    /** The terminal of the first public send: a normal admission is counted; a failure is the final result unless the echo was already fixed first. */
    public void firstSendEnded(Entry entry, Throwable error) {
        synchronized (entry) {
            long now = PerfClock.now();
            if (expireIfDue(entry, now)) {
                return;
            }
            if (error == null) {
                if (!"setup".equals(measurement.phase())) {
                    metrics.count("messages.admitted");
                }
            } else {
                close(entry, FAILED, Measurement.unwrap(error), now);
            }
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
        synchronized (entry) {
            long now = PerfClock.now();
            expireIfDue(entry, now);
            if (!close(entry, invalid == null ? SUCCEEDED : FAILED, invalid, now)) {
                metrics.count(entry.state == SUCCEEDED ? "messages.duplicateReply" : "messages.lateReply");
            }
        }
    }

    /**
     * The final result once the first send has ended: the first result of the correlation, or its expiry. The time is
     * when that result was fixed, so an echo seen before the first send's terminal keeps its own time.
     */
    public CompletionStage<Result> completeAsync(Entry entry) {
        expireIfDue(entry);
        return entry.result.thenApplyAsync(error -> new Result(error, entry.closedTicks));
    }

    private boolean expireIfDue(Entry entry) {
        synchronized (entry) {
            return expireIfDue(entry, PerfClock.now());
        }
    }

    private boolean expireIfDue(Entry entry, long now) {
        if (entry.state != PENDING || now < entry.expiresAtTicks) {
            return false;
        }
        if (close(entry, EXPIRED, expiredError(), now)) {
            metrics.count("messages.expired");
        }
        return true;
    }

    private boolean close(Entry entry, int state, Throwable error, long now) {
        if (entry.state != PENDING) {
            return false;
        }
        entry.state = state;
        entry.closedTicks = now;
        entry.result.complete(error);
        return true;
    }

    private static PerfValidationException expiredError() {
        return new PerfValidationException("CorrelationExpired", "No return send arrived before the correlation deadline.");
    }
}
