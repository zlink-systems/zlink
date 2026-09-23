package systems.zlink.framework.runtime.streams;

import java.nio.ByteBuffer;
import java.nio.charset.StandardCharsets;

public final class ZLinkStreamActorControl {
    public static final String BOUND = "$zlink.actor.bound";
    public static final String UNBOUND = "$zlink.actor.unbound";
    private static final int VERSION = 1;

    private ZLinkStreamActorControl() {}

    public static byte[] bound(int actorSlot, String actorId) {
        byte[] id = actorId.getBytes(StandardCharsets.UTF_8);
        if (actorSlot <= 0 || actorSlot > 0xffff || id.length == 0 || id.length > 255) {
            throw new IllegalArgumentException("STREAM Actor binding control is invalid");
        }
        return ByteBuffer.allocate(1 + Short.BYTES + 1 + id.length)
                .put((byte) VERSION)
                .putShort((short) actorSlot)
                .put((byte) id.length)
                .put(id)
                .array();
    }

    public static byte[] unbound(int actorSlot) {
        if (actorSlot <= 0 || actorSlot > 0xffff) {
            throw new IllegalArgumentException("STREAM Actor slot is invalid");
        }
        return ByteBuffer.allocate(1 + Short.BYTES)
                .put((byte) VERSION)
                .putShort((short) actorSlot)
                .array();
    }
}
