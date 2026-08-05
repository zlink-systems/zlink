package systems.zlink.stream.connector;

import java.nio.ByteBuffer;
import java.nio.charset.CharacterCodingException;
import java.nio.charset.CodingErrorAction;
import java.nio.charset.StandardCharsets;

final class ZLinkSessionClosingControl {
    static final String NAME = "session-closing";
    static final int VERSION = 1;
    private static final int MAX_DIAGNOSTIC_BYTES = 512;

    private ZLinkSessionClosingControl() {
    }

    static ZLinkStreamCloseReason decode(byte[] payload) {
        if (payload.length < 4) {
            throw new IllegalArgumentException("session-closing payload is too short");
        }
        ByteBuffer buffer = ByteBuffer.wrap(payload);
        int version = Byte.toUnsignedInt(buffer.get());
        int reason = Byte.toUnsignedInt(buffer.get());
        int diagnosticLength = Short.toUnsignedInt(buffer.getShort());
        if (version != VERSION || diagnosticLength > MAX_DIAGNOSTIC_BYTES
            || diagnosticLength != buffer.remaining()) {
            throw new IllegalArgumentException("invalid session-closing control");
        }
        byte[] diagnostic = new byte[diagnosticLength];
        buffer.get(diagnostic);
        try {
            StandardCharsets.UTF_8.newDecoder()
                .onMalformedInput(CodingErrorAction.REPORT)
                .onUnmappableCharacter(CodingErrorAction.REPORT)
                .decode(ByteBuffer.wrap(diagnostic));
        } catch (CharacterCodingException error) {
            throw new IllegalArgumentException("invalid session-closing diagnostic", error);
        }
        return switch (reason) {
            case 1 -> ZLinkStreamCloseReason.CLIENT_CLOSE;
            case 2 -> ZLinkStreamCloseReason.IDLE_TIMEOUT;
            case 3 -> ZLinkStreamCloseReason.HEARTBEAT_TIMEOUT;
            case 4 -> ZLinkStreamCloseReason.SERVER_DRAIN;
            case 5 -> ZLinkStreamCloseReason.PROTOCOL_ERROR;
            case 6 -> ZLinkStreamCloseReason.TRANSPORT_ERROR;
            default -> throw new IllegalArgumentException("unknown session-closing reason");
        };
    }
}
