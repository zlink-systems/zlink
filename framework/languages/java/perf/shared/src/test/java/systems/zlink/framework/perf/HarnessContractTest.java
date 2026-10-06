package systems.zlink.framework.perf;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertNotEquals;
import static org.junit.jupiter.api.Assertions.assertNull;
import static org.junit.jupiter.api.Assertions.assertSame;
import static org.junit.jupiter.api.Assertions.assertThrows;
import static org.junit.jupiter.api.Assertions.assertTrue;

import org.junit.jupiter.api.Test;
import org.junit.jupiter.params.ParameterizedTest;
import org.junit.jupiter.params.provider.ValueSource;

import java.util.Base64;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CancellationException;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.TimeUnit;

// The harness contract of the perf README §4, §13, §15: what the .NET HarnessContractTests check, for the Java harness.
class HarnessContractTest {
    private static RoleConfig config(double seconds) {
        return config("java", 0, 1, 1, seconds);
    }

    private static RoleConfig config(String language, int roleInstance, Integer connections, int clientCount, double seconds) {
        return new RoleConfig("test", "session-echo-only/1024/test", "a".repeat(64), language, "client", roleInstance,
                "session-echo-only", "request",
                "ordinary", null, null, null, Map.of(), null, "", "", false, "None", true, null, List.of(), List.of(), null, null,
                null, "Immediate", new RoleConfig.Workload(1024, seconds, seconds, 1, connections, null, clientCount, 1,
                        1000, 1000, 2000, 30000, 5000, 1000), null, Map.of());
    }

    private static PerfTriggerRequest trigger(Measurement measurement, String phase, String seq) {
        return new PerfTriggerRequest(measurement.config().runId(), measurement.config().cellId(), seq, phase);
    }

    private static ResetRequest reset(Measurement measurement, String seq) {
        return new ResetRequest(measurement.config().runId(), measurement.config().cellId(), seq);
    }

    @Test
    void payloadValidatesEveryByteAndCanonicalPaddedBase64() {
        for (int size : new int[] {1024, 4096}) {
            PayloadPattern pattern = new PayloadPattern(size);
            pattern.validate(pattern.base64());
            byte[] bytes = Base64.getDecoder().decode(pattern.base64());
            assertEquals(size, bytes.length);
            assertEquals(29, bytes[0]);
            bytes[size - 1] ^= 1;
            String tampered = Base64.getEncoder().encodeToString(bytes);
            assertThrows(PerfValidationException.class, () -> pattern.validate(tampered));
            assertThrows(PerfValidationException.class, () -> pattern.validate(pattern.base64() + "\n"));
        }
    }

    @Test
    void histogramKeepsExactSumOverflowAndInclusiveBounds() {
        Histogram histogram = new Histogram();
        histogram.record(100_000);
        histogram.record(100_001);
        histogram.record(100_024_000_001L);
        Map<String, Object> metrics = new LinkedHashMap<>();
        Map<String, Object> histograms = new LinkedHashMap<>();
        Map<String, NullReason> reasons = new LinkedHashMap<>();
        histogram.export("latencyMs", "latency", metrics, histograms, reasons);
        Map<String, Object> snapshot = histogram.snapshot();
        @SuppressWarnings("unchecked")
        List<String> counts = (List<String>) snapshot.get("counts");
        assertEquals("1", counts.get(10));
        assertEquals("1", counts.get(11));
        assertEquals("1", snapshot.get("overflow"));
        assertEquals("3", snapshot.get("count"));
        assertEquals("100024200002", snapshot.get("sumNs"));
        assertEquals(0.125, metrics.get("latency.p50Ms"));
        assertNull(metrics.get("latency.p95Ms"));
        assertEquals("HISTOGRAM_OVERFLOW", reasons.get("/metrics/latency.p95Ms").code());
        assertEquals(100000.0, reasons.get("/metrics/latency.p95Ms").lowerBoundMs());
        assertEquals(100024.000001, (Double) metrics.get("latency.maxMs"), 1e-9);
        assertEquals("nearest-rank-bucket-upper-bound-capped-by-max", snapshot.get("percentileMethod"));
    }

