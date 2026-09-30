package systems.zlink.framework.perf;

import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;

// §15.4 PerfMetricsSnapshot: what /perf/stats returns and what a client stores as its original. The maps are the
// document itself; a scenario adds its family metrics to them through Measurement's enrich hook.
public final class PerfSnapshot {
    public final String runId;
    public final String cellId;
    public final String resetSeq;
    public final String role;
    public final int roleInstance;
    public final String configHash;
    public final String phase;
    public final Map<String, Object> window;
    public final Map<String, Object> clock;
    public final List<Map<String, Object>> serializedMessageBytes;
    public final Map<String, Object> metrics;
    public final Map<String, Object> histograms;
    public final Map<String, NullReason> nullReasons;
    public Object publicStatus;
    public List<Object> publicMetrics = List.of();
    public final Map<String, Object> runtimeMetrics;
    public final Map<String, Object> provenance;

    PerfSnapshot(String runId, String cellId, String resetSeq, String role, int roleInstance, String configHash,
            String phase, Map<String, Object> window, Map<String, Object> clock,
            List<Map<String, Object>> serializedMessageBytes, Map<String, Object> metrics,
            Map<String, Object> histograms, Map<String, NullReason> nullReasons, Object publicStatus,
            Map<String, Object> runtimeMetrics, Map<String, Object> provenance) {
        this.runId = runId;
        this.cellId = cellId;
        this.resetSeq = resetSeq;
        this.role = role;
        this.roleInstance = roleInstance;
        this.configHash = configHash;
        this.phase = phase;
        this.window = window;
        this.clock = clock;
        this.serializedMessageBytes = serializedMessageBytes;
        this.metrics = metrics;
        this.histograms = histograms;
        this.nullReasons = nullReasons;
        this.publicStatus = publicStatus;
        this.runtimeMetrics = runtimeMetrics;
        this.provenance = provenance;
    }

    /** The JSON document; the key set is the .NET original's. */
    public Map<String, Object> toMap() {
        Map<String, Object> document = new LinkedHashMap<>();
        document.put("schemaVersion", 2);
        document.put("runId", runId);
        document.put("cellId", cellId);
        document.put("resetSeq", resetSeq);
        document.put("language", "java");
        document.put("role", role);
        document.put("roleInstance", roleInstance);
        document.put("configHash", configHash);
        document.put("phase", phase);
        document.put("window", window);
        document.put("clock", clock);
        document.put("serializedMessageBytes", serializedMessageBytes);
        document.put("metrics", metrics);
        document.put("histograms", histograms);
        document.put("nullReasons", nullReasons);
        document.put("publicStatus", publicStatus);
        document.put("publicMetrics", publicMetrics);
        document.put("runtimeMetrics", runtimeMetrics);
        document.put("provenance", provenance);
        return document;
    }
}
