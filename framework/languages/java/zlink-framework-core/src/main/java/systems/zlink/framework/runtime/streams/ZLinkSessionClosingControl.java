package systems.zlink.framework.runtime.streams;

import java.nio.ByteBuffer;
import java.nio.charset.StandardCharsets;

final class ZLinkSessionClosingControl {
    static final String NAME = "session-closing";
    static final int VERSION = 1;
    static final int SERVER_DRAIN = 4;
    static final int IDLE_TIMEOUT = 2;
    static final int HEARTBEAT_TIMEOUT = 3;
    static final int PROTOCOL_ERROR = 5;
    private static final int MAX_DIAGNOSTIC_BYTES = 512;

    private ZLinkSessionClosingControl() {
    }

    static byte[] serverDrain(String diagnostic) {
        return encode(SERVER_DRAIN, diagnostic);
    }

    static byte[] encode(int reason, String diagnostic) {
        if (reason < 1 || reason > 6) {
            throw new IllegalArgumentException("session-closing reason is invalid");
        }
        byte[] text = diagnostic == null
            ? new byte[0]
            : diagnostic.getBytes(StandardCharsets.UTF_8);
        if (text.length > MAX_DIAGNOSTIC_BYTES) {
            throw new IllegalArgumentException("session-closing diagnostic exceeds 512 bytes");
        }
        return ByteBuffer.allocate(4 + text.length)
            .put((byte) VERSION)
            .put((byte) reason)
            .putShort((short) text.length)
            .put(text)
            .array();
    }
}
