package systems.zlink.framework.runtime.internal.service;

import java.io.ByteArrayOutputStream;
import java.util.List;
import java.util.Objects;
import systems.zlink.framework.runtime.protocol.ServiceWireConstants;

/**
 * Encodes the service prefix defined by service-wire-v1.schema.json.
 *
 * <p>The codec owns validation and copies every frame before it can enter a
 * mailbox. Raw binding buffers therefore never escape the transport turn.
 */
public final class ZLinkServiceWireCodec {
    private static final int KNOWN_FLAGS =
        ServiceWireConstants.FLAG_METADATA
            | ServiceWireConstants.FLAG_BOUND_SESSION
            | ServiceWireConstants.FLAG_SOURCE_SPOT_ID
            | ServiceWireConstants.FLAG_EXTENSION;
    private static final int PREFIX_BYTES = 5;

    public List<byte[]> encode(ZLinkServiceWireFrame record) {
        Objects.requireNonNull(record, "record");
        validateCommandAndFlags(record.command(), record.flags());
        List<byte[]> body = record.frames();
        validateConditionalFrames(record.flags(), body.size());
        validateLivenessBody(record.command(), body);

        byte[] prefix = new byte[] {
            (byte) ServiceWireConstants.MAGIC_0,
            (byte) ServiceWireConstants.MAGIC_1,
            (byte) ServiceWireConstants.WIRE_MAJOR,
            (byte) record.command(),
            (byte) record.flags()
        };
        ByteArrayOutputStream head = new ByteArrayOutputStream();
        head.writeBytes(prefix);
        body.forEach(head::writeBytes);
        return List.of(head.toByteArray());
    }

    public ZLinkServiceWireFrame decode(List<byte[]> multipart) {
        Objects.requireNonNull(multipart, "multipart");
        if (multipart.isEmpty()) {
            throw new ZLinkServiceWireException("service record has no prefix frame");
        }
        if (multipart.size() != 1) {
            throw new ZLinkServiceWireException("service record head must be one frame");
        }
        byte[] head = Objects.requireNonNull(multipart.getFirst(), "head");
        if (head.length < PREFIX_BYTES
            || Byte.toUnsignedInt(head[0]) != ServiceWireConstants.MAGIC_0
            || Byte.toUnsignedInt(head[1]) != ServiceWireConstants.MAGIC_1) {
            throw new ZLinkServiceWireException("invalid service record prefix");
        }
        int major = Byte.toUnsignedInt(head[2]);
        if (major != ServiceWireConstants.WIRE_MAJOR) {
            throw new ZLinkServiceWireException("unsupported service wire major: " + major);
        }
        int command = Byte.toUnsignedInt(head[3]);
        int flags = Byte.toUnsignedInt(head[4]);
        validateCommandAndFlags(command, flags);
        validateConditionalFrames(flags, head.length - PREFIX_BYTES);

        List<byte[]> body = head.length == PREFIX_BYTES
            ? List.of()
            : List.of(java.util.Arrays.copyOfRange(head, PREFIX_BYTES, head.length));
        validateLivenessBody(command, body);
        return new ZLinkServiceWireFrame(command, flags, body);
    }

    private static void validateCommandAndFlags(int command, int flags) {
        if (!isKnownCommand(command)) {
            throw new ZLinkServiceWireException("unknown service command: " + command);
        }
        if ((flags & ~KNOWN_FLAGS) != 0) {
            throw new ZLinkServiceWireException("service record contains an unknown flag");
        }
        if ((command == ServiceWireConstants.COMMAND_LIVENESS_PROBE
            || command == ServiceWireConstants.COMMAND_LIVENESS_ACK) && flags != 0) {
            throw new ZLinkServiceWireException("liveness records do not accept flags");
        }
    }

    private static void validateConditionalFrames(int flags, int bodyFrames) {
        if ((flags & ServiceWireConstants.FLAG_METADATA) != 0 && bodyFrames == 0) {
            throw new ZLinkServiceWireException("metadata flag requires a body frame");
        }
    }

