package systems.zlink.framework.runtime.internal.service;

import java.io.ByteArrayOutputStream;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.charset.CharacterCodingException;
import java.nio.charset.CodingErrorAction;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.List;
import java.util.Objects;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.framework.runtime.protocol.ServiceWireConstants;

/** Closed M6A codec for admission, Node/Channel messaging and reply records. */
public final class ZLinkServiceM6AWireCodec {
    private static final int PREFIX_BYTES = 5;

    public byte[] encodeAdmission(
        int command,
        ZLinkServiceNodeDescriptor descriptor) {
        requireAdmissionCommand(command);
        Objects.requireNonNull(descriptor, "descriptor");
        Writer route = new Writer();
        route.text8(descriptor.meshName(), "meshName");
        route.text8(descriptor.securityIdentity(), "securityIdentity");
        route.u32(descriptor.effectiveMaxMessageBytes());
        route.u64(descriptor.lifecycleGeneration());
        route.u64(descriptor.descriptorRevision());
        route.text16(descriptor.advertisedEndpoint(), "advertisedEndpoint");
        route.u16(descriptor.channels().size());
        for (ZLinkServiceNodeDescriptor.Channel channel : descriptor.channels()) {
            route.text8(channel.name(), "channelName");
            route.u32(channel.weight());
        }

        Writer extension = new Writer();
        extension.tlv(1, new byte[] {(byte) stateToWire(descriptor.state())});
        extension.tlv(2, Writer.bytes(writer ->
            writer.u64(descriptor.applicationVersion())));
        extension.tlv(6, Writer.bytes(writer -> {
            writer.u16(descriptor.protocolCapabilities().size());
            descriptor.protocolCapabilities().forEach(
                capability -> writer.text8(
                    capability, "protocolCapability"));
        }));
        extension.tlv(7, new byte[] {(byte) roleToWire(descriptor.objectRole())});
        extension.tlv(8, Writer.bytes(
            writer -> writer.u32(descriptor.placementWeight())));
        extension.tlv(9, Writer.bytes(
            writer -> writer.u32(descriptor.activeCapacityLimit())));
        extension.tlv(10, Writer.bytes(
            writer -> writer.u32(descriptor.pendingCapacityLimit())));
        extension.tlv(11, Writer.bytes(
            writer -> writer.u32(descriptor.activeCapacityUsed())));
        extension.tlv(12, Writer.bytes(
            writer -> writer.u32(descriptor.pendingCapacityUsed())));
        route.u32(extension.size());
        route.raw(extension.toByteArray());

        Writer result = prefix(command, 0);
        result.u8(1);
        result.u32(route.size());
        result.raw(route.toByteArray());
        return result.toByteArray();
    }

