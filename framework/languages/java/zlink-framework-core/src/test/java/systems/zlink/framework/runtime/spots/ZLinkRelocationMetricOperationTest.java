package systems.zlink.framework.runtime.spots;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertTrue;

import org.junit.jupiter.api.AfterEach;
import org.junit.jupiter.api.Test;

import systems.zlink.framework.configuration.ZLinkMessageFlowLogMode;
import systems.zlink.framework.locations.ZLinkPlacementObjectKind;
import systems.zlink.framework.monitoring.ZLinkFlowOrigin;
import systems.zlink.framework.runtime.configuration.ZLinkDispatchOptionsRegistration;
import systems.zlink.framework.runtime.diagnostics.ZLinkMessageFlowTracer;
import systems.zlink.framework.runtime.diagnostics.ZLinkTelemetryTestFence;
import systems.zlink.framework.runtime.internal.diagnostics.ZLinkFlowContext;
import systems.zlink.framework.runtime.internal.handlers.ZLinkHandlerActivator;
import systems.zlink.framework.runtime.internal.metrics.ZLinkRuntimeMetricProbe;

import java.util.ArrayList;
import java.util.List;
import java.util.logging.Handler;
import java.util.logging.Level;
import java.util.logging.LogRecord;
import java.util.logging.Logger;

final class ZLinkRelocationMetricOperationTest {
    private final Logger flowLogger = Logger.getLogger(ZLinkMessageFlowTracer.class.getName());
    private final boolean previousParents = flowLogger.getUseParentHandlers();
    private final Level previousLevel = flowLogger.getLevel();

    @AfterEach
    void restoreFlowLogger() {
        flowLogger.setUseParentHandlers(previousParents);
        flowLogger.setLevel(previousLevel);
        ZLinkTelemetryTestFence.awaitIdle();
    }

    @Test
    void sourceActorTerminalUsesItsCapturedFlow() {
        ZLinkTelemetryTestFence.awaitIdle();
        String flowId = "01930000-0000-7000-8000-000000000001";
        List<LogRecord> records = new ArrayList<>();
        Handler handler =
                new Handler() {
                    @Override
                    public void publish(LogRecord record) {
                        records.add(record);
                    }

                    @Override
                    public void flush() {}

                    @Override
                    public void close() {}
                };
        flowLogger.setUseParentHandlers(false);
        flowLogger.setLevel(Level.ALL);
        flowLogger.addHandler(handler);
        try {
            var options = new ZLinkDispatchOptionsRegistration();
            options.messageFlow(ZLinkMessageFlowLogMode.NORMAL);
            var tracer =
                    new ZLinkMessageFlowTracer(
                            options, ZLinkHandlerActivator.reflection(), Runnable::run);
            ZLinkRelocationMetricOperation operation;
            try (var ignored =
                    ZLinkFlowContext.enter(
                            new ZLinkFlowContext.State(
                                    flowId, ZLinkFlowOrigin.APPLICATION, null))) {
                operation =
                        new ZLinkRelocationMetricOperation(
                                "outcome-contract",
                                ZLinkPlacementObjectKind.ACTOR,
                                true,
                                tracer,
                                "actor-1");
            }
            operation.complete(ZLinkRelocationTransitionClient.Settlement.TARGET_COMMITTED, false);
            ZLinkTelemetryTestFence.awaitIdle();
            assertEquals(1, records.size());
            String record = records.getFirst().getMessage();
            assertTrue(record.contains("phase=completed"), record);
            assertTrue(record.contains("outcome=succeeded"), record);
            assertTrue(record.contains("surface=actor_relocation"), record);
            assertTrue(record.contains("kind=control"), record);
            assertTrue(record.contains("actor=actor-1"), record);
            assertTrue(record.contains("flow=" + flowId), record);
            assertTrue(record.contains("origin=application"), record);
        } finally {
            flowLogger.removeHandler(handler);
        }
    }

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
