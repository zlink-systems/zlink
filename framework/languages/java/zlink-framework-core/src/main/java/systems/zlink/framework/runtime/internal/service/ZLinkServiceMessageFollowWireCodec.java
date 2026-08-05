package systems.zlink.framework.runtime.internal.service;

import java.io.ByteArrayOutputStream;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.charset.CharacterCodingException;
import java.nio.charset.CodingErrorAction;
import java.nio.charset.StandardCharsets;
import java.util.Objects;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.runtime.protocol.ServiceWireConstants;

/**
 * Encodes the closed service-wire command 50 Message Follow notice.
 *
 * <p>The record is infrastructure-only. Its route fences are decoded before
 * any application payload can be admitted, and the length-delimited version
 * is checked for exact consumption so a newer or malformed tail cannot be
 * interpreted as a valid route.
 */
public final class ZLinkServiceMessageFollowWireCodec {
    public static final int MAX_HOP_COUNT = 8;
    public static final long MAX_QUEUED_MESSAGES = 1024;
    public static final long MAX_QUEUED_BYTES = 16L * 1024L * 1024L;
    private static final int PREFIX_BYTES = 5;
    private static final int VERSION_BYTES = 1;
    private static final int BODY_LENGTH_BYTES = 4;
    private static final int MAX_ROUTE_BODY_BYTES = 0xffff;

    public byte[] encode(Notice notice) {
        Objects.requireNonNull(notice, "notice");
        ByteArrayOutputStream body = new ByteArrayOutputStream();
        writeRoute(body, notice.source());
        writeRoute(body, notice.target());
        writeU8(body, notice.hopCount(), "hopCount");
        writeU32(body, notice.queuedMessages(), "queuedMessages");
        writeU32(body, notice.queuedBytes(), "queuedBytes");
        writeBits64(body, notice.originalOperationHigh());
        writeBits64(body, notice.originalOperationLow());
        writeBits64(body, notice.originalReplyRouteId());

        byte[] bodyBytes = body.toByteArray();
        if (bodyBytes.length > MAX_QUEUED_BYTES) {
            throw protocol("Message Follow notice exceeds its encoded byte bound");
        }
        ByteArrayOutputStream result = new ByteArrayOutputStream(
            PREFIX_BYTES + VERSION_BYTES + BODY_LENGTH_BYTES + bodyBytes.length);
        result.write(ServiceWireConstants.MAGIC_0);
        result.write(ServiceWireConstants.MAGIC_1);
        result.write(ServiceWireConstants.WIRE_MAJOR);
        result.write(ServiceWireConstants.COMMAND_MESSAGE_FOLLOW);
        result.write(0);
        result.write(1);
        writeU32(result, bodyBytes.length, "bodyLength");
        result.writeBytes(bodyBytes);
        return result.toByteArray();
    }

    public Notice decode(byte[] encoded) {
        Objects.requireNonNull(encoded, "encoded");
        Reader reader = new Reader(encoded);
        if (reader.u8("magic0") != ServiceWireConstants.MAGIC_0
            || reader.u8("magic1") != ServiceWireConstants.MAGIC_1
            || reader.u8("major") != ServiceWireConstants.WIRE_MAJOR
            || reader.u8("command") != ServiceWireConstants.COMMAND_MESSAGE_FOLLOW
            || reader.u8("flags") != 0) {
            throw protocol("record is not a Message Follow notice");
        }
        if (reader.u8("version") != 1) {
            throw protocol("Message Follow notice version must be one");
        }
        long bodyLength = reader.u32("bodyLength");
        if (bodyLength > MAX_QUEUED_BYTES || bodyLength != reader.remaining()) {
            throw protocol("Message Follow notice has an invalid body length");
        }
        Reader body = reader.slice((int) bodyLength);
        Route source = readRoute(body);
        Route target = readRoute(body);
        int hopCount = body.u8("hopCount");
        long queuedMessages = body.u32("queuedMessages");
        long queuedBytes = body.u32("queuedBytes");
        long operationHigh = body.bits64("originalOperation.high");
        long operationLow = body.bits64("originalOperation.low");
        long replyRouteId = body.bits64("originalReplyRouteId");
        body.end();
        reader.end();
        return new Notice(
            source,
            target,
            hopCount,
            queuedMessages,
            queuedBytes,
            operationHigh,
            operationLow,
            replyRouteId);
    }

