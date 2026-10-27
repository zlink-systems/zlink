package systems.zlink.framework.runtime.internal.messaging;

import systems.zlink.framework.ZLinkEncodedPayload;

import java.util.Objects;

/** Internal ownership bridge; the public payload API continues to copy caller arrays. */
public final class ZLinkEncodedPayloadAccess {
    private static volatile PayloadAccess access;

    private ZLinkEncodedPayloadAccess() {}

    /** Implemented by the payload class, which owns its constructor and storage. */
    public interface PayloadAccess {
        ZLinkEncodedPayload fromOwned(byte[] bytes);

        byte[] borrow(ZLinkEncodedPayload payload);
    }

    /** Registers the payload implementation once during its class initialization. */
    public static synchronized void registerAccess(PayloadAccess implementation) {
        if (access != null) {
            throw new IllegalStateException("Payload access is already registered");
        }
        access = Objects.requireNonNull(implementation, "implementation");
    }

    private static PayloadAccess access() {
        PayloadAccess selected = access;
        if (selected == null) {
            // An active public factory call initializes the owning class without reflection.
            ZLinkEncodedPayload.from(new byte[0]);
            selected = access;
        }
        return selected;
    }

    /** Adopts a Framework-owned array. The caller must not modify it afterward. */
    public static ZLinkEncodedPayload fromOwned(byte[] bytes) {
        return access().fromOwned(Objects.requireNonNull(bytes, "bytes"));
    }

    /** Borrows storage for internal reads only. Do not mutate or expose it to application code. */
    public static byte[] borrow(ZLinkEncodedPayload payload) {
        return access().borrow(Objects.requireNonNull(payload, "payload"));
    }
}
