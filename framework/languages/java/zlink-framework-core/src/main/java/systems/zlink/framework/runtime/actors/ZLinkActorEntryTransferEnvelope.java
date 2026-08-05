package systems.zlink.framework.runtime.actors;

import java.nio.ByteBuffer;
import java.util.ArrayList;
import java.util.List;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.framework.errors.ZLinkConfigurationException;

/** Internal route-mesh envelope for the multipart actor transfer protocol. */
public final class ZLinkActorEntryTransferEnvelope {
    public static final String PACKET_NAME = "__zlink.actor.transferEntrySpot";

    private ZLinkActorEntryTransferEnvelope() {
    }

    public static Message encode(List<Message> parts) {
        int size = Integer.BYTES;
        for (Message part : parts) {
            size = Math.addExact(size, Math.addExact(Integer.BYTES, part.size()));
        }
        ByteBuffer bytes = ByteBuffer.allocate(size).putInt(parts.size());
        for (Message part : parts) {
            byte[] value = part.toByteArray();
            bytes.putInt(value.length).put(value);
        }
        return Message.from(bytes.array());
    }

    public static List<Message> decode(Message envelope) {
        ByteBuffer bytes = ByteBuffer.wrap(envelope.toByteArray());
        if (bytes.remaining() < Integer.BYTES) {
            throw invalid();
        }
        int count = bytes.getInt();
        if (count < 0 || count > 1_000_000) {
            throw invalid();
        }
        List<Message> parts = new ArrayList<>(count);
        try {
            for (int index = 0; index < count; index++) {
                if (bytes.remaining() < Integer.BYTES) {
                    throw invalid();
                }
                int length = bytes.getInt();
                if (length < 0 || length > bytes.remaining()) {
                    throw invalid();
                }
                byte[] value = new byte[length];
                bytes.get(value);
                parts.add(Message.from(value));
            }
            if (bytes.hasRemaining()) {
                throw invalid();
            }
            return List.copyOf(parts);
        } catch (RuntimeException error) {
            parts.forEach(Message::close);
            throw error;
        }
    }

    private static ZLinkConfigurationException invalid() {
        return new ZLinkConfigurationException("invalid Entry Spot actor transfer envelope");
    }
}
