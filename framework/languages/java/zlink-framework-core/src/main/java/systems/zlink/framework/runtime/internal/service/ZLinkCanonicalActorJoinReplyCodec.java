package systems.zlink.framework.runtime.internal.service;

import java.nio.ByteBuffer;
import java.nio.charset.CharacterCodingException;
import java.nio.charset.CodingErrorAction;
import java.nio.charset.StandardCharsets;
import java.util.Arrays;
import java.util.Objects;
import systems.zlink.framework.runtime.protocol.ServiceWireConstants;

/**
 * Canonical binary reader/writer for the reply(20) actor-join-reply-tail
 * (service-wire-v1.schema.json:2251-2294, tail at :5387-5397). This is a
 * structural codec, not a byte-preserver: decode parses every field into
 * {@link ActorJoinReply} and encode reconstructs the wire bytes from those
 * fields, so a decode + re-encode round trip is a genuine field-for-field
 * verification rather than a pass-through of the original buffer.
 *
 * <p>{@link #encodeTail}/{@link #decodeTail} expose just the {@code join}
 * tail body (joinResult + case fields, service-wire-v1.schema.json:5387-
 * 5397) without the surrounding reply(20) frame (prefix, correlation,
 * terminalResult, failureCode) for transports - such as the routed actor
 * Spot admission reply - that carry the same tail bytes outside a full
 * service-wire reply(20) frame.
 */
public final class ZLinkCanonicalActorJoinReplyCodec {
    //  relocationStorageProfile.chunkDataMaximumBytes
    //  (service-wire-v1.schema.json:1336) - the application-level bound the
    //  golden fixture notes every runtime reuses for receiveChunkLimitBytes.
    private static final long CHUNK_DATA_BYTES_BOUND = 67_108_864L;
    public static final int JOIN_RESULT_ACCEPTED = 0;
    public static final int JOIN_RESULT_REJECTED = 1;

    public record SpotRef(String spotId, long objectGeneration) {
    }

    /** The {@code join} tail alone: joinResult plus its case fields. */
    public record Tail(
        int joinResult,
        SpotRef spot,
        Long membershipEpoch,
        Long receiveChunkLimitBytes) {
        public boolean accepted() {
            return joinResult == JOIN_RESULT_ACCEPTED;
        }
    }

    public record ActorJoinReply(
        long correlation,
        int terminalResult,
        int failureCode,
        int joinResult,
        SpotRef spot,
        Long membershipEpoch,
        Long receiveChunkLimitBytes) {
        public boolean accepted() {
            return joinResult == JOIN_RESULT_ACCEPTED;
        }
    }

    public ActorJoinReply decode(byte[] encoded) {
        byte[] bytes = Objects.requireNonNull(encoded, "encoded");
        if (bytes.length < 5
            || Byte.toUnsignedInt(bytes[0]) != ServiceWireConstants.MAGIC_0
            || Byte.toUnsignedInt(bytes[1]) != ServiceWireConstants.MAGIC_1
            || Byte.toUnsignedInt(bytes[2]) != ServiceWireConstants.WIRE_MAJOR) {
            throw invalid("prefix");
        }
        if (Byte.toUnsignedInt(bytes[3]) != ServiceWireConstants.COMMAND_REPLY) {
            throw invalid("command");
        }
        if (Byte.toUnsignedInt(bytes[4]) != 0) {
            throw invalid("flags");
        }
        Reader r = new Reader(Arrays.copyOfRange(bytes, 5, bytes.length));
        long correlation = r.nonzero64("correlation");
        long terminalResult = r.u32Unsigned();
        long failureCode = r.u32Unsigned();
        if (terminalResult != 0 || !ServiceWireConstants.validTerminalFailure(terminalResult, failureCode)) {
            throw invalid("terminal/failure pair");
        }
        Tail tail = decodeTailBody(r);
        r.end();
        return new ActorJoinReply(
            correlation, (int) terminalResult, (int) failureCode,
            tail.joinResult(), tail.spot(), tail.membershipEpoch(), tail.receiveChunkLimitBytes());
    }

