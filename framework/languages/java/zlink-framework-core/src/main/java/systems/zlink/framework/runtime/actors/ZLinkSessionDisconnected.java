package systems.zlink.framework.runtime.actors;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.errors.ZLinkFrameworkErrorKind;
import systems.zlink.framework.errors.ZLinkFrameworkException;

import java.nio.ByteBuffer;

/** Session-Actor binding §7 disconnect body; transport coordinates are not its identity. */
public record ZLinkSessionDisconnected(
        RoutingId sessionOwnerNodeRid, long sessionOwnerNodeGeneration, long bindingGeneration) {
    private static final int RID_LENGTH_BYTES = Byte.BYTES;
    private static final int GENERATIONS_BYTES = Long.BYTES * 2;

    public byte[] encode() {
        if (sessionOwnerNodeRid == null
                || sessionOwnerNodeRid.size() == 0
                || sessionOwnerNodeGeneration == 0
                || bindingGeneration == 0) {
            throw malformed();
        }
        byte[] rid = sessionOwnerNodeRid.toBytes();
        return ByteBuffer.allocate(RID_LENGTH_BYTES + rid.length + GENERATIONS_BYTES)
                .put((byte) rid.length)
                .put(rid)
                .putLong(sessionOwnerNodeGeneration)
                .putLong(bindingGeneration)
                .array();
    }

    public static ZLinkSessionDisconnected decode(byte[] payload) {
        if (payload.length < RID_LENGTH_BYTES + 1 + GENERATIONS_BYTES) throw malformed();
        int ridLength = Byte.toUnsignedInt(payload[0]);
        if (ridLength == 0 || payload.length != RID_LENGTH_BYTES + ridLength + GENERATIONS_BYTES)
            throw malformed();
        var bytes = ByteBuffer.wrap(payload);
        bytes.get();
        byte[] rid = new byte[ridLength];
        bytes.get(rid);
        long lifecycle = bytes.getLong();
        long binding = bytes.getLong();
        if (lifecycle == 0 || binding == 0) throw malformed();
        return new ZLinkSessionDisconnected(RoutingId.from(rid), lifecycle, binding);
    }

    private static ZLinkFrameworkException malformed() {
        return new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.PROTOCOL_ERROR,
                "Malformed Session disconnect binding identity.");
    }
}
