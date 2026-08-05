package systems.zlink.framework.runtime.internal.service;

import java.io.ByteArrayOutputStream;
import java.nio.charset.CharacterCodingException;
import java.nio.charset.CodingErrorAction;
import java.nio.charset.StandardCharsets;
import java.util.Objects;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.runtime.protocol.ServiceWireConstants;

/** Canonical service-wire command 33/46 relocation reply codec. */
public final class ZLinkServiceRelocationWireCodec {
    private static final int PREFIX_BYTES = 5;
    private static final int MAINTENANCE_RELOCATION_CONTEXT = 2;

    public byte[] encodeReplyRelay(ReplyRelay value) {
        Objects.requireNonNull(value, "value");
        value.validate();
        Writer context = new Writer();
        context.u64(value.relocation().high());
        context.u64(value.relocation().low());
        context.u64(value.targetAttemptGeneration());
        writeCoordinator(context, value.coordinator());
        context.u64(value.participantId());
        context.u64(value.sequence());
        byte[] contextBytes = context.bytes();
        if (contextBytes.length > 0xffff) {
            throw new IllegalArgumentException(
                "reply relay context exceeds uint16");
        }
        Writer body = new Writer();
        body.u64(value.operation().high());
        body.u64(value.operation().low());
        body.u64(value.replyRouteId());
        body.u8(MAINTENANCE_RELOCATION_CONTEXT);
        body.u16(contextBytes.length);
        body.raw(contextBytes);
        body.u32(value.terminalResult());
        body.u32(value.failureCode());
        return prefixed(ServiceWireConstants.COMMAND_REPLY_RELAY, body.bytes());
    }

    public ReplyRelay decodeReplyRelay(byte[] encoded) {
        Reader body = body(encoded, ServiceWireConstants.COMMAND_REPLY_RELAY);
        Operation operation = new Operation(body.u64(), body.u64());
        long replyRoute = body.nonzeroU64("replyRouteId");
        if (body.u8() != MAINTENANCE_RELOCATION_CONTEXT) {
            throw invalid("reply relay context kind");
        }
        Reader context = body.slice(body.u16());
        RelocationId relocation = new RelocationId(
            context.u64(), context.u64());
        long attempt = context.nonzeroU64("targetAttemptGeneration");
        CoordinatorFence coordinator = readCoordinator(context);
        long participant = context.nonzeroU64("participantId");
        long sequence = context.nonzeroU64("sequence");
        context.end();
        int terminal = body.u32();
        int failure = body.u32();
        body.end();
        ReplyRelay result = new ReplyRelay(
            operation, replyRoute, relocation, attempt, coordinator,
            participant, sequence, terminal, failure);
        result.validate();
        return result;
    }

    public byte[] encodeReplyRelayAck(ReplyRelayAck value) {
        Objects.requireNonNull(value, "value");
        value.validate();
        Writer body = new Writer();
        body.u64(value.relocation().high());
        body.u64(value.relocation().low());
        writeCoordinator(body, value.coordinator());
        body.u64(value.operation().high());
        body.u64(value.operation().low());
        body.u64(value.replyRouteId());
        writeRequestSource(body, value.requestSource());
        body.u8(value.status());
        return prefixed(
            ServiceWireConstants.COMMAND_REPLY_RELAY_ACK, body.bytes());
    }

    public ReplyRelayAck decodeReplyRelayAck(byte[] encoded) {
        Reader body = body(
            encoded, ServiceWireConstants.COMMAND_REPLY_RELAY_ACK);
        RelocationId relocation = new RelocationId(
            body.u64(), body.u64());
        CoordinatorFence coordinator = readCoordinator(body);
        Operation operation = new Operation(body.u64(), body.u64());
        long replyRouteId = body.u64();
        RequestSourceFence source = readRequestSource(body);
        int status = body.u8();
        body.end();
        ReplyRelayAck result = new ReplyRelayAck(
            relocation, coordinator, operation, replyRouteId, source, status);
        result.validate();
        return result;
    }

    private static byte[] prefixed(int command, byte[] body) {
        byte[] result = new byte[PREFIX_BYTES + body.length];
        result[0] = (byte) ServiceWireConstants.MAGIC_0;
        result[1] = (byte) ServiceWireConstants.MAGIC_1;
        result[2] = (byte) ServiceWireConstants.WIRE_MAJOR;
        result[3] = (byte) command;
        result[4] = 0;
        System.arraycopy(body, 0, result, PREFIX_BYTES, body.length);
        return result;
    }

