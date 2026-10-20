package systems.zlink.framework.runtime.spots;

import static org.junit.jupiter.api.Assertions.assertEquals;

import org.junit.jupiter.api.Test;

import systems.zlink.framework.locations.ZLinkPlacementObjectKind;
import systems.zlink.framework.runtime.internal.metrics.ZLinkRuntimeMetricProbe;

final class ZLinkRelocationMetricOperationTest {
    @Test
    void targetCommitIsCompleted() throws Exception {
        assertOutcome(
                ZLinkRelocationTransitionClient.Settlement.TARGET_COMMITTED, false, "completed");
    }

    @Test
    void sourcePreserveIsAborted() throws Exception {
        assertOutcome(
                ZLinkRelocationTransitionClient.Settlement.SOURCE_PRESERVED, false, "aborted");
    }

    @Test
    void missingAuthorityIsFailed() throws Exception {
        assertOutcome(
                ZLinkRelocationTransitionClient.Settlement.SOURCE_LEASE_EXPIRED, false, "failed");
    }

    @Test
    void sourceShutdownBeforeSettlementIsShutdown() throws Exception {
        assertOutcome(null, true, "shutdown");
    }

    @Test
    void laterShutdownCannotReplaceConfirmedCommit() throws Exception {
        assertOutcome(
                ZLinkRelocationTransitionClient.Settlement.TARGET_COMMITTED, true, "completed");
    }

    @Test
    void laterShutdownCannotReplaceConfirmedPreserve() throws Exception {
        assertOutcome(ZLinkRelocationTransitionClient.Settlement.SOURCE_PRESERVED, true, "aborted");
    }

    @Test
    void terminationWithoutAnAuthorityObservationDoesNotProvePreserve() throws Exception {
        assertOutcome(null, false, "failed");
    }

    private static void assertOutcome(
            ZLinkRelocationTransitionClient.Settlement settlement,
            boolean sourceStopped,
            String outcome)
            throws Exception {
        try (var probe = ZLinkRuntimeMetricProbe.install()) {
            var operation =
                    new ZLinkRelocationMetricOperation(
                            "outcome-contract", ZLinkPlacementObjectKind.ACTOR, true);
            operation.complete(settlement, sourceStopped);
            assertEquals(outcome, probe.only("zlink.relocation.completed").tags().get("outcome"));
            assertEquals(outcome, probe.only("zlink.relocation.duration").tags().get("outcome"));
            probe.assertContractLabels();
        }
    }
}
