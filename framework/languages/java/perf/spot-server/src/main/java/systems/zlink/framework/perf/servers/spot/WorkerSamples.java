package systems.zlink.framework.perf.servers.spot;

import systems.zlink.framework.perf.Measurement;
import systems.zlink.framework.perf.PerfClock;
import systems.zlink.framework.perf.ScenarioMetrics;

import java.util.concurrent.ConcurrentHashMap;
import java.util.Optional;

/** Holds worker intervals until the source driver has validated the primary echo. */
final class WorkerSamples {
    record Intervals(long submittedTicks, long taskStartedTicks, long taskEndedTicks, long resumedTicks) {}

    private final ConcurrentHashMap<String, Optional<Intervals>> pending = new ConcurrentHashMap<>();

    boolean begin(String correlationId) {
        return pending.putIfAbsent(correlationId, Optional.empty()) == null;
    }

    void observe(String correlationId, Intervals intervals) {
        pending.computeIfPresent(correlationId, (id, existing) -> {
            if (existing.isPresent()) {
                throw new IllegalStateException("Worker timing evidence was observed more than once for a correlation.");
            }
            return Optional.of(intervals);
        });
    }

    boolean complete(Measurement measurement, long startedTicks, String correlationId, Throwable error,
            Long completedTicks, ScenarioMetrics metrics) {
        Optional<Intervals> observed = pending.remove(correlationId);
        Intervals intervals = observed == null ? null : observed.orElse(null);
        long terminal = completedTicks == null ? PerfClock.now() : completedTicks;
        boolean counted = measurement.completeOperation(startedTicks, error, terminal);
        if (counted && intervals != null) {
            metrics.record("workerCallLatencyMs", intervals.submittedTicks(), intervals.resumedTicks(), terminal);
            metrics.record("workerSubmitToStartMs", intervals.submittedTicks(), intervals.taskStartedTicks(), terminal);
            metrics.record("workerTaskLatencyMs", intervals.taskStartedTicks(), intervals.taskEndedTicks(), terminal);
            metrics.record("workerResultToContinuationMs", intervals.taskEndedTicks(), intervals.resumedTicks(), terminal);
        }
        return counted;
    }

    void clear() {
        pending.clear();
    }
}