    private static Reader body(byte[] encoded, int expectedCommand) {
        Objects.requireNonNull(encoded, "encoded");
        if (encoded.length < PREFIX_BYTES
            || Byte.toUnsignedInt(encoded[0]) != ServiceWireConstants.MAGIC_0
            || Byte.toUnsignedInt(encoded[1]) != ServiceWireConstants.MAGIC_1
            || Byte.toUnsignedInt(encoded[2])
                != ServiceWireConstants.WIRE_MAJOR
            || Byte.toUnsignedInt(encoded[3]) != expectedCommand
            || encoded[4] != 0) {
            throw invalid("service reply relay prefix");
        }
        return new Reader(
            java.util.Arrays.copyOfRange(
                encoded, PREFIX_BYTES, encoded.length));
    }

    private static void writeCoordinator(
        Writer writer, CoordinatorFence value) {
        value.validate();
        writer.text8(value.ownerId());
        writer.u64(value.leaseGeneration());
        writer.rid(value.nodeRid());
        writer.u64(value.nodeGeneration());
        writer.text16(value.expectedAuthorityStoreVersion());
    }

    private static CoordinatorFence readCoordinator(Reader reader) {
        return new CoordinatorFence(
            reader.text8(),
            reader.nonzeroU64("coordinator lease generation"),
            reader.rid(),
            reader.nonzeroU64("coordinator node generation"),
            reader.text16());
    }

    private static void writeRequestSource(
        Writer writer, RequestSourceFence value) {
        value.validate();
        writer.text8(value.ownerId());
        writer.u64(value.leaseGeneration());
        writer.rid(value.nodeRid());
        writer.u64(value.nodeGeneration());
    }

    private static RequestSourceFence readRequestSource(Reader reader) {
        return new RequestSourceFence(
            reader.text8(),
            reader.nonzeroU64("request source lease generation"),
            reader.rid(),
            reader.nonzeroU64("request source node generation"));
    }

    private static boolean validFailureCode(int value) {
        return value >= 0 && value <= 22
            || value >= 33 && value <= 35;
    }

    private static boolean validTerminalResult(int value) {
        return value == 0 || value >= 101 && value <= 113;
    }

    private static boolean validTerminalFailure(
        int terminalResult, int failureCode) {
        if (terminalResult == 0
            || terminalResult == 101 || terminalResult == 103
            || terminalResult >= 108 && terminalResult <= 113) {
            return failureCode == 0;
        }
        return switch (terminalResult) {
            case 102 -> failureCode == 1 || failureCode == 6
                || failureCode >= 8 && failureCode <= 11
                || failureCode == 14;
            case 104 -> failureCode == 12 || failureCode == 16;
            case 105 -> failureCode == 2 || failureCode == 5
                || failureCode == 13 || failureCode == 17
                || failureCode == 19 || failureCode == 20
                || failureCode == 35;
            case 106 -> failureCode == 15 || failureCode == 18
                || failureCode == 22;
            case 107 -> failureCode == 3 || failureCode == 4
                || failureCode == 7 || failureCode == 21
                || failureCode == 33 || failureCode == 34;
            default -> false;
        };
    }

    private static IllegalArgumentException invalid(String field) {
        return new IllegalArgumentException(
            "invalid canonical reply relay " + field);
    }

    public record Operation(long high, long low) {
        private void validate() {
            if (high == 0 && low == 0) throw invalid("operation");
        }
    }

    public record RelocationId(long high, long low) {
        private void validate() {
            if (high == 0 && low == 0) throw invalid("relocation id");
        }
    }

    public record CoordinatorFence(
        String ownerId,
        long leaseGeneration,
        RoutingId nodeRid,
        long nodeGeneration,
        String expectedAuthorityStoreVersion) {
        private void validate() {
            requireText(ownerId, "coordinator owner");
            requireText(expectedAuthorityStoreVersion,
                "expected authority store version");
            Objects.requireNonNull(nodeRid, "nodeRid");
            if (leaseGeneration == 0 || nodeGeneration == 0) {
                throw invalid("coordinator fence");
            }
        }
    }

    public record RequestSourceFence(
        String ownerId,
        long leaseGeneration,
        RoutingId nodeRid,
        long nodeGeneration) {
        private void validate() {
            requireText(ownerId, "request source owner");
            Objects.requireNonNull(nodeRid, "nodeRid");
            if (leaseGeneration == 0 || nodeGeneration == 0) {
                throw invalid("request source fence");
            }
        }
    }