    @Test
    void percentilesDoNotExceedTheLargestObservedLatency() {
        Histogram histogram = new Histogram();
        histogram.record(1_001_000);
        Map<String, Object> metrics = new LinkedHashMap<>();
        histogram.export("latencyMs", "latency", metrics, new LinkedHashMap<>(), new LinkedHashMap<>());

        assertEquals(1.001, metrics.get("latency.p50Ms"));
        assertEquals(1.001, metrics.get("latency.p95Ms"));
        assertEquals(1.001, metrics.get("latency.p99Ms"));
    }

    @Test
    void emptyHistogramHasReasonsForEveryLatencyAndMax() {
        Map<String, Object> metrics = new LinkedHashMap<>();
        Map<String, NullReason> reasons = new LinkedHashMap<>();
        new Histogram().export("latencyMs", "latency", metrics, new LinkedHashMap<>(), reasons);
        metrics.values().forEach(value -> assertNull(value));
        assertEquals(6, reasons.size());
        reasons.values().forEach(value -> assertEquals("NO_SAMPLES", value.code()));
    }

    @ParameterizedTest
    @ValueSource(strings = {"01", "+1", "-0", "18446744073709551616"})
    void decimalU64RejectsNoncanonicalOrOutOfRange(String value) {
        PerfValidationException error = assertThrows(PerfValidationException.class, () -> DecimalText.u64(value));
        assertEquals("SchemaMismatch", error.kind());
    }

    @ParameterizedTest
    @ValueSource(strings = {"01", "+1", "-0", "9223372036854775808"})
    void decimalI64RejectsNoncanonicalOrOutOfRange(String value) {
        PerfValidationException error = assertThrows(PerfValidationException.class, () -> DecimalText.i64(value));
        assertEquals("SchemaMismatch", error.kind());
    }

    @Test
    void jsonRejectsMissingFieldsAndWrongTypes() {
        assertThrows(PerfJson.PerfJsonException.class, () -> PerfJson.read("{\"runId\":\"a\",\"cellId\":\"b\",\"resetSeq\":1}", ResetRequest.class));
        assertThrows(PerfJson.PerfJsonException.class, () -> PerfJson.read("{\"runId\":\"a\",\"unknown\":\"b\"}", ResetRequest.class));
        assertEquals("1", PerfJson.read("{\"runId\":\"a\",\"cellId\":\"b\",\"resetSeq\":\"1\"}", ResetRequest.class).resetSeq());
    }

    @Test
    void resetRejectsAnOutstandingWarmupAndMeasuredCannotStartBeforeReset() throws Exception {
        Measurement measurement = new Measurement(config(.05), true);
        CompletableFuture<Void> outstanding = new CompletableFuture<>();
        assertTrue(measurement.start(trigger(measurement, "warmup", "0"), () -> {
            long started = measurement.beginOperation();
            assertTrue(started >= 0);
            return outstanding.thenRun(() -> measurement.completeOperation(started));
        }).accepted());
        Thread.sleep(80); // The test controls a pending operation beyond the warmup window.
        assertFalse(measurement.reset(reset(measurement, "1"), null).ok());
        assertFalse(measurement.start(trigger(measurement, "measured", "1"), null).accepted());
        outstanding.complete(null);
        measurement.phaseTask().get(5, TimeUnit.SECONDS);
        ResetReply ack = measurement.reset(reset(measurement, "1"), null);
        assertTrue(ack.ok());
        assertSame(ack, measurement.reset(reset(measurement, "1"), null));
        assertFalse(measurement.reset(reset(measurement, "0"), null).ok());
        assertTrue(measurement.start(trigger(measurement, "measured", "1"), null).accepted());
        assertFalse(measurement.reset(reset(measurement, "1"), null).ok());
        measurement.phaseTask().get(5, TimeUnit.SECONDS);
        assertSame(ack, measurement.reset(reset(measurement, "1"), null));
    }

