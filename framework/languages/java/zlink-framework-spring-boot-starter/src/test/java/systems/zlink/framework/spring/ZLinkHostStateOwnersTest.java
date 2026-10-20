package systems.zlink.framework.spring;

import static org.junit.jupiter.api.Assertions.assertEquals;

import io.micrometer.core.instrument.simple.SimpleMeterRegistry;

import org.junit.jupiter.api.Test;

import systems.zlink.framework.runtime.internal.metrics.ZLinkRuntimeMetrics;

import java.util.concurrent.atomic.AtomicReference;

final class ZLinkHostStateOwnersTest {
    @Test
    void hostStateExportReadsOneSnapshotForAllStateSeries() {
        var registry = new io.micrometer.core.instrument.simple.SimpleMeterRegistry();
        try {
            var sink = new ZLinkMicrometerMetricSink(registry);
            var calls = new java.util.concurrent.atomic.AtomicInteger();
            sink.observeHostStates(
                    () -> {
                        calls.incrementAndGet();
                        return java.util.Map.of("stopped", 1L);
                    });
            assertEquals(1, calls.get());
            assertEquals(
                    1d, registry.get("zlink.host.state").tag("state", "stopped").gauge().value());
        } finally {
            registry.close();
        }
    }

    @Test
    void hostStatesAggregateLiveOwnersAndRetireExactly() throws Exception {
        var registry = new SimpleMeterRegistry();
        var firstState = new AtomicReference<>("preparing");
        var secondState = new AtomicReference<>("serving");
        try (var provider = ZLinkRuntimeMetrics.install(new ZLinkMicrometerMetricSink(registry));
                var first = ZLinkRuntimeMetrics.registerHostState(firstState::get);
                var second = ZLinkRuntimeMetrics.registerHostState(secondState::get)) {
            assertEquals(
                    1d, registry.get("zlink.host.state").tag("state", "preparing").gauge().value());
            assertEquals(
                    1d, registry.get("zlink.host.state").tag("state", "serving").gauge().value());
            firstState.set("serving");
            ZLinkRuntimeMetrics.publishHostState();
            assertEquals(1, registry.find("zlink.host.state").gauges().size());
            assertEquals(
                    2d, registry.get("zlink.host.state").tag("state", "serving").gauge().value());
            first.close();
            assertEquals(
                    1d, registry.get("zlink.host.state").tag("state", "serving").gauge().value());
            secondState.set("stopped");
            ZLinkRuntimeMetrics.publishHostState();
            assertEquals(1, registry.find("zlink.host.state").gauges().size());
            assertEquals(
                    1d, registry.get("zlink.host.state").tag("state", "stopped").gauge().value());
        } finally {
            registry.close();
        }
    }
}
