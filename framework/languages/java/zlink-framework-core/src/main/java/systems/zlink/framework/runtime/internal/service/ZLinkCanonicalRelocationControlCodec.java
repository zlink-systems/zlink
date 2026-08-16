package systems.zlink.framework.runtime.internal.service;
import java.nio.ByteBuffer;

import java.nio.charset.CharacterCodingException;
import java.nio.charset.CodingErrorAction;
import java.nio.charset.StandardCharsets;
import java.util.Arrays;
import java.util.HashSet;
import java.util.Objects;
import java.util.Set;
import systems.zlink.framework.runtime.protocol.ServiceWireConstants;

/** Strict canonical reader/preserver for service-wire relocation controls. */
final class ZLinkCanonicalRelocationControlCodec {
    private static final int PREFIX = 5;

    public record Control(int command, byte[] encoded) {
        public Control {
            encoded = Objects.requireNonNull(encoded, "encoded").clone();
        }
        @Override public byte[] encoded() { return encoded.clone(); }
    }

    public Control decode(byte[] encoded) {
        byte[] bytes = Objects.requireNonNull(encoded, "encoded").clone();
        if (bytes.length < PREFIX
            || Byte.toUnsignedInt(bytes[0]) != ServiceWireConstants.MAGIC_0
            || Byte.toUnsignedInt(bytes[1]) != ServiceWireConstants.MAGIC_1
            || Byte.toUnsignedInt(bytes[2]) != ServiceWireConstants.WIRE_MAJOR) {
            throw invalid("prefix");
        }
        int command = Byte.toUnsignedInt(bytes[3]);
        int flags = Byte.toUnsignedInt(bytes[4]);
        if (flags != 0) {
            throw invalid("flags");
        }
        Reader reader = new Reader(Arrays.copyOfRange(bytes, PREFIX, bytes.length));
        switch (command) {
            case ServiceWireConstants.COMMAND_RELOCATION_PREPARE -> prepare(reader);
            case ServiceWireConstants.COMMAND_RELOCATION_READY -> ready(reader);
            case ServiceWireConstants.COMMAND_RELOCATION_DATA -> data(reader);
            case ServiceWireConstants.COMMAND_RELOCATION_CUTOVER -> cutover(reader);
            default -> throw invalid("command");
        }
        reader.end();
        return new Control(command, bytes);
    }

    public byte[] encode(Control control) {
        Objects.requireNonNull(control, "control");
        Control decoded = decode(control.encoded());
        if (decoded.command() != control.command()) throw invalid("command summary");
        return decoded.encoded();
    }

    private static void prepare(Reader r) {
        relocationId(r); r.nonzero64(); coordinator(r); candidate(r);
        requireRole(r, 1); object(r); r.bytes8(); r.nonzero64(); root(r);
        r.ordinal();
    }

    private static void ready(Reader r) {
        relocationId(r); r.nonzero64(); coordinator(r); candidate(r);
        object(r); requireRole(r, 2);
    }

    private static void data(Reader r) {
        relocationBase(r); requireRole(r, 1); object(r); frozen(r);
    }

    private static void cutover(Reader r) {
        relocationBase(r); requireRole(r, 1); object(r);
    }