    private static void validateLivenessBody(int command, List<byte[]> body) {
        if (command != ServiceWireConstants.COMMAND_LIVENESS_PROBE
            && command != ServiceWireConstants.COMMAND_LIVENESS_ACK) {
            return;
        }
        if (body.size() != 1 || body.getFirst().length != Long.BYTES) {
            throw new ZLinkServiceWireException("liveness record requires one uint64 probe id");
        }
        long nonzero = 0L;
        for (byte value : body.getFirst()) {
            nonzero |= Byte.toUnsignedInt(value);
        }
        if (nonzero == 0L) {
            throw new ZLinkServiceWireException("liveness probe id must be nonzero");
        }
    }

    private static boolean isKnownCommand(int command) {
        return switch (command) {
            case ServiceWireConstants.COMMAND_HELLO,
                 ServiceWireConstants.COMMAND_ADMIT,
                 ServiceWireConstants.COMMAND_REJECT,
                 ServiceWireConstants.COMMAND_UPDATE,
                 ServiceWireConstants.COMMAND_LIVENESS_PROBE,
                 ServiceWireConstants.COMMAND_LIVENESS_ACK,
                 ServiceWireConstants.COMMAND_NODE_SEND,
                 ServiceWireConstants.COMMAND_NODE_REQUEST,
                 ServiceWireConstants.COMMAND_CHANNEL_SEND,
                 ServiceWireConstants.COMMAND_CHANNEL_REQUEST,
                 ServiceWireConstants.COMMAND_REPLY,
                 ServiceWireConstants.COMMAND_SPOT_SEND,
                 ServiceWireConstants.COMMAND_SPOT_REQUEST,
                 ServiceWireConstants.COMMAND_LOGICAL_MULTICAST,
                 ServiceWireConstants.COMMAND_ACTOR_SEND,
                 ServiceWireConstants.COMMAND_ACTOR_REQUEST,
                 ServiceWireConstants.COMMAND_ACTOR_LOOKUP,
                 ServiceWireConstants.COMMAND_ACTOR_DESTROY,
                 ServiceWireConstants.COMMAND_ACTOR_JOIN,
                 ServiceWireConstants.COMMAND_ACTOR_LEFT,
                 ServiceWireConstants.COMMAND_RELOCATION_READY,
                 ServiceWireConstants.COMMAND_RELOCATION_DATA,
                 ServiceWireConstants.COMMAND_RELOCATION_ACK,
                 ServiceWireConstants.COMMAND_REPLY_RELAY,
                 ServiceWireConstants.COMMAND_RELOCATION_SEAL,
                 ServiceWireConstants.COMMAND_RELOCATION_COMPLETE,
                 ServiceWireConstants.COMMAND_BOUND_SESSION_SEND,
                 ServiceWireConstants.COMMAND_ACTOR_JOINED,
                 ServiceWireConstants.COMMAND_BOUND_SESSION_BIND,
                 ServiceWireConstants.COMMAND_INSTANCE_SPOT,
                 ServiceWireConstants.COMMAND_RELOCATION_PREPARE,
                 ServiceWireConstants.COMMAND_RELOCATION_RESERVED,
                 ServiceWireConstants.COMMAND_SESSION_RELOCATION_SEAL,
                 ServiceWireConstants.COMMAND_SESSION_RELOCATION_SEALED,
                 ServiceWireConstants.COMMAND_SESSION_RELOCATION_ROUTE,
                 ServiceWireConstants.COMMAND_SESSION_RELOCATION_ROUTED,
                 ServiceWireConstants.COMMAND_REPLY_RELAY_ACK,
                 ServiceWireConstants.COMMAND_USER_SPOT_CREATE,
                 ServiceWireConstants.COMMAND_USER_SPOT_CLOSE,
                 ServiceWireConstants.COMMAND_ACTOR_CREATE,
                 ServiceWireConstants.COMMAND_MESSAGE_FOLLOW -> true;
            default -> false;
        };
    }
}
