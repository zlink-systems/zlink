package systems.zlink.framework.runtime.channels;

import java.io.ByteArrayOutputStream;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.charset.CharacterCodingException;
import java.nio.charset.CodingErrorAction;
import java.nio.charset.StandardCharsets;
import java.util.Objects;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.runtime.internal.locations.ZLinkClientServerServerDescriptor;
import systems.zlink.framework.runtime.host.ZLinkFrameworkRuntimeState;

final class ZLinkClientServerServiceWire {
    private static final int MAGIC_0 = 0x5a;
    private static final int MAGIC_1 = 0x4d;
    private static final int WIRE_MAJOR = 1;
    private static final int TOPOLOGY_CLIENT_SERVER = 2;
    private static final int ROLE_CLIENT = 1;
    private static final int ROLE_SERVER = 2;
    private static final int DIRECTION_CLIENT_TO_SERVER = 1;
    private static final int COMMAND_HELLO = 1;
    private static final int COMMAND_ADMIT = 2;
    private static final int COMMAND_REJECT = 3;
    private static final int COMMAND_UPDATE = 4;
    private static final int COMMAND_LIVENESS_PROBE = 5;
    private static final int COMMAND_LIVENESS_ACK = 6;
    private static final int MAX_DESCRIPTOR_BYTES = 1024 * 1024;

    private ZLinkClientServerServiceWire() {
    }

    static byte[] encodeHello(Hello value) {
        Writer body = new Writer();
        body.text8(value.channelName(), "channelName");
        body.u8(DIRECTION_CLIENT_TO_SERVER);
        body.text8(value.securityIdentity(), "securityIdentity");
        body.positiveU32(
            value.normalizedEffectiveMaxMessageBytes(),
            "normalizedEffectiveMaxMessageBytes");
        return encodeAdmission(COMMAND_HELLO, ROLE_CLIENT, body.toByteArray());
    }

    static byte[] encodeAdmit(
        ZLinkClientServerServerDescriptor descriptor,
        int normalizedEffectiveMaxMessageBytes) {
        return encodeServerAdmission(
            COMMAND_ADMIT, descriptor, normalizedEffectiveMaxMessageBytes);
    }

    static byte[] encodeUpdate(
        ZLinkClientServerServerDescriptor descriptor,
        int normalizedEffectiveMaxMessageBytes) {
        return encodeServerAdmission(
            COMMAND_UPDATE, descriptor, normalizedEffectiveMaxMessageBytes);
    }

    static byte[] encodeReject(int reason) {
        if (reason < 1 || reason > 12) {
            throw protocol("ClientServer reject reason must be in 1..12");
        }
        Writer result = prefix(COMMAND_REJECT);
        result.u32(reason);
        return result.toByteArray();
    }

    static byte[] encodeLivenessProbe(long probeId) {
        return encodeLiveness(COMMAND_LIVENESS_PROBE, probeId);
    }

    static byte[] encodeLivenessAck(long probeId) {
        return encodeLiveness(COMMAND_LIVENESS_ACK, probeId);
    }

    static boolean isControlFrame(byte[] frame) {
        return frame != null
            && frame.length >= 5
            && Byte.toUnsignedInt(frame[0]) == MAGIC_0
            && Byte.toUnsignedInt(frame[1]) == MAGIC_1
            && Byte.toUnsignedInt(frame[2]) == WIRE_MAJOR;
    }