    public ZLinkServiceNodeDescriptor decodeAdmission(
        byte[] frame,
        int expectedCommand,
        RoutingId sourceRoutingId) {
        requireAdmissionCommand(expectedCommand);
        Header header = decodeHeader(frame);
        if (header.command() != expectedCommand || header.flags() != 0) {
            throw protocol("unexpected admission header");
        }
        Reader reader = new Reader(frame, PREFIX_BYTES);
        if (reader.u8("topologyKind") != 1) {
            throw protocol("admission is not RouteMesh topology");
        }
        long routeLength = reader.u32("routeLength");
        if (routeLength != reader.remaining()) {
            throw protocol("RouteMesh admission length mismatch");
        }
        String meshName = reader.text8("meshName");
        String securityIdentity = reader.text8("securityIdentity");
        int effectiveMaxMessageBytes =
            reader.positiveU32("effectiveMaxMessageBytes");
        long lifecycleGeneration = reader.nonzeroU64("lifecycleGeneration");
        long descriptorRevision = reader.nonzeroU64("descriptorRevision");
        String endpoint = reader.text16("advertisedEndpoint");
        int channelCount = reader.u16("channelCount");
        List<ZLinkServiceNodeDescriptor.Channel> channels =
            new ArrayList<>(channelCount);
        for (int index = 0; index < channelCount; index++) {
            channels.add(new ZLinkServiceNodeDescriptor.Channel(
                reader.text8("channelName"),
                reader.intU32("channelWeight")));
        }
        long extensionLength = reader.u32("extensionLength");
        if (extensionLength != reader.remaining()) {
            throw protocol("descriptor extension length mismatch");
        }

        ZLinkServiceNodeDescriptor.State state = null;
        Long applicationVersion = null;
        List<String> capabilities = null;
        ZLinkServiceNodeDescriptor.ObjectRole role = null;
        int[] capacity = new int[5];
        boolean[] capacityFound = new boolean[5];
        int previousId = 0;
        while (reader.remaining() > 0) {
            int id = reader.u8("extensionId");
            int length = reader.intU32("extensionFieldLength");
            if (id <= previousId || length > reader.remaining()) {
                throw protocol("invalid descriptor TLV");
            }
            previousId = id;
            Reader value = new Reader(reader.bytes(length, "extensionValue"));
            switch (id) {
                case 1 -> state = stateFromWire(value.u8("state"));
                case 2 -> applicationVersion =
                    value.nonnegativeU64("applicationVersion");
                case 6 -> {
                    int count = value.u16("capabilityCount");
                    List<String> found = new ArrayList<>(count);
                    for (int index = 0; index < count; index++) {
                        found.add(value.text8("protocolCapability"));
                    }
                    capabilities = List.copyOf(found);
                }
                case 7 -> role = roleFromWire(value.u8("objectRole"));
                case 8, 9, 10, 11, 12 -> {
                    int offset = id - 8;
                    capacity[offset] = value.intU32("capacity");
                    capacityFound[offset] = true;
                }
                default -> value.skipRemaining();
            }
            value.end();
        }
        reader.end();
        if (state == null
            || applicationVersion == null
            || capabilities == null
            || role == null) {
            throw protocol("descriptor extension omits a required field");
        }
        for (boolean found : capacityFound) {
            if (!found) {
                throw protocol("descriptor extension omits capacity");
            }
        }
        return new ZLinkServiceNodeDescriptor(
            meshName,
            Objects.requireNonNull(sourceRoutingId, "sourceRoutingId"),
            lifecycleGeneration,
            descriptorRevision,
            endpoint,
            channels,
            state,
            securityIdentity,
            effectiveMaxMessageBytes,
            applicationVersion,
            capabilities,
            role,
            capacity[0],
            capacity[1],
            capacity[2],
            capacity[3],
            capacity[4]);
    }

    public byte[] encodeNodeSendHeader(int flags) {
        return prefix(ServiceWireConstants.COMMAND_NODE_SEND, flags).toByteArray();
    }

    public byte[] encodeNodeRequestHeader(long correlation, int flags) {
        requireCorrelation(correlation);
        Writer result = prefix(ServiceWireConstants.COMMAND_NODE_REQUEST, flags);
        result.u64(correlation);
        return result.toByteArray();
    }

    public long decodeNodeRequestHeader(byte[] frame) {
        Header header = decodeHeader(frame);
        if (header.command() != ServiceWireConstants.COMMAND_NODE_REQUEST) {
            throw protocol("invalid nodeRequest header");
        }
        Reader reader = new Reader(frame, PREFIX_BYTES);
        long correlation = reader.nonzeroU64("correlation");
        reader.end();
        return correlation;
    }

    public byte[] encodeChannelSendHeader(String channelName, int flags) {
        Writer result = prefix(ServiceWireConstants.COMMAND_CHANNEL_SEND, flags);
        result.text8(channelName, "channelName");
        return result.toByteArray();
    }

    public String decodeChannelSendHeader(byte[] frame) {
        Header header = decodeHeader(frame);
        if (header.command() != ServiceWireConstants.COMMAND_CHANNEL_SEND) {
            throw protocol("invalid channelSend header");
        }
        Reader reader = new Reader(frame, PREFIX_BYTES);
        String channelName = reader.text8("channelName");
        reader.end();
        return channelName;
    }

    public byte[] encodeChannelRequestHeader(
        long correlation,
        String channelName,
        int flags) {
        requireCorrelation(correlation);
        Writer result =
            prefix(ServiceWireConstants.COMMAND_CHANNEL_REQUEST, flags);
        result.u64(correlation);
        result.text8(channelName, "channelName");
        return result.toByteArray();
    }

    public ChannelRequest decodeChannelRequestHeader(byte[] frame) {
        Header header = decodeHeader(frame);
        if (header.command() != ServiceWireConstants.COMMAND_CHANNEL_REQUEST) {
            throw protocol("invalid channelRequest header");
        }
        Reader reader = new Reader(frame, PREFIX_BYTES);
        long correlation = reader.nonzeroU64("correlation");
        String channelName = reader.text8("channelName");
        reader.end();
        return new ChannelRequest(correlation, channelName);
    }