    private static void relocationBase(Reader r) {
        relocationId(r); r.nonzero64(); coordinator(r);
    }
    private static void relocationId(Reader r) {
        long high = r.u64(); long low = r.u64();
        if (high == 0 && low == 0) throw invalid("relocation id");
    }
    private static void coordinator(Reader r) {
        r.text8(); r.nonzero64(); r.bytes8(); r.nonzero64(); r.text16();
    }
    private static void candidate(Reader r) {
        r.bytes8(); r.nonzero64(); r.text8(); r.nonzero64();
    }
    private static void role(Reader r) {
        roleValue(r);
    }
    private static void requireRole(Reader r, int expected) {
        if (roleValue(r) != expected) throw invalid("sender role");
    }
    private static int roleValue(Reader r) {
        int value = r.u8(); if (value < 1 || value > 3) throw invalid("role");
        return value;
    }
    private static void object(Reader r) {
        int kind = r.u8(); Reader body = r.slice(r.u16());
        if (kind == 1 || kind == 2) {
            body.text8(); body.nonzero64(); body.nonzero64();
        } else if (kind == 3) {
            body.text8(); body.text8(); body.nonzero64();
        } else throw invalid("object kind");
        body.end();
    }
    private static void root(Reader r) {
        boolean present = r.bool(); Reader body = r.slice(r.u16());
        if (present) { body.text16(); body.u32(); }
        else if (body.remaining() != 0) throw invalid("absent root body");
        body.end();
    }
    private static void frozen(Reader r) {
        int kind = r.u8(); if (kind < 1 || kind > 14) throw invalid("frozen kind");
        int sourceKind = r.u8();
        if (sourceKind < 1 || sourceKind > 4) throw invalid("source kind");
        Reader source = r.slice(r.u16());
        source.bytes8(); source.nonzero64(); source.text8(); source.nonzero64();
        if (sourceKind == 2) source.text8();
        else if (sourceKind == 3 || sourceKind == 4) {
            source.text8(); source.nonzero64();
            if (sourceKind == 4) {
                source.bytes8(); source.nonzero64(); source.nonzero64();
            }
        }
        source.end();
        if ((kind == 8 || kind == 12 || kind == 13) && sourceKind != 1) {
            throw invalid("infrastructure source");
        }
        boolean metadata = r.bool();
        if (metadata) {
            if (!((kind >= 1 && kind <= 7) || kind == 9 || kind == 10 || kind == 14)) {
                throw invalid("metadata kind");
            }
            metadata(r);
        }
        long operationHigh = r.u64(); long operationLow = r.u64();
        int operationKind = r.u32(); if (operationKind > 15) throw invalid("operation kind");
        Reader reply = r.slice(r.u16());
        boolean requiresReply = operationKind >= 1 && operationKind <= 4
            || operationKind == 12;
        if (requiresReply) reply.nonzero64();
        reply.end();
        int instanceOperation = frozenBody(r, kind);
        boolean zero = operationHigh == 0 && operationLow == 0;
        boolean valid = switch (kind) {
            case 1, 3, 7, 12 -> operationKind == 0 && zero;
            case 2 -> operationKind == 1 && !zero;
            case 4 -> operationKind == 2 && !zero;
            case 5, 9 -> operationKind == 0 && !zero;
            case 6 -> operationKind == 3 && !zero;
            case 10 -> operationKind == 4 && !zero;
            case 8 -> operationKind == 0 && zero
                || (operationKind >= 6 && operationKind <= 8 && !zero);
            case 11 -> operationKind >= 1 && operationKind <= 15 && !zero;
            case 13 -> operationKind == 0 && zero;
            case 14 -> instanceOperation == 1
                ? operationKind == 0 && zero : operationKind == 12 && !zero;
            default -> false;
        };
        if (!valid) throw invalid("operation matrix");
    }

    private static int frozenBody(Reader r, int kind) {
        if (kind == 1 || kind == 2) payload(r);
        else if (kind == 3 || kind == 4) { r.text8(); payload(r); }
        else if (kind == 5 || kind == 6) { spotRoute(r); payload(r); }
        else if (kind == 7) { r.text8(); r.text8(); payload(r); }
        else if (kind == 8) actorControl(r);
        else if (kind == 9 || kind == 10) { actorRoute(r); payload(r); }
        else if (kind == 11) {
            int terminal = r.u32(); int failure = r.u32(); boolean has = r.bool();
            if (!validTerminalFailure(terminal, failure) || terminal != 0 && has) {
                throw invalid("completion terminal");
            }
            if (has) payload(r);
        } else if (kind == 12) sendReady(r);
        else if (kind == 13) {
            int phase = r.u8(); if (phase > 9) throw invalid("phase");
            role(r); relocationId(r); object(r);
            if (!validTerminalFailure(r.u32(), r.u32())) throw invalid("control terminal");
        } else {
            instanceRoute(r); r.nonzero64(); int operation = r.u8();
            if (operation < 1 || operation > 2) throw invalid("instance operation");
            payload(r); return operation;
        }
        return 0;
    }

