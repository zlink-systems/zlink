package systems.zlink.framework.perf;

import java.util.Base64;

// §15.2: the logical byte b[i] = (31*i + 17*floor(i/251) + 29) mod 256, carried as canonical padded Base64.
public final class PayloadPattern {
    private final String base64;

    public PayloadPattern(int size) {
        this.base64 = generate(size);
    }

    public String base64() {
        return base64;
    }

    private static String generate(int length) {
        byte[] bytes = new byte[length];
        for (int i = 0; i < length; i++) {
            bytes[i] = expected(i);
        }
        return Base64.getEncoder().encodeToString(bytes);
    }

    private static byte expected(int index) {
        return (byte) ((31 * index + 17 * (index / 251) + 29) & 0xFF);
    }

    /** The text must equal the canonical Base64 of the pattern, which fixes its length and every byte. */
    public void validate(String payload) {
        if (payload == null || !payload.equals(base64)) {
            throw new PerfValidationException("PayloadMismatch", "Payload is not the canonical Base64 pattern.");
        }
    }

    public static void validateIdentity(PerfEchoRequest request, PerfEchoReply reply) {
        if (!request.runId().equals(reply.runId()) || !request.cellId().equals(reply.cellId())
                || !request.resetSeq().equals(reply.resetSeq()) || !request.phase().equals(reply.phase())
                || request.clientId() != reply.clientId() || !request.sequence().equals(reply.sequence())
                || !request.correlationId().equals(reply.correlationId())
                || reply.clockDomainId() == null || reply.clockDomainId().isEmpty()) {
            throw new PerfValidationException("IdentityMismatch", "Echo identity differs from the submitted operation.");
        }
        DecimalText.i64(reply.receivedTicks());
    }

    public static PerfEchoReply reply(PerfEchoRequest request, long receivedTicks) {
        return new PerfEchoReply(request.runId(), request.cellId(), request.resetSeq(), request.phase(),
                request.clientId(), request.sequence(), request.correlationId(), DecimalText.of(receivedTicks),
                PerfClock.DOMAIN, request.payload());
    }
}
