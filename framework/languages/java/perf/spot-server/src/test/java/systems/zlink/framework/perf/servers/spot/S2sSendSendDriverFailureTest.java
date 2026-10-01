package systems.zlink.framework.perf.servers.spot;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertTrue;

import org.junit.jupiter.api.Test;

import systems.zlink.framework.perf.Measurement;
import systems.zlink.framework.perf.PayloadPattern;
import systems.zlink.framework.perf.PerfClock;
import systems.zlink.framework.perf.PerfEchoRequest;
import systems.zlink.framework.perf.PerfTriggerRequest;
import systems.zlink.framework.perf.ResetRequest;
import systems.zlink.framework.perf.RoleConfig;
import systems.zlink.framework.perf.ScenarioMetrics;
import systems.zlink.framework.perf.SendSendCorrelation;

import java.util.List;
import java.util.Map;
import java.util.concurrent.TimeUnit;

class S2sSendSendDriverFailureTest {
    private static final String CELL = "s2s-spot-to-channel-send-send-echo/4096/test";

    private static RoleConfig config() {
        return new RoleConfig("test", CELL, "e".repeat(64), "java", "spot", 0,
                "s2s-spot-to-channel-send-send-echo", "send-send", "ordinary", "routemesh", "ch", "mesh",
                Map.of(), null, "", "", true, "Spot", true, null, List.of("spot-0"), List.of(), 1, null,
                null, "SpotWide", new RoleConfig.Workload(4096, .15, .01, 1, null, 1, 1, null,
                        1000, 1000, 2000, 1000, 5000, 1000), null, Map.of());
    }

    @Test
    void localDriverFailureDoesNotDiscardAValidatedCorrelationSuccess() throws Exception {
        Measurement measurement = new Measurement(config(), true);
        ScenarioMetrics metrics = new ScenarioMetrics(measurement)
                .counters("driver.failed")
                .latency("driverLatencyMs", "driver.latency");
        SendSendCorrelation correlations = new SendSendCorrelation(measurement, metrics);
        S2sSpotToChannelSendSendEchoScenario scenario = new S2sSpotToChannelSendSendEchoScenario(
                null, null, measurement, null, null, metrics, correlations);

        assertTrue(measurement.start(trigger("warmup", "0"), null).accepted());
        measurement.phaseTask().get(5, TimeUnit.SECONDS);
        assertTrue(measurement.reset(new ResetRequest("test", CELL, "1"), null).ok());
        assertTrue(measurement.start(trigger("measured", "1"), () -> {
            long started = measurement.beginOperation("send");
            PerfEchoRequest echo = measurement.request(0, 1, false).withReturnSpotId("spot-0").withSentTicks(started);
            SendSendCorrelation.Entry entry = correlations.register(echo, started);
            correlations.firstSendEnded(entry, null);
            correlations.reply(PayloadPattern.reply(echo, PerfClock.now()));
            scenario.driverFailed(new IllegalStateException("local driver failed after start"));
            return scenario.completeCorrelation(echo, PerfClock.now(), 0, false);
        }).accepted());
        measurement.phaseTask().get(5, TimeUnit.SECONDS);

        var snapshot = measurement.snapshot(null);
        assertEquals("1", snapshot.metrics.get("driver.failed"));
        assertEquals("1", snapshot.metrics.get("messages.completed"));
        assertEquals("0", snapshot.metrics.get("messages.failed"));
        assertEquals("0", ((Map<?, ?>) snapshot.histograms.get("driverLatencyMs")).get("count"));
    }

    @Test
    void successfulCorrelationRecordsDriverResultInterval() throws Exception {
        Measurement measurement = new Measurement(config(), true);
        ScenarioMetrics metrics = new ScenarioMetrics(measurement).latency("driverLatencyMs", "driver.latency");
        SendSendCorrelation correlations = new SendSendCorrelation(measurement, metrics);
        S2sSpotToChannelSendSendEchoScenario scenario = new S2sSpotToChannelSendSendEchoScenario(
                null, null, measurement, null, null, metrics, correlations);
        assertTrue(measurement.start(trigger("warmup", "0"), null).accepted());
        measurement.phaseTask().get(5, TimeUnit.SECONDS);
        assertTrue(measurement.reset(new ResetRequest("test", CELL, "1"), null).ok());
        assertTrue(measurement.start(trigger("measured", "1"), () -> {
            long started = measurement.beginOperation("send");
            PerfEchoRequest echo = measurement.request(0, 1, false).withReturnSpotId("spot-0").withSentTicks(started);
            SendSendCorrelation.Entry entry = correlations.register(echo, started);
            correlations.firstSendEnded(entry, null);
            correlations.reply(PayloadPattern.reply(echo, PerfClock.now()));
            long driverCompleted = PerfClock.now();
            return scenario.completeCorrelation(echo, started, driverCompleted, true);
        }).accepted());
        measurement.phaseTask().get(5, TimeUnit.SECONDS);
        var snapshot = measurement.snapshot(null);
        assertEquals("1", snapshot.metrics.get("messages.completed"));
        assertEquals("1", ((Map<?, ?>) snapshot.histograms.get("driverLatencyMs")).get("count"));
    }

    private static PerfTriggerRequest trigger(String phase, String resetSeq) {
        return new PerfTriggerRequest("test", CELL, resetSeq, phase);
    }
}