    private static void writeRoute(ByteArrayOutputStream output, Route route) {
        Objects.requireNonNull(route, "route");
        ByteArrayOutputStream body = new ByteArrayOutputStream();
        if (route instanceof ActorRoute actor) {
            output.write(1);
            writeText8(body, actor.actorId(), "actorId");
            writeNonzero(body, actor.objectGeneration(), "objectGeneration");
            writeRid(body, actor.targetNodeRid(), "targetNodeRid");
            writeNonzero(
                body, actor.targetNodeGeneration(), "targetNodeGeneration");
            writeNonzero(
                body,
                actor.authorityOwnerGeneration(),
                "authorityOwnerGeneration");
            writeNonzero(
                body, actor.ownerLeaseGeneration(), "ownerLeaseGeneration");
        } else if (route instanceof SpotRoute spot) {
            output.write(2);
            writeText8(body, spot.spotId(), "spotId");
            writeNonzero(body, spot.objectGeneration(), "objectGeneration");
            writeRid(body, spot.targetNodeRid(), "targetNodeRid");
            writeNonzero(
                body, spot.targetNodeGeneration(), "targetNodeGeneration");
            writeNonzero(
                body,
                spot.authorityOwnerGeneration(),
                "authorityOwnerGeneration");
            writeNonzero(
                body, spot.ownerLeaseGeneration(), "ownerLeaseGeneration");
        } else {
            throw protocol("unknown Message Follow route type");
        }
        byte[] bytes = body.toByteArray();
        if (bytes.length > MAX_ROUTE_BODY_BYTES) {
            throw protocol("Message Follow route body exceeds u16 length");
        }
        writeU16(output, bytes.length, "routeLength");
        output.writeBytes(bytes);
    }

    private static Route readRoute(Reader reader) {
        int kind = reader.u8("routeKind");
        int length = reader.u16("routeLength");
        Reader body = reader.slice(length);
        Route route;
        if (kind == 1) {
            route = new ActorRoute(
                body.text8("actorId"),
                body.nonzeroU64("objectGeneration"),
                body.rid("targetNodeRid"),
                body.nonzeroU64("targetNodeGeneration"),
                body.nonzeroU64("authorityOwnerGeneration"),
                body.nonzeroU64("ownerLeaseGeneration"));
        } else if (kind == 2) {
            route = new SpotRoute(
                body.text8("spotId"),
                body.nonzeroU64("objectGeneration"),
                body.rid("targetNodeRid"),
                body.nonzeroU64("targetNodeGeneration"),
                body.nonzeroU64("authorityOwnerGeneration"),
                body.nonzeroU64("ownerLeaseGeneration"));
        } else {
            throw protocol("unknown Message Follow route kind: " + kind);
        }
        body.end();
        return route;
    }

    private static void writeU8(ByteArrayOutputStream output, long value, String field) {
        if (value < 0 || value > 0xff) {
            throw protocol(field + " exceeds u8");
        }
        output.write((int) value);
    }

    private static void writeU16(ByteArrayOutputStream output, long value, String field) {
        if (value < 0 || value > 0xffff) {
            throw protocol(field + " exceeds u16");
        }
        output.write((int) (value >>> 8));
        output.write((int) value);
    }

    private static void writeU32(ByteArrayOutputStream output, long value, String field) {
        if (value < 0 || value > 0xffff_ffffL) {
            throw protocol(field + " exceeds u32");
        }
        output.writeBytes(ByteBuffer.allocate(Integer.BYTES)
            .order(ByteOrder.BIG_ENDIAN)
            .putInt((int) value)
            .array());
    }

    private static void writeBits64(ByteArrayOutputStream output, long value) {
        output.writeBytes(ByteBuffer.allocate(Long.BYTES)
            .order(ByteOrder.BIG_ENDIAN)
            .putLong(value)
            .array());
    }

    private static void writeNonzero(
        ByteArrayOutputStream output, long value, String field) {
        if (value <= 0) {
            throw protocol(field + " must be nonzero");
        }
        writeBits64(output, value);
    }

    private static void writeRid(
        ByteArrayOutputStream output, RoutingId value, String field) {
        byte[] bytes = Objects.requireNonNull(value, field).toBytes();
        if (bytes.length == 0 || bytes.length > 0xff) {
            throw protocol(field + " must contain 1..255 bytes");
        }
        output.write(bytes.length);
        output.writeBytes(bytes);
    }

    private static void writeText8(
        ByteArrayOutputStream output, String value, String field) {
        byte[] bytes = Objects.requireNonNull(value, field)
            .getBytes(StandardCharsets.UTF_8);
        if (bytes.length == 0
            || bytes.length > 0xff
            || value.indexOf('\0') >= 0) {
            throw protocol(field + " exceeds text8");
        }
        output.write(bytes.length);
        output.writeBytes(bytes);
    }

    private static ZLinkServiceWireException protocol(String message) {
        return new ZLinkServiceWireException(message);
    }

