package systems.zlink.perf;

import java.util.Arrays;
import java.util.Base64;

public final class Contracts {
    private Contracts() {}
    private static final java.util.Map<Integer,byte[]> PATTERNS=java.util.Map.of(64,bytes(64),4096,bytes(4096));
    private static final java.util.Map<Integer,String> PAYLOADS=java.util.Map.of(64,Base64.getEncoder().encodeToString(PATTERNS.get(64)),4096,Base64.getEncoder().encodeToString(PATTERNS.get(4096)));
    public record PerfEchoRequest(String runId, String cellId, String resetSeq, String phase,
        int clientId, String sequence, String correlationId, String scheduledTicks, String sentTicks,
        String clockDomainId, String returnSpotId, String returnChannel, String payload) {}
    public record PerfEchoReply(String runId, String cellId, String resetSeq, String phase,
        int clientId, String sequence, String correlationId, String receivedTicks, String clockDomainId, String payload) {}
    public record PerfDriveRequest(PerfEchoRequest echo) {}
    public record PerfDriveReply(boolean started, PerfEchoReply echo) {}
    public record PerfPublishEvent(String runId, String cellId, String resetSeq, String phase,
        String sequence, String scheduledTicks, String topic, String sentTicks, String clockDomainId, String payload) {}
    public record PerfCreateRequest(String value) {}
    public record PerfBindRequest(String actorId) {}
    public record PerfBindReply(String actorId) {}
    public record WorkerObservation(String startedTicks, String endedTicks, String clockDomainId, String iterations, long checksum) {}
    public static byte[] bytes(int length) {
        byte[] value = new byte[length];
        for (int i=0; i<length; i++) value[i]=(byte)((31*i + 17*(i/251) + 29)%256);
        return value;
    }
    public static String payload(int length) { String cached=PAYLOADS.get(length);return cached!=null?cached:Base64.getEncoder().encodeToString(bytes(length)); }
    public static void validatePayload(String value, int length) {
        byte[] decoded=Base64.getDecoder().decode(value);
        if (!Arrays.equals(decoded,PATTERNS.containsKey(length)?PATTERNS.get(length):bytes(length)) || !payload(length).equals(value))
            throw new Validation("PayloadMismatch", "Payload length, full pattern or canonical Base64 differs");
    }
    public static final class Validation extends RuntimeException {
        public final String kind;
        public Validation(String kind,String message) { super(message); this.kind=kind; }
    }
}
