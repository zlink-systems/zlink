package systems.zlink.framework.runtime.internal.locations;

import java.util.Objects;

public record ZLinkPendingObjectCreation(
    String reservationId,
    String requestContentReference,
    byte[] requestSha256,
    int requestEncodedSize) {
    public ZLinkPendingObjectCreation {
        if (reservationId == null || reservationId.isBlank()) {
            throw new IllegalArgumentException(
                "reservationId must not be blank");
        }
        if (requestContentReference == null
            || requestContentReference.isBlank()) {
            throw new IllegalArgumentException(
                "requestContentReference must not be blank");
        }
        requestSha256 = Objects.requireNonNull(
            requestSha256, "requestSha256").clone();
        if (requestSha256.length != 32) {
            throw new IllegalArgumentException(
                "requestSha256 must contain exactly 32 bytes");
        }
        if (requestEncodedSize < 0
            || requestEncodedSize > 1024 * 1024) {
            throw new IllegalArgumentException(
                "requestEncodedSize must be in 0..1048576");
        }
    }

    @Override
    public byte[] requestSha256() {
        return requestSha256.clone();
    }
}
