package systems.zlink.framework.perf;

// Perf spec §12/§15.2. Decimal strings are application fields, not a message codec; the Framework's default typed JSON
// serializer carries this record as it is (lowerCamelCase property names, no packet-specific codec).
public record PerfEchoRequest(
        String runId,
        String cellId,
        String resetSeq,
        String phase,
        int clientId,
        String sequence,
        String correlationId,
        String sentTicks,
        String clockDomainId,
        String returnSpotId,
        String returnChannel,
        String payload) {

    public PerfEchoRequest withSentTicks(long ticks) {
        return new PerfEchoRequest(runId, cellId, resetSeq, phase, clientId, sequence, correlationId,
                DecimalText.of(ticks), clockDomainId, returnSpotId, returnChannel, payload);
    }

    public PerfEchoRequest withReturnChannel(String channel) {
        return new PerfEchoRequest(runId, cellId, resetSeq, phase, clientId, sequence, correlationId, sentTicks,
                clockDomainId, returnSpotId, channel, payload);
    }

    public PerfEchoRequest withReturnSpotId(String spotId) {
        return new PerfEchoRequest(runId, cellId, resetSeq, phase, clientId, sequence, correlationId, sentTicks,
                clockDomainId, spotId, returnChannel, payload);
    }
}