    static Control decode(byte[] frame) {
        if (frame == null || frame.length > MAX_DESCRIPTOR_BYTES) {
            throw protocol("ClientServer control record is oversized");
        }
        Reader reader = new Reader(frame);
        if (reader.u8("magic[0]") != MAGIC_0
            || reader.u8("magic[1]") != MAGIC_1
            || reader.u8("wireMajor") != WIRE_MAJOR) {
            throw protocol("ClientServer control record prefix is invalid");
        }
        int command = reader.u8("command");
        if (reader.u8("flags") != 0) {
            throw protocol("ClientServer control flags are invalid");
        }
        if (command == COMMAND_REJECT) {
            int reason = reader.intU32("reason");
            reader.end();
            if (reason < 1 || reason > 12) {
                throw protocol("ClientServer reject reason is invalid");
            }
            return new Reject(reason);
        }
        if (command == COMMAND_LIVENESS_PROBE
            || command == COMMAND_LIVENESS_ACK) {
            long probeId = reader.nonzeroU64("probeId");
            reader.end();
            return command == COMMAND_LIVENESS_PROBE
                ? new LivenessProbe(probeId)
                : new LivenessAck(probeId);
        }
        if (command != COMMAND_HELLO
            && command != COMMAND_ADMIT
            && command != COMMAND_UPDATE) {
            throw protocol("ClientServer control command is invalid");
        }
        if (reader.u8("topologyKind") != TOPOLOGY_CLIENT_SERVER) {
            throw protocol("control record is not ClientServer topology");
        }
        long admissionLength = reader.u32("admissionLength");
        if (admissionLength != reader.remaining()) {
            throw protocol("ClientServer admission length is invalid");
        }
        int role = reader.u8("role");
        if (reader.u16("roleLength") != reader.remaining()) {
            throw protocol("ClientServer role body length is invalid");
        }
        if (command == COMMAND_HELLO) {
            if (role != ROLE_CLIENT) {
                throw protocol("ClientServer hello role is invalid");
            }
            Hello hello = new Hello(
                reader.text8("channelName"),
                requireDirection(reader),
                reader.text8("securityIdentity"),
                reader.positiveU32("normalizedEffectiveMaxMessageBytes"));
            reader.end();
            return hello;
        }
        if (role != ROLE_SERVER) {
            throw protocol("ClientServer server role is invalid");
        }
        Admission admission = new Admission(
            reader.text8("channelName"),
            requireDirection(reader),
            RoutingId.from(reader.bytes8("serverRid")),
            reader.nonzeroU64("lifecycleGeneration"),
            reader.nonzeroU64("descriptorRevision"),
            requireWeight(reader),
            stateFromWire(reader.u8("runtimeState")),
            reader.text8("securityIdentity"),
            reader.positiveU32("normalizedEffectiveMaxMessageBytes"),
            reader.text16("advertisedEndpoint"));
        reader.end();
        return command == COMMAND_ADMIT
            ? new Admit(admission)
            : new Update(admission);
    }

    private static byte[] encodeServerAdmission(
        int command,
        ZLinkClientServerServerDescriptor descriptor,
        int normalizedEffectiveMaxMessageBytes) {
        Objects.requireNonNull(descriptor, "descriptor");
        Writer body = new Writer();
        body.text8(descriptor.channelName(), "channelName");
        body.u8(DIRECTION_CLIENT_TO_SERVER);
        body.bytes8(descriptor.serverRid().toBytes(), "serverRid");
        body.nonzeroU64(
            descriptor.lifecycleGeneration(), "lifecycleGeneration");
        body.nonzeroU64(
            descriptor.descriptorRevision(), "descriptorRevision");
        body.u32(descriptor.weight());
        body.u8(stateToWire(descriptor.state()));
        body.text8(descriptor.securityIdentity(), "securityIdentity");
        body.positiveU32(
            normalizedEffectiveMaxMessageBytes,
            "normalizedEffectiveMaxMessageBytes");
        body.text16(descriptor.endpoint(), "advertisedEndpoint");
        return encodeAdmission(command, ROLE_SERVER, body.toByteArray());
    }

    private static byte[] encodeAdmission(
        int command,
        int role,
        byte[] body) {
        if (body.length > 0xffff) {
            throw protocol("ClientServer role body is oversized");
        }
        Writer admission = new Writer();
        admission.u8(role);
        admission.u16(body.length);
        admission.raw(body);
        Writer result = prefix(command);
        result.u8(TOPOLOGY_CLIENT_SERVER);
        result.u32(admission.size());
        result.raw(admission.toByteArray());
        if (result.size() > MAX_DESCRIPTOR_BYTES) {
            throw protocol("ClientServer admission is oversized");
        }
        return result.toByteArray();
    }