    public byte[] encodeReplyHeader(
        long correlation,
        int terminalResult,
        int failureCode) {
        validateReply(correlation, terminalResult, failureCode);
        Writer result = prefix(ServiceWireConstants.COMMAND_REPLY, 0);
        result.u64(correlation);
        result.u32(terminalResult);
        result.u32(failureCode);
        return result.toByteArray();
    }

    public Reply decodeReplyHeader(byte[] frame) {
        Header header = decodeHeader(frame);
        if (header.command() != ServiceWireConstants.COMMAND_REPLY
            || header.flags() != 0) {
            throw protocol("invalid reply header");
        }
        Reader reader = new Reader(frame, PREFIX_BYTES);
        long correlation = reader.nonzeroU64("correlation");
        int terminal = reader.intU32("terminalResult");
        int failure = reader.intU32("failureCode");
        reader.end();
        validateReply(correlation, terminal, failure);
        return new Reply(correlation, terminal, failure);
    }

    public byte[] encodeApplicationPayload(ApplicationPayload payload) {
        Objects.requireNonNull(payload, "payload");
        byte[] payloadBytes = payload.payloadForCodec();
        Writer body = new Writer();
        body.text8(payload.packetName(), "packetName");
        body.text8(payload.contentType(), "contentType");
        body.u32(payloadBytes.length);
        body.raw(payloadBytes);
        Writer result = new Writer();
        result.u8(1);
        result.u32(body.size());
        result.raw(body.toByteArray());
        return result.toByteArray();
    }

    /**
     * Encodes the opaque framework message parts used by cross-node
     * application delivery. The outer packet and content type are fixed by
     * the generated service-wire profile; application packet metadata stays
     * in the message parts themselves.
     */
    public static ApplicationPayload encodeFrameworkMultipart(
        List<Message> parts) {
        Objects.requireNonNull(parts, "parts");
        if (parts.isEmpty()) {
            throw protocol("framework multipart requires at least one part");
        }
        long encodedSize = Integer.BYTES;
        for (Message part : parts) {
            int partSize = Objects.requireNonNull(part, "part").size();
            try {
                encodedSize = Math.addExact(
                    encodedSize,
                    Math.addExact(Integer.BYTES, partSize));
            } catch (ArithmeticException overflow) {
                throw protocol("framework multipart payload is too large");
            }
        }
        if (encodedSize > Integer.MAX_VALUE) {
            throw protocol("framework multipart payload is too large");
        }

        Writer multipart = new Writer((int) encodedSize);
        multipart.u32(parts.size());
        for (Message part : parts) {
            byte[] bytes = Objects.requireNonNull(part, "part").toByteArray();
            multipart.u32(bytes.length);
            multipart.raw(bytes);
        }
        return new ApplicationPayload(
            ServiceWireConstants.FRAMEWORK_MULTIPART_PACKET_NAME,
            ServiceWireConstants.FRAMEWORK_MULTIPART_CONTENT_TYPE,
            multipart.toByteArray());
    }

    /**
     * Decodes the generated framework multipart profile. The part-count
     * bound is checked against the remaining byte length before allocating
     * the result list, and malformed inputs close parts already created.
     */
    public static List<Message> decodeFrameworkMultipart(
        ApplicationPayload payload) {
        Objects.requireNonNull(payload, "payload");
        if (!ServiceWireConstants.FRAMEWORK_MULTIPART_PACKET_NAME.equals(
                payload.packetName())
            || !ServiceWireConstants.FRAMEWORK_MULTIPART_CONTENT_TYPE.equals(
                payload.contentType())) {
            throw protocol("framework application payload profile is unsupported");
        }

        Reader reader = new Reader(payload.payloadForCodec());
        long count = reader.u32("frameworkMultipartPartCount");
        if (count == 0
            || count > reader.remaining() / (long) Integer.BYTES) {
            throw protocol("framework multipart part count is invalid");
        }
        List<Message> parts = new ArrayList<>((int) count);
        try {
            for (long index = 0; index < count; index++) {
                int size = reader.intU32("frameworkMultipartPartLength");
                parts.add(Message.from(
                    reader.bytes(size, "frameworkMultipartPart")));
            }
            reader.end();
            return List.copyOf(parts);
        } catch (RuntimeException failure) {
            parts.forEach(Message::close);
            throw failure;
        }
    }

