package systems.zlink.framework;

import java.util.Arrays;
import java.util.Objects;

public final class ZLinkEncodedPayload {
    private final byte[] bytes;

    private ZLinkEncodedPayload(byte[] bytes) {
        this.bytes = bytes;
    }

    public static ZLinkEncodedPayload from(byte[] bytes) {
        Objects.requireNonNull(bytes, "bytes");
        return new ZLinkEncodedPayload(Arrays.copyOf(bytes, bytes.length));
    }

    public byte[] bytes() {
        return Arrays.copyOf(bytes, bytes.length);
    }

}
