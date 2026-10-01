package systems.zlink.framework.perf.servers.spot;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertTrue;

import org.junit.jupiter.api.Test;

import systems.zlink.framework.perf.Measurement;
import systems.zlink.framework.perf.PerfClock;
import systems.zlink.framework.perf.PerfTriggerRequest;
import systems.zlink.framework.perf.ResetRequest;
import systems.zlink.framework.perf.RoleConfig;
import systems.zlink.framework.perf.ScenarioMetrics;

import java.util.List;
import java.util.Map;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.locks.LockSupport;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicBoolean;

class WorkerSamplesTest {
    private static final String CELL = "spot-worker-offload-echo/1024/test";

    private static RoleConfig config() {
        return new RoleConfig("test", CELL, "c".repeat(64), "java", "spot", 0, "spot-worker-offload-echo",
                "request", "ordinary", "routemesh", "ch", "mesh", Map.of(), null, "", "", true, "Spot", true,
                null, List.of("spot-0"), List.of(), 1, null,
                new RoleConfig.WorkerConfig("xorshift32-v1", 1, 1, 1, 100, 1000), "SpotWide",
                new RoleConfig.Workload(1024, .15, .01, 1, null, 1, 1, null, 1000, 1000, 1000, 1000, 5000, 1000),
                null, Map.of());
    }

    @Test
    void workerHistogramsIncludeOnlyPrimarySuccessfulOperations() throws Exception {
        Measurement measurement = new Measurement(config(), true);
        ScenarioMetrics metrics = new ScenarioMetrics(measurement)
                .latency("workerCallLatencyMs", "worker.callLatency")
                .latency("workerSubmitToStartMs", "worker.submitToStart")
                .latency("workerTaskLatencyMs", "worker.taskLatency")
                .latency("workerResultToContinuationMs", "worker.resultToContinuation");
        WorkerSamples samples = new WorkerSamples();

        assertTrue(measurement.start(trigger("warmup", "0"), null).accepted());
        measurement.phaseTask().get(5, TimeUnit.SECONDS);
        assertTrue(measurement.reset(new ResetRequest("test", CELL, "1"), null).ok());
        AtomicBoolean successfulWindowSample = new AtomicBoolean();

        assertTrue(measurement.start(trigger("measured", "1"), () -> {
            long successfulStarted = measurement.beginOperation();
            assertTrue(samples.begin("successful"));
            observe(samples, "successful");
            successfulWindowSample.set(samples.complete(measurement, successfulStarted, "successful", null,
                    PerfClock.now(), metrics));

            long failedStarted = measurement.beginOperation();
            assertTrue(samples.begin("failed"));
            observe(samples, "failed");
            samples.complete(measurement, failedStarted, "failed", new IllegalStateException("primary echo failed"),
                    null, metrics);
            return CompletableFuture.completedFuture(null);
        }).accepted());
        measurement.phaseTask().get(5, TimeUnit.SECONDS);

        assertTrue(successfulWindowSample.get());
        var snapshot = measurement.snapshot(null);
        for (String key : List.of("workerCallLatencyMs", "workerSubmitToStartMs", "workerTaskLatencyMs",
                "workerResultToContinuationMs")) {
            assertEquals("1", ((Map<?, ?>) snapshot.histograms.get(key)).get("count"), key);
        }
        assertEquals("1", snapshot.metrics.get("messages.failed"));
    }

    private static void observe(WorkerSamples samples, String correlationId) {
        long submitted = PerfClock.now();
        samples.observe(correlationId, new WorkerSamples.Intervals(submitted, submitted + 100_000L,
                submitted + 200_000L, submitted + 300_000L));
        LockSupport.parkNanos(2_000_000L);
    }

    private static PerfTriggerRequest trigger(String phase, String resetSeq) {
        return new PerfTriggerRequest("test", CELL, resetSeq, phase);
    }
}
