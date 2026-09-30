package systems.zlink.framework.perf;

public record PerfPublishEvent(
        String runId,
        String cellId,
        String resetSeq,
        String phase,
        String sequence,
        String topic,
        String sentTicks,
        String clockDomainId,
        String payload) {}
