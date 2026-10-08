package systems.zlink.framework.perf.servers.subscriber;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertTrue;

import org.junit.jupiter.api.Test;
import org.junit.jupiter.api.io.TempDir;

import systems.zlink.framework.perf.CellDirectory;
import systems.zlink.framework.perf.FanoutSupport;
import systems.zlink.framework.perf.Measurement;
import systems.zlink.framework.perf.ObjectsReadiness;
import systems.zlink.framework.perf.PerfClock;
import systems.zlink.framework.perf.PerfPublishEvent;
import systems.zlink.framework.perf.PerfSnapshot;
import systems.zlink.framework.perf.PerfTriggerRequest;
import systems.zlink.framework.perf.ResetRequest;
import systems.zlink.framework.perf.RoleConfig;

import java.nio.file.Path;
import java.util.List;
import java.util.Map;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.TimeUnit;

class FanoutReceiptsTest {
    @Test
    void firstHandlerEntryUsesTheSubscribersHalfOpenWindow(@TempDir Path directory)
            throws Exception {
        RoleConfig config =
                new RoleConfig(
                        "test",
                        "pubsub-fanout-echo/1024/test",
                        "a".repeat(64),
                        "java",
                        "subscriber",
                        0,
                        "pubsub-fanout-echo",
                        "publish",
                        "ordinary",
                        null,
                        "ch",
                        null,
                        Map.of(),
                        null,
                        "",
                        "",
                        false,
                        "None",
                        true,
                        null,
                        List.of(),
                        List.of(),
                        null,
                        1,
                        null,
                        "Framework default",
                        new RoleConfig.Workload(
                                1024, .05, .05, null, 1, 1, null, 30000, 30000, 5000, 1000),
                        null,
                        Map.of());
        Measurement measurement = new Measurement(config, false);
        FanoutReceipts receipts =
                new FanoutReceipts(
                        null,
                        measurement,
                        new ObjectsReadiness(false, ""),
                        new CellDirectory(directory));
        measurement.start(
                new PerfTriggerRequest(config.runId(), config.cellId(), "0", "warmup"), null);
        measurement.phaseTask().get(5, TimeUnit.SECONDS);
        assertTrue(
                measurement
                        .reset(new ResetRequest(config.runId(), config.cellId(), "1"), null)
                        .ok());
        assertTrue(
                measurement
                        .start(
                                new PerfTriggerRequest(
                                        config.runId(), config.cellId(), "1", "measured"),
                                () -> CompletableFuture.completedFuture(null))
                        .accepted());
        Map<String, Object> window = measurement.snapshot(null).window;
        long start = Long.parseLong((String) window.get("startTicks"));
        long end = Long.parseLong((String) window.get("endTicks"));

        receipts.record(event(config, measurement, 1), start - 1);
        receipts.record(event(config, measurement, 2), start);
        receipts.record(event(config, measurement, 3), end - 1);
        receipts.record(event(config, measurement, 4), end);
        receipts.record(event(config, measurement, 2), start);

        PerfSnapshot snapshot = measurement.snapshot(null);
        Map<?, ?> receiptsMetric = (Map<?, ?>) snapshot.runtimeMetrics.get("fanoutReceipts");
        Map<?, ?> counts = (Map<?, ?>) receiptsMetric.get("value");
        assertEquals("2", counts.get("uniqueInWindow"));
        assertEquals("4", counts.get("measuredEventsSeen"));
        assertEquals("2", counts.get("measuredOutsideWindow"));
        assertEquals("1", snapshot.metrics.get("fanout.duplicateEvents"));
    }

    private static PerfPublishEvent event(
            RoleConfig config, Measurement measurement, long sequence) {
        return new PerfPublishEvent(
                config.runId(),
                config.cellId(),
                "1",
                "measured",
                Long.toString(sequence),
                FanoutSupport.TOPIC,
                Long.toString(PerfClock.now()),
                PerfClock.DOMAIN,
                measurement.pattern().base64());
    }
}