    private static void metadata(Reader r) {
        int start = r.offset(); if (r.u8() != 1) throw invalid("metadata version");
        int count = r.u8(); Set<String> keys = new HashSet<>();
        for (int i = 0; i < count; i++) {
            if (!keys.add(r.text8())) throw invalid("duplicate metadata key");
            r.text16(); if (r.offset() - start > 1024) throw invalid("metadata bound");
        }
    }
    private static void payload(Reader r) {
        if (r.u8() != 1) throw invalid("payload version");
        Reader body = r.slice(r.u32Length(Integer.MAX_VALUE));
        body.text8(); body.text8(); int length = body.u32Length(Integer.MAX_VALUE);
        body.raw(length); body.end();
    }
    private static void spotIdentity(Reader r) { r.text8(); r.nonzero64(); }
    private static void actorIdentity(Reader r) { r.text8(); r.nonzero64(); }
    private static void spotRoute(Reader r) {
        spotIdentity(r); r.bytes8(); r.nonzero64(); r.nonzero64(); r.nonzero64();
    }
    private static void actorRoute(Reader r) {
        actorIdentity(r); r.bytes8(); r.nonzero64(); r.nonzero64(); r.nonzero64();
    }
    private static void membership(Reader r) { actorIdentity(r); spotIdentity(r); }
    private static void optionalMembership(Reader r) {
        boolean present = r.bool(); Reader body = r.slice(r.u16());
        if (present) membership(body); else if (body.remaining() != 0) throw invalid("membership body");
        body.end();
    }
    private static void actorControl(Reader r) {
        int lifecycle = r.u8(); if (lifecycle < 1 || lifecycle > 5) throw invalid("lifecycle");
        Reader body = r.slice(r.u16());
        if (lifecycle == 1 || lifecycle == 4 || lifecycle == 5) membership(body);
        else if (lifecycle == 2) { optionalMembership(body); membership(body); }
        else { membership(body); membership(body); }
        body.end();
    }
    private static void sendReady(Reader r) {
        int kind = r.u8(); if (kind < 1 || kind > 5) throw invalid("destination");
        Reader body = r.slice(r.u16());
        if (kind == 1) body.bytes8(); else if (kind == 2) body.text8();
        else if (kind == 3) spotRoute(body);
        else { actorRoute(body); if (kind == 5) body.nonzero64(); }
        body.end();
    }
    private static void instanceRoute(Reader r) {
        int kind = r.u8(); if (kind < 1 || kind > 2) throw invalid("instance route");
        Reader body = r.slice(r.u16()); body.bytes8(); body.nonzero64(); body.text8();
        if (kind == 1) {
            body.nonzero64(); body.text8(); body.nonzero64(); body.nonzero64(); body.text16();
        } else {
            body.text8(); body.text8(); body.text8(); body.nonzero64();
        }
        body.end();
    }

    private static boolean validTerminalFailure(int terminal, int failure) {
        //  Schema terminal-failure-integrity (spec 51:43-47): delegate to the
        //  generated single source instead of redefining the pair table here.
        return ServiceWireConstants.validTerminalFailure(terminal, failure);
    }

    private static IllegalArgumentException invalid(String field) {
        return new IllegalArgumentException("invalid canonical relocation control " + field);
    }

    private static final class Reader {
        private final byte[] bytes; private int offset;
        Reader(byte[] bytes) { this.bytes = bytes; }
        int offset() { return offset; }
        int remaining() { return bytes.length - offset; }
        int u8() { require(1); return Byte.toUnsignedInt(bytes[offset++]); }
        int u16() { return u8() << 8 | u8(); }
        int u32() { return u8() << 24 | u8() << 16 | u8() << 8 | u8(); }
        int u32Length(int max) {
            long value = Integer.toUnsignedLong(u32());
            if (value > max) throw invalid("length bound"); return (int) value;
        }
        long u64() { return Integer.toUnsignedLong(u32()) << 32 | Integer.toUnsignedLong(u32()); }
        long nonzero64() { long value = u64(); if (value == 0) throw invalid("zero field"); return value; }
        long ordinal() { long value = u64(); if (value < 0) throw invalid("ordinal"); return value; }
        boolean bool() { int value = u8(); if (value > 1) throw invalid("boolean"); return value != 0; }
        byte[] bytes8() { int length = u8(); if (length == 0) throw invalid("bytes8"); return raw(length); }
        String text8() { return text(u8()); }
        String text16() { return text(u16()); }
        String text(int length) {
            if (length == 0) throw invalid("text");
            try {
                String value = StandardCharsets.UTF_8.newDecoder()
                    .onMalformedInput(CodingErrorAction.REPORT)
                    .onUnmappableCharacter(CodingErrorAction.REPORT)
                    .decode(ByteBuffer.wrap(raw(length))).toString();
                if (value.indexOf('\0') >= 0) throw invalid("NUL text"); return value;
            } catch (CharacterCodingException failure) { throw invalid("UTF-8"); }
        }
        Reader slice(int length) { return new Reader(raw(length)); }
        byte[] raw(int length) {
            require(length); byte[] result = Arrays.copyOfRange(bytes, offset, offset + length);
            offset += length; return result;
        }
        void require(int length) { if (length < 0 || remaining() < length) throw invalid("truncated field"); }
        void end() { if (remaining() != 0) throw invalid("trailing byte"); }
    }
}