    private static byte[] encodeLiveness(int command, long probeId) {
        Writer result = prefix(command);
        result.nonzeroU64(probeId, "probeId");
        return result.toByteArray();
    }

    private static Writer prefix(int command) {
        Writer result = new Writer();
        result.u8(MAGIC_0);
        result.u8(MAGIC_1);
        result.u8(WIRE_MAJOR);
        result.u8(command);
        result.u8(0);
        return result;
    }

    private static int requireDirection(Reader reader) {
        int direction = reader.u8("direction");
        if (direction != DIRECTION_CLIENT_TO_SERVER) {
            throw protocol("ClientServer direction is invalid");
        }
        return direction;
    }

    private static int requireWeight(Reader reader) {
        int weight = reader.intU32("weight");
        if (weight > 10_000) {
            throw protocol("ClientServer weight is invalid");
        }
        return weight;
    }

    private static int stateToWire(ZLinkFrameworkRuntimeState state) {
        return switch (state) {
            case PREPARING -> 0;
            case SERVING -> 1;
            case RELOCATING, RELOCATED, DRAINING -> 2;
            case STOPPED -> 3;
            case ERROR -> 4;
        };
    }

    private static ZLinkFrameworkRuntimeState stateFromWire(int state) {
        return switch (state) {
            case 0 -> ZLinkFrameworkRuntimeState.PREPARING;
            case 1 -> ZLinkFrameworkRuntimeState.SERVING;
            case 2 -> ZLinkFrameworkRuntimeState.DRAINING;
            case 3 -> ZLinkFrameworkRuntimeState.STOPPED;
            case 4 -> ZLinkFrameworkRuntimeState.ERROR;
            default -> throw protocol("ClientServer runtime state is invalid");
        };
    }

    sealed interface Control permits Hello, Admit, Update, Reject,
        LivenessProbe, LivenessAck {
    }

    record Hello(
        String channelName,
        int direction,
        String securityIdentity,
        int normalizedEffectiveMaxMessageBytes) implements Control {
        Hello(
            String channelName,
            String securityIdentity,
            int normalizedEffectiveMaxMessageBytes) {
            this(
                channelName,
                DIRECTION_CLIENT_TO_SERVER,
                securityIdentity,
                normalizedEffectiveMaxMessageBytes);
        }
    }

    record Admission(
        String channelName,
        int direction,
        RoutingId serverRid,
        long lifecycleGeneration,
        long descriptorRevision,
        int weight,
        ZLinkFrameworkRuntimeState state,
        String securityIdentity,
        int normalizedEffectiveMaxMessageBytes,
        String advertisedEndpoint) {
    }

    record Admit(Admission admission) implements Control {
    }

    record Update(Admission admission) implements Control {
    }

    record Reject(int reason) implements Control {
    }

    record LivenessProbe(long probeId) implements Control {
    }

    record LivenessAck(long probeId) implements Control {
    }

    private static IllegalArgumentException protocol(String message) {
        return new IllegalArgumentException(message);
    }

    private static final class Writer {
        private final ByteArrayOutputStream output = new ByteArrayOutputStream();

        void u8(int value) {
            if (value < 0 || value > 0xff) {
                throw protocol("value exceeds u8");
            }
            output.write(value);
        }

        void u16(int value) {
            if (value < 0 || value > 0xffff) {
                throw protocol("value exceeds u16");
            }
            output.writeBytes(ByteBuffer.allocate(2)
                .order(ByteOrder.BIG_ENDIAN)
                .putShort((short) value)
                .array());
        }

        void u32(long value) {
            if (value < 0 || value > 0xffff_ffffL) {
                throw protocol("value exceeds u32");
            }
            output.writeBytes(ByteBuffer.allocate(4)
                .order(ByteOrder.BIG_ENDIAN)
                .putInt((int) value)
                .array());
        }

        void positiveU32(int value, String field) {
            if (value <= 0) {
                throw protocol(field + " must be positive");
            }
            u32(value);
        }

