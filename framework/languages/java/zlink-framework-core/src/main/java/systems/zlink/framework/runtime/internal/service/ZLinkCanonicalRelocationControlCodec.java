package systems.zlink.framework.runtime.internal.service;

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
    private static final int MAX_PARTICIPANTS = 2048;
    private static final int MAX_EXTENSION = 1024 * 1024;

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
        if (flags != (command == ServiceWireConstants.COMMAND_RELOCATION_READY
                ? 8 : 0)) {
            throw invalid("flags");
        }
        Reader reader = new Reader(Arrays.copyOfRange(bytes, PREFIX, bytes.length));
        switch (command) {
            case ServiceWireConstants.COMMAND_RELOCATION_PREPARE -> prepare(reader);
            case ServiceWireConstants.COMMAND_RELOCATION_READY -> ready(reader);
            case ServiceWireConstants.COMMAND_RELOCATION_RESERVED -> reserved(reader);
            case ServiceWireConstants.COMMAND_RELOCATION_DATA -> data(reader);
            case ServiceWireConstants.COMMAND_RELOCATION_ACK -> ack(reader);
            case ServiceWireConstants.COMMAND_RELOCATION_SEAL -> seal(reader);
            case ServiceWireConstants.COMMAND_RELOCATION_COMPLETE -> complete(reader);
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
        relocationId(r); r.nonzero64(); round(r); coordinator(r); candidate(r);
        role(r); object(r); r.bytes8(); r.nonzero64(); r.ordinal(); r.ordinal();
        participants(r); root(r); r.ordinal();
    }

    private static void ready(Reader r) {
        relocationId(r); r.nonzero64(); round(r); coordinator(r); candidate(r);
        object(r); int readyRole = roleValue(r);
        long offeredMessages = r.ordinal(); long offeredBytes = r.ordinal();
        int participantCount = participants(r);
        if ((readyRole == 2 && (offeredMessages == 0 || offeredBytes == 0
                || participantCount != 0))
            || (readyRole == 1 && (offeredMessages != 0 || offeredBytes != 0
                || participantCount == 0))
            || (readyRole != 1 && readyRole != 2)) {
            throw invalid("ready offer or accept fields");
        }
        int length = r.u32Length(MAX_EXTENSION);
        Reader extension = r.slice(length);
        int previous = 0;
        boolean[] required = new boolean[10];
        while (extension.remaining() != 0) {
            int tag = extension.u8();
            if (tag <= previous || tag < 2 || tag > 9 || tag == 7) {
                throw invalid("ready extension tag");
            }
            previous = tag;
            Reader field = extension.slice(extension.u32Length(MAX_EXTENSION));
            switch (tag) {
                case 2, 3, 4 -> field.nonzero64();
                case 5 -> field.text16();
                case 6 -> field.u32();
                case 8 -> field.ordinal();
                case 9 -> progress(field);
                default -> throw invalid("ready extension tag");
            }
            field.end(); required[tag] = true;
        }
        extension.end();
        if (!required[2] || !required[3] || !required[4]
            || !required[8] || !required[9] || required[5] != required[6]) {
            throw invalid("ready required extension");
        }
    }

    private static void reserved(Reader r) {
        relocationId(r); r.nonzero64(); round(r); coordinator(r); candidate(r);
        r.nonzero64(); participants(r);
    }

    private static void data(Reader r) {
        relocationBase(r); role(r); r.nonzero64(); r.nonzero64(); frozen(r);
    }

    private static void ack(Reader r) {
        relocationBase(r); role(r); r.nonzero64(); r.ordinal();
    }

    private static void seal(Reader r) {
        relocationBase(r); role(r); r.bool();
        int count = r.u32Length(MAX_PARTICIPANTS);
        long previous = 0;
        for (int i = 0; i < count; i++) {
            long id = r.nonzero64();
            if (Long.compareUnsigned(id, previous) <= 0) throw invalid("terminal order");
            previous = id; r.ordinal();
        }
    }

    private static void complete(Reader r) {
        relocationBase(r); role(r); requestSource(r);
        int state = r.u8();
        if (state > 2) throw invalid("cleanup state");
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
    private static void requestSource(Reader r) {
        r.text8(); r.nonzero64(); r.bytes8(); r.nonzero64();
    }
    private static void candidate(Reader r) {
        r.bytes8(); r.nonzero64(); r.text8(); r.nonzero64();
    }
    private static void role(Reader r) {
        roleValue(r);
    }
    private static int roleValue(Reader r) {
        int value = r.u8(); if (value < 1 || value > 3) throw invalid("role");
        return value;
    }
    private static void round(Reader r) {
        int value = r.u8(); if (value < 1 || value > 3) throw invalid("round");
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
    private static int participants(Reader r) {
        int count = r.u32Length(MAX_PARTICIPANTS); long previous = 0;
        for (int i = 0; i < count; i++) {
            long id = r.nonzero64();
            if (Long.compareUnsigned(id, previous) <= 0) throw invalid("participant order");
            previous = id;
            int kind = r.u8(); Reader identity = r.slice(r.u16());
            if (kind == 2) {
                identity.bytes8(); identity.nonzero64(); identity.text8();
                identity.nonzero64(); identity.bytes8(); identity.nonzero64();
            } else if (kind != 1 || identity.remaining() != 0) {
                throw invalid("participant identity");
            }
            identity.end(); r.ordinal(); r.ordinal();
        }
        return count;
    }
    private static void root(Reader r) {
        boolean present = r.bool(); Reader body = r.slice(r.u16());
        if (present) { body.text16(); body.u32(); }
        else if (body.remaining() != 0) throw invalid("absent root body");
        body.end();
    }
    private static void progress(Reader r) {
        int count = r.u32Length(MAX_PARTICIPANTS); long previous = 0;
        for (int i = 0; i < count; i++) {
            long id = r.nonzero64();
            if (Long.compareUnsigned(id, previous) <= 0) throw invalid("progress order");
            previous = id;
            long accepted = r.ordinal(); long replay = r.ordinal();
            if (replay > accepted) throw invalid("replay cursor");
        }
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
        if (terminal == 0 || terminal == 101 || terminal == 103
            || terminal >= 108 && terminal <= 113) return failure == 0;
        return switch (terminal) {
            case 102 -> failure == 1 || failure == 6 || failure >= 8 && failure <= 11 || failure == 14;
            case 104 -> failure == 12 || failure == 16;
            case 105 -> failure == 2 || failure == 5 || failure == 13
                || failure == 17 || failure == 19 || failure == 20 || failure == 35;
            case 106 -> failure == 15 || failure == 18 || failure == 22;
            case 107 -> failure == 3 || failure == 4 || failure == 7
                || failure == 21 || failure == 33 || failure == 34;
            default -> false;
        };
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
                    .decode(java.nio.ByteBuffer.wrap(raw(length))).toString();
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