    @Test
    void publicStatusSamplingDoesNotHoldTheApplicationCounterLock() throws Exception {
        Measurement measurement = new Measurement(config(.12), true);
        measurement.samplePublicState(() -> {
            CompletableFuture<String> read = CompletableFuture.supplyAsync(measurement::phase);
            try {
                return Map.of("phase", read.get(1, TimeUnit.SECONDS));
            } catch (Exception error) {
                throw new AssertionError("Public status observation held the application counter lock.", error);
            }
        });
        assertTrue(measurement.start(trigger(measurement, "warmup", "0"), null).accepted());
        measurement.phaseTask().get(5, TimeUnit.SECONDS);
        assertFalse(measurement.hasErrors());
    }

    @Test
    void measuredPhaseEndsWithoutWaitingForOutstandingOperations() throws Exception {
        Measurement measurement = new Measurement(config(.08), true);
        measurement.start(trigger(measurement, "warmup", "0"), null);
        measurement.phaseTask().get(5, TimeUnit.SECONDS);
        assertTrue(measurement.reset(reset(measurement, "1"), null).ok());
        CompletableFuture<Void> pending = new CompletableFuture<>();
        CompletableFuture<Void> entered = new CompletableFuture<>();
        int[] calls = {0};
        java.util.function.Supplier<CompletionStage<Void>> workload = () -> {
            calls[0]++;
            long started = measurement.beginOperation();
            assertTrue(started >= 0);
            entered.complete(null);
            return pending.thenRun(() -> measurement.completeOperation(started));
        };
        PerfTriggerRequest trigger = trigger(measurement, "measured", "1");
        assertTrue(measurement.start(trigger, workload).accepted());
        assertEquals("alreadyStarted", measurement.start(trigger, workload).state());
        entered.get(5, TimeUnit.SECONDS);
        Thread.sleep(100);
        assertEquals(-1, measurement.beginOperation());
        boolean phaseCompletedAtWindowEnd = measurement.phaseTask().isDone();
        pending.complete(null);
        measurement.phaseTask().get(5, TimeUnit.SECONDS);
        PerfSnapshot snapshot = measurement.snapshot(null);
        assertEquals(1, calls[0]);
        assertTrue(phaseCompletedAtWindowEnd);
        assertEquals("1", snapshot.metrics.get("messages.sent"));
        assertEquals("0", snapshot.metrics.get("messages.completed"));
        assertEquals("1", snapshot.metrics.get("messages.inflightAtEnd"));
        assertFalse(snapshot.metrics.containsKey("messages.settleCompleted"));
        assertFalse(snapshot.metrics.containsKey("messages.unresolved"));
        assertEquals(0.0, snapshot.metrics.get("throughput.kops"));
        assertEquals("0", ((Map<?, ?>) snapshot.histograms.get("latencyMs")).get("count"));
        assertFalse(snapshot.histograms.containsKey("settleLatencyMs"));
        assertFalse(snapshot.window.containsKey("settleSeconds"));
        assertEquals("complete", snapshot.phase);
        assertEquals("1", snapshot.metrics.get("messages.sent"));
        assertEquals("0", snapshot.metrics.get("messages.failed"));
        assertEquals("0", snapshot.metrics.get("messages.timeout"));
        assertEquals("0", snapshot.metrics.get("messages.cancelled"));
    }

