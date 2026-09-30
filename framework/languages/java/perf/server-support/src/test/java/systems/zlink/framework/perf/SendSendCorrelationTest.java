package systems.zlink.framework.perf;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertNotNull;
import static org.junit.jupiter.api.Assertions.assertNull;
import static org.junit.jupiter.api.Assertions.assertSame;
import static org.junit.jupiter.api.Assertions.assertThrows;
import static org.junit.jupiter.api.Assertions.assertTrue;

import org.junit.jupiter.api.Test;

import java.util.Base64;
import java.util.List;
import java.util.Map;
import java.util.concurrent.TimeUnit;

// §13: the harness correlation of a send/send operation and the family metrics that hang off it.
class SendSendCorrelationTest {
    private static RoleConfig config(int expiryMs) {
        return new RoleConfig("test", "s2s-channel-to-spot-send-send-echo/4096/test", "b".repeat(64), "channel", 0,
                "s2s-channel-to-spot-send-send-echo", "send-send", "ordinary", "routemesh", "ch", "mesh", Map.of(), null, "", "",
                true, "ObjectClient", true, null, List.of(), List.of(), null, null, null, "SpotWide",
                new RoleConfig.Workload(1024, .05, .05, 1, null, 1, 1, null, 1000, expiryMs, 5000, 30000, 5000, 1000), null,
                Map.of());
    }

    private static final class Fixture {
        final Measurement measurement;
        final ScenarioMetrics metrics;
        final SendSendCorrelation correlations;

        Fixture(int expiryMs) {
            measurement = new Measurement(config(expiryMs), true);
            metrics = new ScenarioMetrics(measurement);
            correlations = new SendSendCorrelation(measurement, metrics);
        }

        PerfEchoRequest request(long sequence) {
            return measurement.request(0, sequence, true).withReturnChannel("ch");
        }

        PerfEchoReply reply(PerfEchoRequest request) {
            return PayloadPattern.reply(request, PerfClock.now());
        }

        String count(String key) {
            return (String) measurement.snapshot(null).metrics.get(key);
        }

        SendSendCorrelation.Result complete(SendSendCorrelation.Entry entry) throws Exception {
            return correlations.completeAsync(entry).toCompletableFuture().get(5, TimeUnit.SECONDS);
        }

    }

    @Test
    void firstReplyIsTheResultAndLaterRepliesAreOnlyCounted() throws Exception {
        Fixture f = new Fixture(60);
        PerfEchoRequest request = f.request(1);
        SendSendCorrelation.Entry entry = f.correlations.register(request, PerfClock.now());
        f.correlations.firstSendEnded(entry, null);
        f.correlations.reply(f.reply(request));
        f.correlations.reply(f.reply(request));
        SendSendCorrelation.Result result = f.complete(entry);
        assertNull(result.error());
        assertTrue(result.completedTicks() >= entry.startedTicks());
        assertEquals("1", f.count("messages.duplicateReply"));
        assertEquals("0", f.count("messages.lateReply"));
    }

    @Test
    void echoBeforeTheFirstSendTerminalKeepsItsOwnTime() throws Exception {
        Fixture f = new Fixture(60);
        PerfEchoRequest request = f.request(1);
        SendSendCorrelation.Entry entry = f.correlations.register(request, PerfClock.now());
        f.correlations.reply(f.reply(request));
        Thread.sleep(30);
        f.correlations.firstSendEnded(entry, null);
        SendSendCorrelation.Result result = f.complete(entry);
        assertNull(result.error());
        assertTrue(PerfClock.now() - result.completedTicks() >= 25_000_000L, "The echo time was replaced by the later send terminal.");
    }

    @Test
    void expiryIsATimeoutAndALaterReplyIsLateNotASuccess() throws Exception {
        Fixture f = new Fixture(60);
        PerfEchoRequest request = f.request(1);
        SendSendCorrelation.Entry entry = f.correlations.register(request, PerfClock.now());
        f.correlations.firstSendEnded(entry, null);
        Throwable error = f.complete(entry).error();
        assertTrue(error instanceof PerfValidationException);
        assertEquals("CorrelationExpired", ((PerfValidationException) error).kind());
        f.correlations.reply(f.reply(request));
        assertEquals("1", f.count("messages.expired"));
        assertEquals("1", f.count("messages.lateReply"));
        assertEquals("0", f.count("messages.duplicateReply"));
        // The measurement classifies the expiry as a timeout, not a failure (expired is a subset of timeout).
        f.measurement.start(new PerfTriggerRequest("test", f.measurement.config().cellId(), "0", "warmup"), () -> {
            long started = f.measurement.beginOperation("send");
            assertTrue(started >= 0);
            f.measurement.completeOperation(started, error);
            return java.util.concurrent.CompletableFuture.completedFuture(null);
        });
        f.measurement.phaseTask().get(5, TimeUnit.SECONDS);
        PerfSnapshot snapshot = f.measurement.snapshot(null);
        assertEquals("1", snapshot.metrics.get("messages.timeout"));
        assertEquals("0", snapshot.metrics.get("messages.failed"));
        assertEquals("1", snapshot.metrics.get("messages.sent"));
    }