    public ApplicationPayload decodeApplicationPayload(byte[] frame) {
        Reader reader = new Reader(frame);
        if (reader.u8("version") != 1) {
            throw protocol("invalid application payload version");
        }
        long bodyLength = reader.u32("bodyLength");
        if (bodyLength != reader.remaining()) {
            throw protocol("application payload body length mismatch");
        }
        String packetName = reader.text8("packetName");
        String contentType = reader.text8("contentType");
        int payloadLength = reader.intU32("payloadLength");
        if (payloadLength != reader.remaining()) {
            throw protocol("application payload length mismatch");
        }
        byte[] payload = reader.bytes(payloadLength, "payload");
        reader.end();
        return new ApplicationPayload(packetName, contentType, payload);
    }

    public Header decodeHeader(byte[] frame) {
        if (frame == null || frame.length < PREFIX_BYTES) {
            throw protocol("truncated service wire prefix");
        }
        if (Byte.toUnsignedInt(frame[0]) != ServiceWireConstants.MAGIC_0
            || Byte.toUnsignedInt(frame[1]) != ServiceWireConstants.MAGIC_1) {
            throw protocol("invalid service wire magic");
        }
        if (Byte.toUnsignedInt(frame[2]) != ServiceWireConstants.WIRE_MAJOR) {
            throw protocol("unsupported service wire major");
        }
        return new Header(
            Byte.toUnsignedInt(frame[3]),
            Byte.toUnsignedInt(frame[4]));
    }

    public byte[] encodeReject(int reason) {
        if (reason < 1 || reason > 12) {
            throw protocol("invalid reject reason");
        }
        Writer result = prefix(ServiceWireConstants.COMMAND_REJECT, 0);
        result.u32(reason);
        return result.toByteArray();
    }

    public int decodeReject(byte[] frame) {
        Header header = decodeHeader(frame);
        if (header.command() != ServiceWireConstants.COMMAND_REJECT
            || header.flags() != 0) {
            throw protocol("invalid reject record");
        }
        Reader reader = new Reader(frame, PREFIX_BYTES);
        int reason = reader.intU32("reason");
        reader.end();
        if (reason < 1 || reason > 12) {
            throw protocol("invalid reject reason");
        }
        return reason;
    }

    private static Writer prefix(int command, int flags) {
        if ((flags & ~ServiceWireConstants.FLAG_METADATA) != 0) {
            throw protocol("M6A application header contains an unknown flag");
        }
        Writer result = new Writer();
        result.u8(ServiceWireConstants.MAGIC_0);
        result.u8(ServiceWireConstants.MAGIC_1);
        result.u8(ServiceWireConstants.WIRE_MAJOR);
        result.u8(command);
        result.u8(flags);
        return result;
    }

    private static void validateReply(
        long correlation,
        int terminal,
        int failure) {
        boolean typedFailure =
            terminal == 102 || (terminal >= 104 && terminal <= 107);
        boolean validFailure =
            (failure >= 0 && failure <= 22)
                || (failure >= 33 && failure <= 35);
        if (correlation <= 0
            || (terminal != 0 && (terminal < 101 || terminal > 113))
            || !validFailure
            || (terminal == 0 && failure != 0)
            || (typedFailure && failure == 0)
            || (!typedFailure && failure != 0)) {
            throw protocol("invalid reply terminal fields");
        }
    }

    private static void requireCorrelation(long correlation) {
        if (correlation <= 0) {
            throw protocol("correlation must be non-zero");
        }
    }

    private static void requireAdmissionCommand(int command) {
        if (command != ServiceWireConstants.COMMAND_HELLO
            && command != ServiceWireConstants.COMMAND_ADMIT
            && command != ServiceWireConstants.COMMAND_UPDATE) {
            throw protocol("command is not an admission record");
        }
    }

    private static int stateToWire(ZLinkServiceNodeDescriptor.State value) {
        return value.ordinal() + 1;
    }

    private static ZLinkServiceNodeDescriptor.State stateFromWire(int value) {
        if (value < 1
            || value > ZLinkServiceNodeDescriptor.State.values().length) {
            throw protocol("invalid runtime state");
        }
        return ZLinkServiceNodeDescriptor.State.values()[value - 1];
    }