    /** Decodes just the {@code join} tail bytes (no reply(20) frame prefix). */
    public Tail decodeTail(byte[] encoded) {
        Reader r = new Reader(Objects.requireNonNull(encoded, "encoded"));
        Tail tail = decodeTailBody(r);
        r.end();
        return tail;
    }

    private static Tail decodeTailBody(Reader r) {
        int joinResult = (int) r.u32Unsigned();
        if (joinResult != JOIN_RESULT_ACCEPTED && joinResult != JOIN_RESULT_REJECTED) {
            throw invalid("joinResult");
        }
        Reader body = r.slice(r.u16());
        if (joinResult == JOIN_RESULT_ACCEPTED) {
            SpotRef spot = spotRef(body);
            long membershipEpoch = body.nonzero64("membershipEpoch");
            long receiveChunkLimitBytes = body.u32Unsigned();
            if (receiveChunkLimitBytes > CHUNK_DATA_BYTES_BOUND) {
                throw invalid("receiveChunkLimitBytes bound");
            }
            body.end();
            return new Tail(joinResult, spot, membershipEpoch, receiveChunkLimitBytes);
        }
        boolean hasSpot = body.bool8();
        Reader optional = body.slice(body.u16());
        SpotRef spot = hasSpot ? spotRef(optional) : null;
        optional.end();
        body.end();
        return new Tail(joinResult, spot, null, null);
    }

    public byte[] encode(ActorJoinReply reply) {
        Objects.requireNonNull(reply, "reply");
        if (reply.correlation() == 0) throw invalid("correlation");
        if (reply.terminalResult() != 0
            || !ServiceWireConstants.validTerminalFailure(reply.terminalResult(), reply.failureCode())) {
            throw invalid("terminal/failure pair");
        }
        byte[] tail = encodeTail(new Tail(
            reply.joinResult(), reply.spot(), reply.membershipEpoch(), reply.receiveChunkLimitBytes()));
        Writer frame = new Writer();
        frame.u8(ServiceWireConstants.MAGIC_0);
        frame.u8(ServiceWireConstants.MAGIC_1);
        frame.u8(ServiceWireConstants.WIRE_MAJOR);
        frame.u8(ServiceWireConstants.COMMAND_REPLY);
        frame.u8(0);
        frame.u64(reply.correlation());
        frame.u32(reply.terminalResult());
        frame.u32(reply.failureCode());
        frame.raw(tail);
        return frame.toByteArray();
    }

    /**
     * Encodes just the {@code join} tail (joinResult + bodyLength + case
     * fields, no reply(20) frame prefix/correlation/terminal/failure).
     */
    public byte[] encodeTail(Tail tail) {
        Objects.requireNonNull(tail, "tail");
        byte[] caseBody;
        if (tail.joinResult() == JOIN_RESULT_ACCEPTED) {
            SpotRef spot = Objects.requireNonNull(tail.spot(), "spot");
            long membershipEpoch = Objects.requireNonNull(tail.membershipEpoch(), "membershipEpoch");
            long receiveChunkLimitBytes = Objects.requireNonNull(
                tail.receiveChunkLimitBytes(), "receiveChunkLimitBytes");
            if (membershipEpoch == 0) throw invalid("membershipEpoch");
            if (receiveChunkLimitBytes < 0 || receiveChunkLimitBytes > CHUNK_DATA_BYTES_BOUND) {
                throw invalid("receiveChunkLimitBytes bound");
            }
            Writer w = new Writer();
            w.spotRef(spot);
            w.u64(membershipEpoch);
            w.u32(receiveChunkLimitBytes);
            caseBody = w.toByteArray();
        } else if (tail.joinResult() == JOIN_RESULT_REJECTED) {
            Writer inner = new Writer();
            boolean hasSpot = tail.spot() != null;
            if (hasSpot) {
                inner.spotRef(tail.spot());
            }
            Writer w = new Writer();
            w.u8(hasSpot ? 1 : 0);
            w.u16(inner.toByteArray().length);
            w.raw(inner.toByteArray());
            caseBody = w.toByteArray();
        } else {
            throw invalid("joinResult");
        }
        Writer w = new Writer();
        w.u32(tail.joinResult());
        w.u16(caseBody.length);
        w.raw(caseBody);
        return w.toByteArray();
    }