    @Test
    void aFailedFirstSendIsTheFinalResultUnlessTheEchoWasFirst() throws Exception {
        Fixture f = new Fixture(60);
        PerfEchoRequest failedSend = f.request(1);
        SendSendCorrelation.Entry entry = f.correlations.register(failedSend, PerfClock.now());
        IllegalStateException boom = new IllegalStateException("send failed");
        f.correlations.firstSendEnded(entry, boom);
        f.correlations.reply(f.reply(failedSend));
        assertSame(boom, f.complete(entry).error());
        assertEquals("1", f.count("messages.lateReply"));

        PerfEchoRequest echoFirst = f.request(2);
        SendSendCorrelation.Entry second = f.correlations.register(echoFirst, PerfClock.now());
        f.correlations.reply(f.reply(echoFirst));
        f.correlations.firstSendEnded(second, boom);
        assertNull(f.complete(second).error());
        assertEquals("1", f.count("messages.lateReply"));
    }

    @Test
    void aReplyWithAWrongPayloadOrIdentityFailsTheOperationAndIsNotASuccess() throws Exception {
        Fixture f = new Fixture(60);
        PerfEchoRequest request = f.request(1);
        SendSendCorrelation.Entry entry = f.correlations.register(request, PerfClock.now());
        f.correlations.firstSendEnded(entry, null);
        byte[] bytes = Base64.getDecoder().decode(request.payload());
        bytes[bytes.length - 1] ^= 1;
        PerfEchoReply good = f.reply(request);
        f.correlations.reply(new PerfEchoReply(good.runId(), good.cellId(), good.resetSeq(), good.phase(), good.clientId(),
                good.sequence(), good.correlationId(), good.receivedTicks(), good.clockDomainId(),
                Base64.getEncoder().encodeToString(bytes)));
        assertEquals("PayloadMismatch", ((PerfValidationException) f.complete(entry).error()).kind());

        PerfEchoRequest other = f.request(2);
        SendSendCorrelation.Entry second = f.correlations.register(other, PerfClock.now());
        PerfEchoReply wrong = f.reply(other);
        f.correlations.reply(new PerfEchoReply(wrong.runId(), wrong.cellId(), wrong.resetSeq(), wrong.phase(),
                wrong.clientId(), "99", wrong.correlationId(), wrong.receivedTicks(), wrong.clockDomainId(), wrong.payload()));
        assertEquals("IdentityMismatch", ((PerfValidationException) f.complete(second).error()).kind());
    }

    @Test
    void aReplyForAnUnissuedCorrelationIsCountedAndChangesNoOperation() throws Exception {
        Fixture f = new Fixture(60);
        PerfEchoRequest known = f.request(1);
        SendSendCorrelation.Entry entry = f.correlations.register(known, PerfClock.now());
        f.correlations.reply(f.reply(f.request(2)));
        assertEquals("1", f.count("messages.unknownCorrelation"));
        // The stray reply closed nothing: the issued operation still runs into its own expiry.
        assertEquals("CorrelationExpired", ((PerfValidationException) f.complete(entry).error()).kind());
        assertThrows(PerfValidationException.class, () -> f.correlations.register(known, PerfClock.now()));
    }

    @Test
    void admittedCountsOnlyANormalFirstSendInsideAnActiveCellNotTheSetupProbe() {
        Fixture f = new Fixture(60);
        SendSendCorrelation.Entry probe = f.correlations.register(f.request(1), PerfClock.now());
        f.correlations.firstSendEnded(probe, null); // Phase is "setup": a probe is not an admitted measured send
        assertEquals("0", f.count("messages.admitted"));
    }

    @Test
    void scenarioMetricsClearAtResetAndKeepUnsupportedKeysNullWithTheirReason() throws Exception {
        Fixture f = new Fixture(60);
        f.metrics.latency("driverLatencyMs", "driver.latency").spotInternalsUnsupported()
                .aliasLatency("latency", "spot.remoteCallLatency");
        f.metrics.count("driver.issued", 3);
        PerfSnapshot before = f.measurement.snapshot(null);
        assertEquals("3", before.metrics.get("driver.issued"));
        assertNull(before.metrics.get("spot.mailboxDepth.max"));
        assertEquals("PUBLIC_OBSERVATION_UNSUPPORTED", before.nullReasons.get("/metrics/spot.mailboxDepth.max").code());
        assertNull(before.metrics.get("spot.remoteCallLatency.p50Ms"));
        assertEquals("NO_SAMPLES", before.nullReasons.get("/metrics/spot.remoteCallLatency.p50Ms").code());
        assertNotNull(before.histograms.get("driverLatencyMs"));
        // A key the scenario never named keeps the reason Measurement gave it.
        assertEquals("NOT_APPLICABLE", before.nullReasons.get("/metrics/fanout.subscriberCount").code());
        f.measurement.start(new PerfTriggerRequest("test", f.measurement.config().cellId(), "0", "warmup"), null);
        f.measurement.phaseTask().get(5, TimeUnit.SECONDS);
        assertTrue(f.measurement.reset(new ResetRequest("test", f.measurement.config().cellId(), "1"), null).ok());
        assertEquals("0", f.measurement.snapshot(null).metrics.get("driver.issued"));
    }
}