    @Test
    void terminalAtTheWindowEndCountsAsInflightAtEnd() throws Exception {
        Measurement measurement = new Measurement(config(.05), true);
        measurement.start(trigger(measurement, "warmup", "0"), null);
        measurement.phaseTask().get(5, TimeUnit.SECONDS);
        assertTrue(measurement.reset(reset(measurement, "1"), null).ok());
        boolean[] success = {true};

        measurement.start(trigger(measurement, "measured", "1"), () -> {
            long started = measurement.beginOperation();
            success[0] = measurement.completeOperation(started, null, measurement.endTicks());
            return CompletableFuture.completedFuture(null);
        });
        measurement.phaseTask().get(5, TimeUnit.SECONDS);

        PerfSnapshot snapshot = measurement.snapshot(null);
        assertEquals("0", snapshot.metrics.get("messages.completed"));
        assertEquals("1", snapshot.metrics.get("messages.inflightAtEnd"));
        assertEquals("0", ((Map<?, ?>) snapshot.histograms.get("latencyMs")).get("count"));
        assertFalse(success[0]);
    }

    @Test
    void terminalObservedBeforeWindowEndButCommittedAfterSealRemainsInflight() throws Exception {
        Measurement measurement = new Measurement(config(.05), true);
        measurement.start(trigger(measurement, "warmup", "0"), null);
        measurement.phaseTask().get(5, TimeUnit.SECONDS);
        assertTrue(measurement.reset(reset(measurement, "1"), null).ok());
        assertTrue(measurement.start(trigger(measurement, "measured", "1"),
                () -> CompletableFuture.completedFuture(null)).accepted());
        long started = measurement.beginOperation();
        assertTrue(started >= 0);
        long terminalTicks = PerfClock.now();
        measurement.phaseTask().get(5, TimeUnit.SECONDS);
        assertFalse(measurement.completeOperation(started, null, terminalTicks));
        PerfSnapshot snapshot = measurement.snapshot(null);
        assertEquals("0", snapshot.metrics.get("messages.completed"));
        assertEquals("1", snapshot.metrics.get("messages.inflightAtEnd"));
    }

    @Test
    void aLateTerminalLeavesTheSealedResultUnchanged() throws Exception {
        Measurement measurement = new Measurement(config(.05), true);
        measurement.start(trigger(measurement, "warmup", "0"), null);
        measurement.phaseTask().get(5, TimeUnit.SECONDS);
        assertTrue(measurement.reset(reset(measurement, "1"), null).ok());
        assertTrue(measurement.start(trigger(measurement, "measured", "1"),
                () -> CompletableFuture.completedFuture(null)).accepted());
        long started = measurement.beginOperation();
        assertTrue(started >= 0);
        measurement.phaseTask().get(5, TimeUnit.SECONDS);
        PerfSnapshot sealed = measurement.snapshot(null);
        measurement.completeOperation(started);
        PerfSnapshot after = measurement.snapshot(null);
        assertEquals(sealed.metrics.get("messages.completed"), after.metrics.get("messages.completed"));
        assertEquals(sealed.metrics.get("messages.inflightAtEnd"), after.metrics.get("messages.inflightAtEnd"));
        assertEquals(sealed.histograms.get("latencyMs"), after.histograms.get("latencyMs"));
        assertTrue(measurement.reset(reset(measurement, "2"), null).ok());
    }

    @Test
    void publicCancellationExceptionIsCountedAsCancelled() throws Exception {
        Measurement measurement = new Measurement(config(.05), true);
        measurement.start(trigger(measurement, "warmup", "0"), null);
        measurement.phaseTask().get(5, TimeUnit.SECONDS);
        assertTrue(measurement.reset(reset(measurement, "1"), null).ok());

        measurement.start(trigger(measurement, "measured", "1"), () -> {
            long started = measurement.beginOperation();
            measurement.completeOperation(started, new CancellationException("cancelled"));
            return CompletableFuture.completedFuture(null);
        });
        measurement.phaseTask().get(5, TimeUnit.SECONDS);

        PerfSnapshot snapshot = measurement.snapshot(null);
        assertEquals("1", snapshot.metrics.get("messages.cancelled"));
        assertEquals("0", snapshot.metrics.get("messages.failed"));
        assertEquals("0", snapshot.metrics.get("messages.inflightAtEnd"));
    }