    private static SpotRef spotRef(Reader r) {
        String spotId = r.text8();
        long generation = r.nonzero64("objectGeneration");
        return new SpotRef(spotId, generation);
    }

    private static IllegalArgumentException invalid(String field) {
        return new IllegalArgumentException("invalid canonical actor join reply " + field);
    }

    private static final class Reader {
        private final byte[] bytes;
        private int offset;
        Reader(byte[] bytes) { this.bytes = bytes; }
        int remaining() { return bytes.length - offset; }
        int u8() { require(1); return Byte.toUnsignedInt(bytes[offset++]); }
        int u16() { return u8() << 8 | u8(); }
        long u32Unsigned() { return Integer.toUnsignedLong(u8() << 24 | u8() << 16 | u8() << 8 | u8()); }
        long u64() { return u32Unsigned() << 32 | u32Unsigned(); }
        long nonzero64(String field) {
            long value = u64();
            if (value == 0) throw invalid(field);
            return value;
        }
        boolean bool8() {
            int value = u8();
            if (value > 1) throw invalid("boolean");
            return value != 0;
        }
        String text8() { return text(u8()); }
        String text(int length) {
            if (length == 0) throw invalid("text");
            try {
                String value = StandardCharsets.UTF_8.newDecoder()
                    .onMalformedInput(CodingErrorAction.REPORT)
                    .onUnmappableCharacter(CodingErrorAction.REPORT)
                    .decode(ByteBuffer.wrap(raw(length))).toString();
                if (value.indexOf('\0') >= 0) throw invalid("NUL text");
                return value;
            } catch (CharacterCodingException failure) {
                throw invalid("UTF-8");
            }
        }
        Reader slice(int length) { return new Reader(raw(length)); }
        byte[] raw(int length) {
            require(length);
            byte[] result = Arrays.copyOfRange(bytes, offset, offset + length);
            offset += length;
            return result;
        }
        void require(int length) {
            if (length < 0 || remaining() < length) throw invalid("truncated field");
        }
        void end() {
            if (remaining() != 0) throw invalid("trailing byte");
        }
    }

    private static final class Writer {
        private byte[] bytes = new byte[16];
        private int length;
        void u8(int value) {
            ensure(1);
            bytes[length++] = (byte) value;
        }
        void u16(int value) {
            u8(value >>> 8 & 0xFF);
            u8(value & 0xFF);
        }
        void u32(long value) {
            u8((int) (value >>> 24 & 0xFF));
            u8((int) (value >>> 16 & 0xFF));
            u8((int) (value >>> 8 & 0xFF));
            u8((int) (value & 0xFF));
        }
        void u64(long value) {
            u32(value >>> 32 & 0xFFFFFFFFL);
            u32(value & 0xFFFFFFFFL);
        }
        void raw(byte[] value) {
            ensure(value.length);
            System.arraycopy(value, 0, bytes, length, value.length);
            length += value.length;
        }
        void text8(String value) {
            byte[] encoded = value.getBytes(StandardCharsets.UTF_8);
            if (encoded.length == 0 || encoded.length > 255) throw invalid("text8 length");
            u8(encoded.length);
            raw(encoded);
        }
        void spotRef(SpotRef spot) {
            text8(spot.spotId());
            u64(spot.objectGeneration());
        }
        void ensure(int extra) {
            if (length + extra > bytes.length) {
                bytes = Arrays.copyOf(bytes, Math.max(bytes.length * 2, length + extra));
            }
        }
        byte[] toByteArray() { return Arrays.copyOf(bytes, length); }
    }
}
