package systems.zlink.framework.perf;

public record PerfTriggerRequest(String runId, String cellId, String resetSeq, String phase) {}
