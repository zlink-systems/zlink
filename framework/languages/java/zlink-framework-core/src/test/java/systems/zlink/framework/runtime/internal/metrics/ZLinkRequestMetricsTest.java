package systems.zlink.framework.runtime.internal.metrics;

import static org.junit.jupiter.api.Assertions.assertEquals;

import java.util.concurrent.TimeoutException;
import org.junit.jupiter.api.Test;
import systems.zlink.contracts.errors.ZlinkRequestException;
import systems.zlink.contracts.sockets.RequestResult;
import systems.zlink.framework.errors.ZLinkFrameworkErrorKind;
import systems.zlink.framework.errors.ZLinkFrameworkException;

final class ZLinkRequestMetricsTest {
    @Test
    void inflightSurvivesProviderAttachmentAndTerminalOutcomesAreExact() throws Exception {
        ZLinkRequestMetrics.Series node = ZLinkRequestMetrics.node("late-mesh");
        ZLinkRequestMetrics.start(node);

        try (ZLinkRequestMetricProbe probe = ZLinkRequestMetricProbe.install()) {
            assertEquals(1L, probe.inflight("late-mesh", "node"));

            ZLinkRequestMetrics.complete(node, -1L, null);
            assertEquals(0L, probe.inflight("late-mesh", "node"));
            assertEquals(0L, probe.durationCount(
                "late-mesh", "node", "completed"));

            assertOutcome(probe, ZLinkRequestMetrics.channel("outcomes"),
                "outcomes", "channel", null, "completed", false);
            assertOutcome(probe, ZLinkRequestMetrics.spot("outcomes"),
                "outcomes", "spot", new IllegalStateException("failed"),
                "failed", false);
            assertOutcome(probe, ZLinkRequestMetrics.instanceSpot("outcomes"),
                "outcomes", "instance_spot", new ZLinkFrameworkException(
                    ZLinkFrameworkErrorKind.DEADLINE_EXCEEDED,
                    "deadline"), "timed_out", true);
            assertOutcome(probe, ZLinkRequestMetrics.actor("outcomes"),
                "outcomes", "actor",
                new RuntimeException(new TimeoutException("timeout")),
                "timed_out", true);
            assertOutcome(probe, ZLinkRequestMetrics.actor("transport-outcome"),
                "transport-outcome", "actor", new RuntimeException(
                    new ZlinkRequestException(RequestResult.TIMED_OUT)),
                "timed_out", true);
        }
    }

    private static void assertOutcome(
        ZLinkRequestMetricProbe probe,
        ZLinkRequestMetrics.Series series,
        String meshName,
        String surface,
        Throwable failure,
        String outcome,
        boolean timedOut) {
        ZLinkRequestMetrics.start(series);
        ZLinkRequestMetrics.complete(series, 1L, failure);

        assertEquals(0L, probe.inflight(meshName, surface));
        assertEquals(1L, probe.durationCount(meshName, surface, outcome));
        assertEquals(timedOut ? 1L : 0L,
            probe.timeoutCount(meshName, surface));
    }
}
