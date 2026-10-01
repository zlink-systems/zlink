package systems.zlink.framework.runtime.internal.metrics;

import static org.junit.jupiter.api.Assertions.assertEquals;

import org.junit.jupiter.api.Test;

import systems.zlink.framework.monitoring.ZLinkApplicationJobQueuePressureState;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceTopologyRegistry;

final class ZLinkMetricWireValuesTest {
    @Test
    void metricUnitsHaveStableSymbols() {
        assertEquals("s", ZLinkRuntimeMetrics.Unit.SECONDS.wire());
        assertEquals("By", ZLinkRuntimeMetrics.Unit.BYTES.wire());
        assertEquals("{request}", ZLinkRuntimeMetrics.Unit.REQUEST.wire());
        assertEquals("{message}", ZLinkRuntimeMetrics.Unit.MESSAGE.wire());
        assertEquals("{failure}", ZLinkRuntimeMetrics.Unit.FAILURE.wire());
        assertEquals("{peer}", ZLinkRuntimeMetrics.Unit.PEER.wire());
        assertEquals("{job}", ZLinkRuntimeMetrics.Unit.JOB.wire());
        assertEquals(
                ZLinkRuntimeMetrics.Unit.SECONDS,
                ZLinkRuntimeMetrics.Metric.REQUEST_DURATION.unit());
        assertEquals(
                ZLinkRuntimeMetrics.Unit.REQUEST,
                ZLinkRuntimeMetrics.Metric.REQUEST_TIMEOUTS.unit());
        assertEquals(
                ZLinkRuntimeMetrics.Unit.MESSAGE,
                ZLinkRuntimeMetrics.Metric.MESSAGES_DROPPED.unit());
        assertEquals(
                ZLinkRuntimeMetrics.Unit.FAILURE,
                ZLinkRuntimeMetrics.Metric.CHANNEL_SELECTION_FAILURES.unit());
    }

    @Test
    void pressureStatesHaveStableWireValues() {
        assertEquals("running", ZLinkApplicationJobQueuePressureState.RUNNING.wire());
        assertEquals("paused", ZLinkApplicationJobQueuePressureState.PAUSED.wire());
    }

    @Test
    void channelSelectionFailuresHaveStableWireValues() {
        assertEquals(
                "no_member", ZLinkServiceTopologyRegistry.ChannelSelectionFailure.NO_MEMBER.wire());
        assertEquals(
                "not_ready", ZLinkServiceTopologyRegistry.ChannelSelectionFailure.NOT_READY.wire());
        assertEquals(
                "draining", ZLinkServiceTopologyRegistry.ChannelSelectionFailure.DRAINING.wire());
    }
}