    private static int roleToWire(ZLinkServiceNodeDescriptor.ObjectRole value) {
        return value.ordinal();
    }

    private static ZLinkServiceNodeDescriptor.ObjectRole roleFromWire(int value) {
        if (value < 0
            || value >= ZLinkServiceNodeDescriptor.ObjectRole.values().length) {
            throw protocol("invalid object role");
        }
        return ZLinkServiceNodeDescriptor.ObjectRole.values()[value];
    }

    private static ZLinkServiceWireException protocol(String message) {
        return new ZLinkServiceWireException(message);
    }

    public record Header(int command, int flags) {
    }

    public record ChannelRequest(long correlation, String channelName) {
    }

    public record Reply(
        long correlation,
        int terminalResult,
        int failureCode) {
    }

    public record ApplicationPayload(
        String packetName,
        String contentType,
        byte[] payload) {
        public ApplicationPayload {
            if (packetName == null || packetName.isEmpty()) {
                throw new IllegalArgumentException("packetName is required");
            }
            if (contentType == null || contentType.isEmpty()) {
                throw new IllegalArgumentException("contentType is required");
            }
            payload = Objects.requireNonNull(payload, "payload").clone();
        }

        @Override
        public byte[] payload() {
            return payload.clone();
        }

        private byte[] payloadForCodec() {
            return payload;
        }
    }

    private static final class Writer {
        private final ByteArrayOutputStream output;

        Writer() {
            this(32);
        }

        Writer(int initialCapacity) {
            output = new ByteArrayOutputStream(initialCapacity);
        }

        static byte[] bytes(Consumer consumer) {
            Writer writer = new Writer();
            consumer.accept(writer);
            return writer.toByteArray();
        }

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

        void u64(long value) {
            if (value < 0) {
                throw protocol("value exceeds supported u64 range");
            }
            output.writeBytes(ByteBuffer.allocate(8)
                .order(ByteOrder.BIG_ENDIAN)
                .putLong(value)
                .array());
        }

        void text8(String value, String field) {
            byte[] bytes = text(value, field);
            if (bytes.length > 0xff) {
                throw protocol(field + " exceeds text8");
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

        void tlv(int id, byte[] value) {
            u8(id);
            u32(value.length);
            raw(value);
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

        @FunctionalInterface
        interface Consumer {
            void accept(Writer writer);
        }
    }

    private static final class Reader {
        private final ByteBuffer input;

        Reader(byte[] value) {
            this(value, 0);
        }

        Reader(byte[] value, int offset) {
            Objects.requireNonNull(value, "value");
            if (offset < 0 || offset > value.length) {
                throw protocol("invalid reader offset");
            }
            input = ByteBuffer.wrap(value)
                .order(ByteOrder.BIG_ENDIAN);
            input.position(offset);
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
            long value = nonnegativeU64(field);
            if (value == 0) {
                throw protocol(field + " must be non-zero");
            }
            return value;
        }

        long nonnegativeU64(String field) {
            require(8, field);
            long value = input.getLong();
            if (value < 0) {
                throw protocol(field + " exceeds supported JVM u64 range");
            }
            return value;
        }

        String text8(String field) {
            int length = u8(field + ".length");
            if (length == 0) {
                throw protocol(field + " must be non-empty");
            }
            return decodeText(bytes(length, field), field);
        }

        String text16(String field) {
            int length = u16(field + ".length");
            if (length == 0 || length > 4096) {
                throw protocol(field + " has invalid length");
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

        void skipRemaining() {
            input.position(input.limit());
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

        private static String decodeText(byte[] bytes, String field) {
            try {
                String value = StandardCharsets.UTF_8.newDecoder()
                    .onMalformedInput(CodingErrorAction.REPORT)
                    .onUnmappableCharacter(CodingErrorAction.REPORT)
                    .decode(ByteBuffer.wrap(bytes))
                    .toString();
                if (value.indexOf('\0') >= 0
                    || !java.util.Arrays.equals(
                        value.getBytes(StandardCharsets.UTF_8),
                        bytes)) {
                    throw protocol(field + " is not canonical UTF-8");
                }
                return value;
            } catch (CharacterCodingException failure) {
                throw protocol(field + " is not canonical UTF-8");
            }
        }
    }
}