        void nonzeroU64(long value, String field) {
            if (value <= 0) {
                throw protocol(field + " must be non-zero");
            }
            output.writeBytes(ByteBuffer.allocate(8)
                .order(ByteOrder.BIG_ENDIAN)
                .putLong(value)
                .array());
        }

        void text8(String value, String field) {
            byte[] bytes = text(value, field);
            bytes8(bytes, field);
        }

        void bytes8(byte[] bytes, String field) {
            if (bytes == null || bytes.length == 0) {
                throw protocol(field + " must be non-empty");
            }
            if (bytes.length > 0xff) {
                throw protocol(field + " exceeds bytes8");
            }
            u8(bytes.length);
            raw(bytes);
        }

        void text16(String value, String field) {
            byte[] bytes = text(value, field);
            if (bytes.length > 4096) {
                throw protocol(field + " exceeds endpoint bound");
            }
            u16(bytes.length);
            raw(bytes);
        }

        void raw(byte[] value) {
            output.writeBytes(value);
        }

        int size() {
            return output.size();
        }

        byte[] toByteArray() {
            return output.toByteArray();
        }

        private static byte[] text(String value, String field) {
            if (value == null || value.isEmpty() || value.indexOf('\0') >= 0) {
                throw protocol(field + " must be non-empty text without NUL");
            }
            return value.getBytes(StandardCharsets.UTF_8);
        }
    }

    private static final class Reader {
        private final ByteBuffer input;

        Reader(byte[] value) {
            input = ByteBuffer.wrap(Objects.requireNonNull(value, "value"))
                .order(ByteOrder.BIG_ENDIAN);
        }

        int u8(String field) {
            require(1, field);
            return Byte.toUnsignedInt(input.get());
        }

        int u16(String field) {
            require(2, field);
            return Short.toUnsignedInt(input.getShort());
        }

        long u32(String field) {
            require(4, field);
            return Integer.toUnsignedLong(input.getInt());
        }

        int intU32(String field) {
            long value = u32(field);
            if (value > Integer.MAX_VALUE) {
                throw protocol(field + " exceeds JVM integer range");
            }
            return (int) value;
        }

        int positiveU32(String field) {
            int value = intU32(field);
            if (value <= 0) {
                throw protocol(field + " must be positive");
            }
            return value;
        }

        long nonzeroU64(String field) {
            require(8, field);
            long value = input.getLong();
            if (value <= 0) {
                throw protocol(field + " must be non-zero");
            }
            return value;
        }

        String text8(String field) {
            return decodeText(bytes8(field), field);
        }

        byte[] bytes8(String field) {
            int length = u8(field + ".length");
            if (length == 0) {
                throw protocol(field + " must be non-empty");
            }
            return bytes(length, field);
        }

        String text16(String field) {
            int length = u16(field + ".length");
            if (length == 0 || length > 4096) {
                throw protocol(field + " length is invalid");
            }
            return decodeText(bytes(length, field), field);
        }

        byte[] bytes(int length, String field) {
            require(length, field);
            byte[] result = new byte[length];
            input.get(result);
            return result;
        }

        int remaining() {
            return input.remaining();
        }

        void end() {
            if (input.hasRemaining()) {
                throw protocol("ClientServer control record has trailing bytes");
            }
        }

        private void require(int length, String field) {
            if (length < 0 || input.remaining() < length) {
                throw protocol(field + " is truncated");
            }
        }

        private static String decodeText(byte[] bytes, String field) {
            try {
                String value = StandardCharsets.UTF_8.newDecoder()
                    .onMalformedInput(CodingErrorAction.REPORT)
                    .onUnmappableCharacter(CodingErrorAction.REPORT)
                    .decode(ByteBuffer.wrap(bytes))
                    .toString();
                if (value.indexOf('\0') >= 0
                    || !java.util.Arrays.equals(
                        value.getBytes(StandardCharsets.UTF_8), bytes)) {
                    throw protocol(field + " is not canonical UTF-8");
                }
                return value;
            } catch (CharacterCodingException failure) {
                throw protocol(field + " is not canonical UTF-8");
            }
        }
    }
}
