package systems.zlink.framework.perf;

import io.micrometer.core.instrument.FunctionCounter;
import io.micrometer.core.instrument.Gauge;
import io.micrometer.core.instrument.Meter;
import io.micrometer.core.instrument.MeterRegistry;
import io.micrometer.core.instrument.Tag;

import java.util.ArrayList;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;

// Standard Micrometer provider observation, restricted to the documented host capacity instruments the Framework
// starter registers (zlink.host.core_hwm.*, zlink.host.application_job_queue.*). It reads existing gauges and counters at
// collection; it does not synthesize provider metrics (§14, §23).
public final class PublicMetricCollector {
    private final MeterRegistry registry;

    public PublicMetricCollector(MeterRegistry registry) {
        this.registry = registry;
    }

    public List<Object> snapshot() {
        List<Object> observations = new ArrayList<>();
        for (Meter meter : registry.getMeters()) {
            String name = meter.getId().getName();
            if (!(name.startsWith("zlink.host.core_hwm.") || name.startsWith("zlink.host.application_job_queue."))) {
                continue;
            }
            Number value;
            String kind;
            if (meter instanceof Gauge gauge) {
                value = gauge.value();
                kind = "observable";
            } else if (meter instanceof FunctionCounter counter) {
                value = counter.count();
                kind = "counter";
            } else {
                continue;
            }
            if (value instanceof Double number && !Double.isFinite(number)) {
                throw new IllegalStateException("Provider returned a non-finite public metric.");
            }
            Map<String, Object> labels = new LinkedHashMap<>();
            for (Tag tag : meter.getId().getTags()) {
                labels.put(tag.getKey(), tag.getValue());
            }
            Map<String, Object> observation = new LinkedHashMap<>();
            observation.put("name", name);
            observation.put("kind", kind);
            observation.put("unit", meter.getId().getBaseUnit());
            observation.put("labels", labels);
            observation.put("value", value);
            observation.put("meter", "micrometer");
            observations.add(observation);
        }
        if (observations.isEmpty()) {
            throw new IllegalStateException("No public host capacity metrics were collected.");
        }
        return observations;
    }
}