    @Test
    void completeOperationReturnsWhetherAWindowSuccessWasCounted() throws Exception {
        Measurement measurement = new Measurement(config(.05), true);
        measurement.start(trigger(measurement, "warmup", "0"), null);
        measurement.phaseTask().get(5, TimeUnit.SECONDS);
        assertTrue(measurement.reset(reset(measurement, "1"), null).ok());
        boolean[] counted = {false};
        measurement.start(trigger(measurement, "measured", "1"), () -> {
            long started = measurement.beginOperation();
            counted[0] = measurement.completeOperation(started, null, PerfClock.now());
            return CompletableFuture.completedFuture(null);
        });
        measurement.phaseTask().get(5, TimeUnit.SECONDS);
        assertTrue(counted[0]);
        assertEquals("1", measurement.snapshot(null).metrics.get("messages.completed"));
    }

    @Test
    void inflightAtEndIsNumericBeforeTheWindowStarts() {
        Measurement measurement = new Measurement(config(.05), true);
        assertEquals("0", measurement.snapshot(null).metrics.get("messages.inflightAtEnd"));
    }

    @Test
    void warmupSuccessReturnsCountedIndependentlyOfPhase() throws Exception {
        Measurement measurement = new Measurement(config(.05), true);
        boolean[] counted = {false};
        measurement.start(trigger(measurement, "warmup", "0"), () -> {
            long started = measurement.beginOperation();
            counted[0] = measurement.completeOperation(started);
            return CompletableFuture.completedFuture(null);
        });
        measurement.phaseTask().get(5, TimeUnit.SECONDS);
        assertTrue(counted[0]);
    }

    @Test
    void snapshotCarriesTheSameTopLevelKeysAsTheDotnetOriginal() {
        Measurement measurement = new Measurement(config("kotlin", 0, 1, 1, .05), true);
        Map<String, Object> document = measurement.snapshot(null).toMap();
        assertEquals(List.of("schemaVersion", "runId", "cellId", "resetSeq", "language", "role", "roleInstance", "configHash",
                "phase", "window", "clock", "serializedMessageBytes", "metrics", "histograms", "nullReasons", "publicStatus",
                "publicMetrics", "runtimeMetrics", "provenance"), List.copyOf(document.keySet()));
        assertEquals("kotlin", document.get("language"));
        assertNotEquals(0, document.size());
    }

    @Test
    void connectorRangeIsSharedByTheScenarioAndRequestedMetric() {
        Measurement first = new Measurement(config("java", 0, 10, 3, .05), false);
        Measurement second = new Measurement(config("java", 1, 10, 3, .05), false);
        Measurement third = new Measurement(config("java", 2, 10, 3, .05), false);

        assertEquals(new Measurement.ConnectionRange(0, 4), first.connectionRange());
        assertEquals(new Measurement.ConnectionRange(4, 3), second.connectionRange());
        assertEquals(new Measurement.ConnectionRange(7, 3), third.connectionRange());
        assertEquals("3", second.snapshot(null).metrics.get("connections.requested"));
    }

    @Test
    void windowPredicateUsesTheSameHalfOpenIntervalAtBothBoundaries() throws Exception {
        Measurement measurement = new Measurement(config(.05), true);
        measurement.start(trigger(measurement, "warmup", "0"), null);
        Map<String, Object> window = measurement.snapshot(null).window;
        long start = Long.parseLong((String) window.get("startTicks"));
        long end = measurement.endTicks();

        assertFalse(measurement.windowContainsTicks(start - 1));
        assertTrue(measurement.windowContainsTicks(start));
        assertTrue(measurement.windowContainsTicks(end - 1));
        assertFalse(measurement.windowContainsTicks(end));

        measurement.phaseTask().get(5, TimeUnit.SECONDS);
    }
}
