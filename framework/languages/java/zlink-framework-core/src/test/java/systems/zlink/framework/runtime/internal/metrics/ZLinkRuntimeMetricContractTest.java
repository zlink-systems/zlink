package systems.zlink.framework.runtime.internal.metrics;

import static org.junit.jupiter.api.Assertions.assertEquals;

import com.fasterxml.jackson.databind.JsonNode;
import com.fasterxml.jackson.databind.ObjectMapper;

import org.junit.jupiter.api.Test;

import java.util.HashSet;
import java.util.Set;

final class ZLinkRuntimeMetricContractTest {
    @Test
    void currentPopulationSurvivesLateAttachmentAndReplacementOfAnOwner() throws Exception {
        var tags = java.util.Map.of("mesh_name", "late-population");
        try (var first = new ZLinkPopulationMetrics(ZLinkRuntimeMetrics.Metric.ACTOR_COUNT, tags)) {
            first.add(2);
            try (var metrics = ZLinkRuntimeMetricProbe.install()) {
                assertEquals(2, metrics.observable("zlink.actor.count", tags));
                try (var second =
                        new ZLinkPopulationMetrics(ZLinkRuntimeMetrics.Metric.ACTOR_COUNT, tags)) {
                    second.add(3);
                    assertEquals(5, metrics.observable("zlink.actor.count", tags));
                    first.add(-1);
                    assertEquals(4, metrics.observable("zlink.actor.count", tags));
                }
                assertEquals(1, metrics.observable("zlink.actor.count", tags));
            }
        }
    }

    @Test
    void catchesAnOutOfContractLabelOnAnActualEmission() throws Exception {
        try (var metrics = ZLinkRuntimeMetricProbe.install()) {
            ZLinkRuntimeMetrics.increment(
                    "zlink.relocation.started", java.util.Map.of("actor_id", "forbidden"));
            org.junit.jupiter.api.Assertions.assertThrows(
                    AssertionError.class, metrics::assertContractLabels);
        }
    }

    @Test
    void instrumentsMatchSharedSpecificationFixture() throws Exception {
        JsonNode fixture;
        try (var input = getClass().getResourceAsStream("/runtime-metrics-v1.json")) {
            org.junit.jupiter.api.Assertions.assertNotNull(input);
            fixture = new ObjectMapper().readTree(input);
        }
        Set<String> expected = new HashSet<>();
        fixture.path("metrics").forEach(metric -> expected.add(metric.path("name").asText()));
        Set<String> actual = new HashSet<>();
        for (var metric : ZLinkRuntimeMetrics.Metric.values()) actual.add(metric.metricName());
        assertEquals(expected, actual);
        for (JsonNode metric : fixture.path("metrics")) {
            var instrument = ZLinkRuntimeMetrics.Metric.decode(metric.path("name").asText());
            assertEquals(
                    metric.path("kind").asText(),
                    instrument.kind().name().toLowerCase(java.util.Locale.ROOT));
            assertEquals(metric.path("unit").asText(), instrument.unit().wire());
            java.util.List<String> labels = new java.util.ArrayList<>();
            metric.path("labels").forEach(label -> labels.add(label.asText()));
            assertEquals(labels, instrument.labels(), instrument.metricName());
        }
    }
}
