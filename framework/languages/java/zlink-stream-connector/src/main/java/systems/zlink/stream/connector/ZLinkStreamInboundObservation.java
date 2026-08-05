package systems.zlink.stream.connector;

import java.time.Instant;
import java.util.Arrays;
import java.util.Map;

public record ZLinkStreamInboundObservation(
    ZLinkStreamMessageKind kind,
    String packetName,
    ZLinkStreamCodec codec,
    Long requestSeq,
    Map<String, String> metadata,
    int payloadLength,
    boolean compressed,
    Instant receivedAt,
    byte[] payloadPreview) {
    public ZLinkStreamInboundObservation {
        metadata = Map.copyOf(metadata);
        payloadPreview = payloadPreview == null
            ? new byte[0]
            : Arrays.copyOf(payloadPreview, payloadPreview.length);
    }

    @Override
    public byte[] payloadPreview() {
        return Arrays.copyOf(payloadPreview, payloadPreview.length);
    }
}