    public sealed interface Route permits ActorRoute, SpotRoute {
        RoutingId targetNodeRid();
        long objectGeneration();
        long targetNodeGeneration();
        long authorityOwnerGeneration();
        long ownerLeaseGeneration();
    }

    public record ActorRoute(
        String actorId,
        long objectGeneration,
        RoutingId targetNodeRid,
        long targetNodeGeneration,
        long authorityOwnerGeneration,
        long ownerLeaseGeneration) implements Route {
        public ActorRoute {
            if (actorId == null || actorId.isBlank()) {
                throw protocol("actorId must not be blank");
            }
            Objects.requireNonNull(targetNodeRid, "targetNodeRid");
            if (objectGeneration <= 0
                || targetNodeGeneration <= 0
                || authorityOwnerGeneration <= 0
                || ownerLeaseGeneration <= 0) {
                throw protocol("Message Follow route generations must be nonzero");
            }
        }
    }

    public record SpotRoute(
        String spotId,
        long objectGeneration,
        RoutingId targetNodeRid,
        long targetNodeGeneration,
        long authorityOwnerGeneration,
        long ownerLeaseGeneration) implements Route {
        public SpotRoute {
            if (spotId == null || spotId.isBlank()) {
                throw protocol("spotId must not be blank");
            }
            Objects.requireNonNull(targetNodeRid, "targetNodeRid");
            if (objectGeneration <= 0
                || targetNodeGeneration <= 0
                || authorityOwnerGeneration <= 0
                || ownerLeaseGeneration <= 0) {
                throw protocol("Message Follow route generations must be nonzero");
            }
        }
    }

    public record Notice(
        Route source,
        Route target,
        int hopCount,
        long queuedMessages,
        long queuedBytes,
        long originalOperationHigh,
        long originalOperationLow,
        long originalReplyRouteId) {
        public Notice {
            Objects.requireNonNull(source, "source");
            Objects.requireNonNull(target, "target");
            if (source.getClass() != target.getClass()
                || hopCount < 1
                || hopCount > MAX_HOP_COUNT
                || queuedMessages < 0
                || queuedMessages > MAX_QUEUED_MESSAGES
                || queuedBytes < 0
                || queuedBytes > MAX_QUEUED_BYTES
                || (originalOperationHigh == 0 && originalOperationLow == 0)) {
                throw protocol("Message Follow notice contains an invalid fence or bound");
            }
        }
    }

    private static final class Reader {
        private final ByteBuffer input;

        Reader(byte[] bytes) {
            input = ByteBuffer.wrap(bytes).order(ByteOrder.BIG_ENDIAN);
        }

        int u8(String field) {
            require(1, field);
            return Byte.toUnsignedInt(input.get());
        }

        int u16(String field) {
            require(Short.BYTES, field);
            return Short.toUnsignedInt(input.getShort());
        }

        long u32(String field) {
            require(Integer.BYTES, field);
            return Integer.toUnsignedLong(input.getInt());
        }

        long bits64(String field) {
            require(Long.BYTES, field);
            return input.getLong();
        }

        long nonzeroU64(String field) {
            long value = bits64(field);
            if (value <= 0) {
                throw protocol(field + " must be nonzero");
            }
            return value;
        }

        RoutingId rid(String field) {
            int length = u8(field + ".length");
            if (length == 0) {
                throw protocol(field + " must not be empty");
            }
            require(length, field);
            byte[] bytes = new byte[length];
            input.get(bytes);
            return RoutingId.from(bytes);
        }

        String text8(String field) {
            int length = u8(field + ".length");
            if (length == 0) {
                throw protocol(field + " must not be empty");
            }
            require(length, field);
            byte[] bytes = new byte[length];
            input.get(bytes);
            try {
                String value = StandardCharsets.UTF_8.newDecoder()
                    .onMalformedInput(CodingErrorAction.REPORT)
                    .onUnmappableCharacter(CodingErrorAction.REPORT)
                    .decode(ByteBuffer.wrap(bytes))
                    .toString();
                if (value.indexOf('\0') >= 0
                    || !java.util.Arrays.equals(
                        bytes, value.getBytes(StandardCharsets.UTF_8))) {
                    throw protocol(field + " is not canonical UTF-8");
                }
                return value;
            } catch (CharacterCodingException failure) {
                throw protocol(field + " is not canonical UTF-8");
            }
        }

        long remaining() {
            return input.remaining();
        }

        Reader slice(int length) {
            require(length, "route/body");
            byte[] bytes = new byte[length];
            input.get(bytes);
            return new Reader(bytes);
        }

        void end() {
            if (input.hasRemaining()) {
                throw protocol("trailing bytes are forbidden");
            }
        }

        private void require(int length, String field) {
            if (length < 0 || input.remaining() < length) {
                throw protocol("truncated " + field);
            }
        }
    }
}