    public record ReplyRelay(
        Operation operation,
        long replyRouteId,
        RelocationId relocation,
        long targetAttemptGeneration,
        CoordinatorFence coordinator,
        long participantId,
        long sequence,
        int terminalResult,
        int failureCode) {
        private void validate() {
            Objects.requireNonNull(operation, "operation").validate();
            Objects.requireNonNull(relocation, "relocation").validate();
            Objects.requireNonNull(coordinator, "coordinator").validate();
            if (replyRouteId == 0 || targetAttemptGeneration == 0
                || participantId == 0 || sequence == 0
                || !validTerminalResult(terminalResult)
                || !validFailureCode(failureCode)
                || !validTerminalFailure(terminalResult, failureCode)) {
                throw invalid("command 33 field");
            }
        }
    }

    public record ReplyRelayAck(
        RelocationId relocation,
        CoordinatorFence coordinator,
        Operation operation,
        long replyRouteId,
        RequestSourceFence requestSource,
        int status) {
        private void validate() {
            Objects.requireNonNull(relocation, "relocation").validate();
            Objects.requireNonNull(coordinator, "coordinator").validate();
            Objects.requireNonNull(operation, "operation").validate();
            Objects.requireNonNull(requestSource, "requestSource").validate();
            if (replyRouteId == 0 || status != 1 && status != 2) {
                throw invalid("command 46 field");
            }
        }
    }

    private static void requireText(String value, String field) {
        if (value == null || value.isEmpty() || value.indexOf('\0') >= 0) {
            throw invalid(field);
        }
    }

    private static final class Writer {
        private final ByteArrayOutputStream output = new ByteArrayOutputStream();
        void u8(int value) { output.write(value); }
        void u16(int value) { u8(value >>> 8); u8(value); }
        void u32(long value) {
            u8((int) (value >>> 24)); u8((int) (value >>> 16));
            u8((int) (value >>> 8)); u8((int) value);
        }
        void u64(long value) { u32(value >>> 32); u32(value); }
        void raw(byte[] value) { output.writeBytes(value); }
        void text8(String value) {
            byte[] bytes = text(value);
            if (bytes.length == 0 || bytes.length > 255) {
                throw invalid("text8");
            }
            u8(bytes.length); raw(bytes);
        }
        void text16(String value) {
            byte[] bytes = text(value);
            if (bytes.length == 0 || bytes.length > 0xffff) {
                throw invalid("text16");
            }
            u16(bytes.length); raw(bytes);
        }
        void rid(RoutingId value) {
            byte[] bytes = value.toBytes();
            if (bytes.length == 0 || bytes.length > 255) {
                throw invalid("RoutingId");
            }
            u8(bytes.length); raw(bytes);
        }
        byte[] bytes() { return output.toByteArray(); }
        private static byte[] text(String value) {
            requireText(value, "text");
            return value.getBytes(StandardCharsets.UTF_8);
        }
    }

    private static final class Reader {
        private final byte[] bytes;
        private int offset;
        Reader(byte[] bytes) { this.bytes = bytes; }
        int u8() { require(1); return Byte.toUnsignedInt(bytes[offset++]); }
        int u16() { return u8() << 8 | u8(); }
        int u32() {
            return u8() << 24 | u8() << 16 | u8() << 8 | u8();
        }
        long u64() {
            return Integer.toUnsignedLong(u32()) << 32
                | Integer.toUnsignedLong(u32());
        }
        long nonzeroU64(String field) {
            long value = u64();
            if (value == 0) throw invalid(field);
            return value;
        }
        String text8() { return text(u8()); }
        String text16() { return text(u16()); }
        RoutingId rid() {
            int length = u8();
            if (length == 0) throw invalid("RoutingId");
            return RoutingId.from(raw(length));
        }
        Reader slice(int length) { return new Reader(raw(length)); }
        byte[] raw(int length) {
            require(length);
            byte[] result = java.util.Arrays.copyOfRange(
                bytes, offset, offset + length);
            offset += length;
            return result;
        }
        String text(int length) {
            if (length == 0) throw invalid("text");
            try {
                String value = StandardCharsets.UTF_8.newDecoder()
                    .onMalformedInput(CodingErrorAction.REPORT)
                    .onUnmappableCharacter(CodingErrorAction.REPORT)
                    .decode(java.nio.ByteBuffer.wrap(raw(length)))
                    .toString();
                requireText(value, "text");
                return value;
            } catch (CharacterCodingException failure) {
                throw invalid("UTF-8 text");
            }
        }
        void end() {
            if (offset != bytes.length) throw invalid("trailing byte");
        }
        void require(int length) {
            if (length < 0 || bytes.length - offset < length) {
                throw invalid("truncated field");
            }
        }
    }
}
