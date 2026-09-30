package systems.zlink.framework.perf;

public record PerfTriggerReply(
        String runId,
        String cellId,
        String resetSeq,
        String phase,
        boolean accepted,
        String state,
        String configHash,
        String reason) {

    public PerfTriggerReply withState(String newState) {
        return new PerfTriggerReply(runId, cellId, resetSeq, phase, accepted, newState, configHash, reason);
    }
}
