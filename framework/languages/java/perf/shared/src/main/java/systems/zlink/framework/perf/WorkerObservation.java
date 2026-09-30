package systems.zlink.framework.perf;

// The public worker callback returns this in-process; it is never serialized by the Framework, so the 32-bit
// checksum stays a plain long and is written into the original as a JSON integer (§15.2).
public record WorkerObservation(String startedTicks, String endedTicks, String clockDomainId, String iterations, long checksum) {}
