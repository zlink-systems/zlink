package systems.zlink.framework.runtime.internal.relocation;

import java.util.Optional;
import systems.zlink.framework.actors.ZLinkActorJoinOperationId;

/** Private admission-field codec for a deferred canonical Actor Join. */
public final class ZLinkActorJoinAdmissionProfileCodec {
    private static final String PREFIX = "__zlink.canonical-join-v1:";

    private ZLinkActorJoinAdmissionProfileCodec() {
    }

    public static String encode(ZLinkActorJoinOperationId operationId) {
        if (operationId == null) {
            return "";
        }
        return PREFIX
            + Long.toUnsignedString(operationId.high(), 16)
            + ":"
            + Long.toUnsignedString(operationId.low(), 16);
    }

    public static Optional<ZLinkActorJoinOperationId> decode(String encoded) {
        if (encoded == null || !encoded.startsWith(PREFIX)) {
            return Optional.empty();
        }
        String[] fields = encoded.substring(PREFIX.length()).split(":", -1);
        if (fields.length != 2 || fields[0].isBlank() || fields[1].isBlank()) {
            throw new IllegalArgumentException(
                "canonical Actor Join admission profile is invalid");
        }
        return Optional.of(new ZLinkActorJoinOperationId(
            Long.parseUnsignedLong(fields[0], 16),
            Long.parseUnsignedLong(fields[1], 16)));
    }
}
