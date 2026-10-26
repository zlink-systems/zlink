package systems.zlink.framework;

import systems.zlink.framework.runtime.internal.messaging.ZLinkEncodedPayloadAccess;

import java.util.Arrays;
import java.util.Objects;

public final class ZLinkEncodedPayload {
    static {
        ZLinkEncodedPayloadAccess.registerAccess(
                new ZLinkEncodedPayloadAccess.PayloadAccess() {
                    @Override
                    public ZLinkEncodedPayload fromOwned(byte[] bytes) {
                        return new ZLinkEncodedPayload(bytes);
                    }

                    @Override
                    public byte[] borrow(ZLinkEncodedPayload payload) {
                        return payload.bytes;
                    }
                });
    }

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
