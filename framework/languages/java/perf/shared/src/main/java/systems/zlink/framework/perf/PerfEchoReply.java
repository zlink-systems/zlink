package systems.zlink.framework.perf;

public record PerfEchoReply(
        String runId,
        String cellId,
        String resetSeq,
        String phase,
        int clientId,
        String sequence,
        String correlationId,
        String receivedTicks,
        String clockDomainId,
        String payload) {}
