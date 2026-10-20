package systems.zlink.framework.runtime.internal.service;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.framework.runtime.protocol.ServiceWireConstants;

import java.io.ByteArrayOutputStream;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.List;
import java.util.Objects;

/** Closed M6A codec for admission, Node/Channel messaging and reply records. */
public final class ZLinkServiceM6AWireCodec {
    public static final int IDENTITY_MISMATCH_REJECT_REASON =
            (int) ServiceWireCodec.RejectReason.IDENTITY_MISMATCH.wire;
    private static final int MAX_ENDPOINT_BYTES = 4096;
    private static final int FIELD_STATE = 1;
    private static final int FIELD_APPLICATION_VERSION = 2;
    private static final int FIELD_PROTOCOL_CAPABILITIES = 6;
    private static final int FIELD_OBJECT_ROLE = 7;
    private static final int FIELD_PLACEMENT_WEIGHT = 8;
    private static final int FIELD_ACTIVE_CAPACITY_LIMIT = 9;
    private static final int FIELD_PENDING_CAPACITY_LIMIT = 10;
    private static final int FIELD_ACTIVE_CAPACITY_USED = 11;
    private static final int FIELD_PENDING_CAPACITY_USED = 12;
    private static final ZLinkServiceNodeDescriptor.ObjectRole[] ROLES =
            ZLinkServiceNodeDescriptor.ObjectRole.values();
    private static final int APPLICATION_PREFIX_BYTES = 1 + Integer.BYTES;
    private static final byte[] FRAMEWORK_MULTIPART_PROFILE_FIELDS =
            applicationProfileFields(
                    ServiceWireConstants.FRAMEWORK_MULTIPART_PACKET_NAME,
                    ServiceWireConstants.FRAMEWORK_MULTIPART_CONTENT_TYPE);

    public byte[] encodeAdmission(int command, ZLinkServiceNodeDescriptor descriptor) {
        requireAdmissionCommand(command);
        Objects.requireNonNull(descriptor, "descriptor");
        Writer route = new Writer();
        route.text8(descriptor.meshName(), "meshName");
        route.text8(descriptor.securityIdentity(), "securityIdentity");
        //  lifecycleGeneration is an opaque equality token over the full
        //  unsigned 64-bit range (spec 13 §7.1); descriptorRevision is
        //  ordered and stays range-checked.
        route.opaqueU64(descriptor.lifecycleGeneration());
        route.u64(descriptor.descriptorRevision());
        route.text16(descriptor.advertisedEndpoint(), "advertisedEndpoint");
        route.u16(descriptor.channels().size());
        for (ZLinkServiceNodeDescriptor.Channel channel : descriptor.channels()) {
            route.text8(channel.name(), "channelName");
            route.u32(channel.weight());
        }

        Writer extension = new Writer();
        extension.tlv(FIELD_STATE, new byte[] {(byte) stateToWire(descriptor.state())});
        extension.tlv(
                FIELD_APPLICATION_VERSION,
                Writer.bytes(writer -> writer.u64(descriptor.applicationVersion())));
        extension.tlv(
                FIELD_PROTOCOL_CAPABILITIES,
                Writer.bytes(
                        writer -> {
                            writer.u16(descriptor.protocolCapabilities().size());
                            descriptor
                                    .protocolCapabilities()
                                    .forEach(
                                            capability ->
                                                    writer.text8(capability, "protocolCapability"));
                        }));
        extension.tlv(FIELD_OBJECT_ROLE, new byte[] {(byte) roleToWire(descriptor.objectRole())});
        extension.tlv(
                FIELD_PLACEMENT_WEIGHT,
                Writer.bytes(writer -> writer.u32(descriptor.placementWeight())));
        extension.tlv(
                FIELD_ACTIVE_CAPACITY_LIMIT,
                Writer.bytes(writer -> writer.u32(descriptor.activeCapacityLimit())));
        extension.tlv(
                FIELD_PENDING_CAPACITY_LIMIT,
                Writer.bytes(writer -> writer.u32(descriptor.pendingCapacityLimit())));
        extension.tlv(
                FIELD_ACTIVE_CAPACITY_USED,
                Writer.bytes(writer -> writer.u32(descriptor.activeCapacityUsed())));
        extension.tlv(
                FIELD_PENDING_CAPACITY_USED,
                Writer.bytes(writer -> writer.u32(descriptor.pendingCapacityUsed())));
        route.u32(extension.size());
        route.raw(extension.toByteArray());

        return prefix(command, 0, Byte.BYTES + Integer.BYTES + route.size())
                .put((byte) 1)
                .putInt(route.size())
                .put(route.toByteArray())
                .array();
    }

    public ZLinkServiceNodeDescriptor decodeAdmission(
            byte[] frame, int expectedCommand, RoutingId sourceRoutingId) {
        requireAdmissionCommand(expectedCommand);
        int header = decodeHeaderFields(frame);
        if ((header >>> Byte.SIZE) != expectedCommand || (header & 0xff) != 0) {
            throw protocol("unexpected admission header");
        }
        ByteBuffer reader = Reader.view(frame, ZLinkServiceWireCodec.PREFIX_BYTES);
        if (Reader.u8(reader, "topologyKind") != 1) {
            throw protocol("admission is not RouteMesh topology");
        }
        long routeLength = Reader.u32(reader, "routeLength");
        if (routeLength != reader.remaining()) {
            throw protocol("RouteMesh admission length mismatch");
        }
        String meshName = Reader.text8(reader, "meshName");
        String securityIdentity = Reader.text8(reader, "securityIdentity");
        //  spec 13 §7.1 / service-wire-v1.schema.json `nonzero-u64`: the
        //  lifecycle generation is an OPAQUE equality token produced by a
        //  CSPRNG -- "which lifecycle is newer isn't judged by numeric
        //  magnitude". Every other language carries it as a full unsigned
        //  64-bit value (C++ std::uint64_t, Node bigint, .NET ulong), so
        //  roughly half of all generated tokens have bit 63 set. Rejecting
        //  those as "out of JVM range" made admission against a peer fail
        //  about half the time, at random, per process lifetime. Java keeps
        //  the raw 64-bit pattern in a long and only ever compares it for
        //  equality, which is exactly the contract.
        long lifecycleGeneration = Reader.opaqueNonzeroU64(reader, "lifecycleGeneration");
        long descriptorRevision = Reader.nonzeroU64(reader, "descriptorRevision");
        String endpoint = Reader.text16(reader, "advertisedEndpoint");
        int channelCount = Reader.u16(reader, "channelCount");
        List<ZLinkServiceNodeDescriptor.Channel> channels = new ArrayList<>(channelCount);
        for (int index = 0; index < channelCount; index++) {
            channels.add(
                    new ZLinkServiceNodeDescriptor.Channel(
                            Reader.text8(reader, "channelName"),
                            Reader.intU32(reader, "channelWeight")));
        }
        long extensionLength = Reader.u32(reader, "extensionLength");
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
            int id = Reader.u8(reader, "extensionId");
            int length = Reader.intU32(reader, "extensionFieldLength");
            if (id <= previousId || length > reader.remaining()) {
                throw protocol("invalid descriptor TLV");
            }
            previousId = id;
            ByteBuffer value = Reader.view(Reader.bytes(reader, length, "extensionValue"));
            switch (id) {
                case FIELD_STATE -> state = stateFromWire(Reader.u8(value, "state"));
                case FIELD_APPLICATION_VERSION ->
                        applicationVersion = Reader.nonnegativeU64(value, "applicationVersion");
                case FIELD_PROTOCOL_CAPABILITIES -> {
                    int count = Reader.u16(value, "capabilityCount");
                    List<String> found = new ArrayList<>(count);
                    for (int index = 0; index < count; index++) {
                        found.add(Reader.text8(value, "protocolCapability"));
                    }
                    capabilities = List.copyOf(found);
                }
                case FIELD_OBJECT_ROLE -> role = roleFromWire(Reader.u8(value, "objectRole"));
                case FIELD_PLACEMENT_WEIGHT,
                        FIELD_ACTIVE_CAPACITY_LIMIT,
                        FIELD_PENDING_CAPACITY_LIMIT,
                        FIELD_ACTIVE_CAPACITY_USED,
                        FIELD_PENDING_CAPACITY_USED -> {
                    int offset = id - FIELD_PLACEMENT_WEIGHT;
                    capacity[offset] = Reader.intU32(value, "capacity");
                    capacityFound[offset] = true;
                }
                default -> Reader.skipRemaining(value);
            }
            Reader.end(value);
        }
        Reader.end(reader);
        if (state == null || applicationVersion == null || capabilities == null || role == null) {
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
        return prefix(ServiceWireConstants.COMMAND_NODE_SEND, flags, 0).array();
    }

    public byte[] encodeNodeRequestHeader(long correlation, int flags) {
        requireCorrelation(correlation);
        return prefix(ServiceWireConstants.COMMAND_NODE_REQUEST, flags, Long.BYTES)
                .putLong(correlation)
                .array();
    }

    public long decodeNodeRequestHeader(byte[] frame) {
        int header = decodeHeaderFields(frame);
        if ((header >>> Byte.SIZE) != ServiceWireConstants.COMMAND_NODE_REQUEST) {
            throw protocol("invalid nodeRequest header");
        }
        ByteBuffer reader = Reader.view(frame, ZLinkServiceWireCodec.PREFIX_BYTES);
        long correlation = Reader.opaqueNonzeroU64(reader, "correlation");
        Reader.end(reader);
        return correlation;
    }

    public byte[] encodeChannelSendHeader(String channelName, int flags) {
        byte[] channel = Writer.text8Bytes(channelName, "channelName");
        return prefix(ServiceWireConstants.COMMAND_CHANNEL_SEND, flags, Byte.BYTES + channel.length)
                .put((byte) channel.length)
                .put(channel)
                .array();
    }

    public String decodeChannelSendHeader(byte[] frame) {
        int header = decodeHeaderFields(frame);
        if ((header >>> Byte.SIZE) != ServiceWireConstants.COMMAND_CHANNEL_SEND) {
            throw protocol("invalid channelSend header");
        }
        ByteBuffer reader = Reader.view(frame, ZLinkServiceWireCodec.PREFIX_BYTES);
        String channelName = Reader.text8(reader, "channelName");
        Reader.end(reader);
        return channelName;
    }

    public byte[] encodeChannelRequestHeader(long correlation, String channelName, int flags) {
        requireCorrelation(correlation);
        byte[] channel = Writer.text8Bytes(channelName, "channelName");
        return prefix(
                        ServiceWireConstants.COMMAND_CHANNEL_REQUEST,
                        flags,
                        Long.BYTES + Byte.BYTES + channel.length)
                .putLong(correlation)
                .put((byte) channel.length)
                .put(channel)
                .array();
    }

    public ChannelRequest decodeChannelRequestHeader(byte[] frame) {
        int header = decodeHeaderFields(frame);
        if ((header >>> Byte.SIZE) != ServiceWireConstants.COMMAND_CHANNEL_REQUEST) {
            throw protocol("invalid channelRequest header");
        }
        ByteBuffer reader = Reader.view(frame, ZLinkServiceWireCodec.PREFIX_BYTES);
        long correlation = Reader.opaqueNonzeroU64(reader, "correlation");
        String channelName = Reader.text8(reader, "channelName");
        Reader.end(reader);
        return new ChannelRequest(correlation, channelName);
    }

    public byte[] encodeReplyHeader(long correlation, int terminalResult, int failureCode) {
        validateReply(correlation, terminalResult, failureCode);
        return prefix(ServiceWireConstants.COMMAND_REPLY, 0, Long.BYTES + 2 * Integer.BYTES)
                .putLong(correlation)
                .putInt(terminalResult)
                .putInt(failureCode)
                .array();
    }

    public Reply decodeReplyHeader(byte[] frame) {
        int header = decodeHeaderFields(frame);
        if ((header >>> Byte.SIZE) != ServiceWireConstants.COMMAND_REPLY || (header & 0xff) != 0) {
            throw protocol("invalid reply header");
        }
        //  service-wire-v1.schema.json: reply(20).tail -> `request-specific-tail`,
        //  a conditional-union with NO `bodyLengthType`, so any inline tail
        //  bytes for the selected case (keyed on the original operation kind,
        //  which is not recoverable from this generic header) follow
        //  immediately after failureCode. This generic decoder therefore only
        //  requires the fixed 16-byte body to be present; trailing bytes
        //  belong to the tail and are validated by the operation-specific
        //  reply decoder, matching Node/.NET's decodeReplyHeader semantics.
        //  Do not call Reader.end(reader) here: it would reject a reply that
        //  carries a non-empty tail.
        ByteBuffer reader = Reader.view(frame, ZLinkServiceWireCodec.PREFIX_BYTES);
        long correlation = Reader.opaqueNonzeroU64(reader, "correlation");
        int terminal = Reader.intU32(reader, "terminalResult");
        int failure = Reader.intU32(reader, "failureCode");
        validateReply(correlation, terminal, failure);
        return new Reply(correlation, terminal, failure);
    }

    public byte[] encodeApplicationPayload(ApplicationPayload payload) {
        Objects.requireNonNull(payload, "payload");
        ByteBuffer payloadBytes = payload.payloadForCodec();
        ByteBuffer result =
                applicationFrame(
                        payload.packetName(), payload.contentType(), payloadBytes.remaining());
        result.put(payloadBytes);
        return result.array();
    }

    private static ByteBuffer applicationFrame(String packet, String content, int payloadLength) {
        byte[] profileFields = applicationProfileFields(packet, content);
        ByteBuffer result = ByteBuffer.allocate(applicationFrameSize(profileFields, payloadLength));
        writeApplicationFrameHeader(result, profileFields, payloadLength);
        return result;
    }

    /**
     * Encodes the opaque framework message parts used by cross-node application delivery. The outer
     * packet and content type are fixed by the generated service-wire profile; application packet
     * metadata stays in the message parts themselves.
     */
    public static ApplicationPayload encodeFrameworkMultipart(List<Message> parts) {
        ByteBuffer multipart = ByteBuffer.allocate(multipartSize(parts));
        writeMultipart(multipart, parts);
        return new ApplicationPayload(
                ServiceWireConstants.FRAMEWORK_MULTIPART_PACKET_NAME,
                ServiceWireConstants.FRAMEWORK_MULTIPART_CONTENT_TYPE,
                multipart.flip());
    }

    /** Writes the complete application frame without an intermediate payload. */
    public static byte[] encodeFrameworkMultipartFrame(List<Message> parts) {
        int multipartSize = multipartSize(parts);
        ByteBuffer frame = ByteBuffer.allocate(frameworkMultipartFrameSize(multipartSize));
        writeFrameworkMultipartFrame(frame, parts, multipartSize);
        return frame.array();
    }

    /** Allocates and writes the complete application frame in native storage. */
    public static Message encodeFrameworkMultipartMessage(List<Message> parts) {
        int multipartSize = multipartSize(parts);
        Message frame = Message.allocate(frameworkMultipartFrameSize(multipartSize));
        boolean encoded = false;
        try {
            writeFrameworkMultipartFrame(
                    frame.mutableDataBuffer().order(ByteOrder.BIG_ENDIAN), parts, multipartSize);
            encoded = true;
            return frame;
        } finally {
            if (!encoded) {
                frame.close();
            }
        }
    }

    private static int frameworkMultipartFrameSize(int multipartSize) {
        return applicationFrameSize(FRAMEWORK_MULTIPART_PROFILE_FIELDS, multipartSize);
    }

    private static void writeFrameworkMultipartFrame(
            ByteBuffer target, List<Message> parts, int multipartSize) {
        writeApplicationFrameHeader(target, FRAMEWORK_MULTIPART_PROFILE_FIELDS, multipartSize);
        writeMultipart(target, parts);
    }

    private static int applicationFrameSize(byte[] profileFields, int payloadLength) {
        if (payloadLength < 0) {
            throw protocol("application payload length is invalid");
        }
        long frameSize =
                APPLICATION_PREFIX_BYTES + profileFields.length + Integer.BYTES + payloadLength;
        if (frameSize > Integer.MAX_VALUE) {
            throw protocol("application payload is too large");
        }
        return (int) frameSize;
    }

    private static void writeApplicationFrameHeader(
            ByteBuffer target, byte[] profileFields, int payloadLength) {
        int frameSize = applicationFrameSize(profileFields, payloadLength);
        target.put((byte) 1).putInt(frameSize - APPLICATION_PREFIX_BYTES);
        target.put(profileFields);
        target.putInt(payloadLength);
    }

    private static byte[] applicationProfileFields(String packet, String content) {
        byte[] packetName = Writer.text(packet, "packetName");
        byte[] contentType = Writer.text(content, "contentType");
        if (packetName.length > 0xff || contentType.length > 0xff) {
            throw protocol("application payload text exceeds text8");
        }
        return ByteBuffer.allocate(1 + packetName.length + 1 + contentType.length)
                .put((byte) packetName.length)
                .put(packetName)
                .put((byte) contentType.length)
                .put(contentType)
                .array();
    }

    private static int multipartSize(List<Message> parts) {
        Objects.requireNonNull(parts, "parts");
        if (parts.isEmpty()) {
            throw protocol("framework multipart requires at least one part");
        }
        long encodedSize = Integer.BYTES;
        for (int index = 0; index < parts.size(); index++) {
            Message part = parts.get(index);
            encodedSize += Integer.BYTES + (long) Objects.requireNonNull(part, "part").size();
            if (encodedSize > Integer.MAX_VALUE) {
                throw protocol("framework multipart payload is too large");
            }
        }
        return (int) encodedSize;
    }

    private static void writeMultipart(ByteBuffer target, List<Message> parts) {
        target.putInt(parts.size());
        for (int index = 0; index < parts.size(); index++) {
            Message part = parts.get(index);
            target.putInt(part.size());
            part.copyTo(target);
        }
    }

    /**
     * Decodes the generated framework multipart profile. The part-count bound is checked against
     * the remaining byte length before allocating the result list, and malformed inputs close parts
     * already created.
     */
    public static List<Message> decodeFrameworkMultipart(ApplicationPayload payload) {
        Objects.requireNonNull(payload, "payload");
        return decodeMultipartView(
                        payload.packetName(),
                        payload.contentType(),
                        Reader.view(payload.payloadForCodec()))
                .detachAll();
    }

    /** Borrows the frame for decoding; returned parts own their binding storage. */
    public static List<Message> decodeFrameworkMultipartFrame(ByteBuffer frame) {
        return decodeFrameworkMultipartFrameView(frame).detachAll();
    }

    /**
     * Borrows one received frame and validates the multipart profile without copying its inner
     * parts. The caller must keep the frame owner alive until the returned view is closed.
     */
    public static ZLinkFrameworkMultipartView decodeFrameworkMultipartFrameView(ByteBuffer frame) {
        ByteBuffer reader = Reader.view(frame);
        if (Reader.u8(reader, "version") != 1) {
            throw protocol("invalid application payload version");
        }
        if (Reader.u32(reader, "bodyLength") != reader.remaining()) {
            throw protocol("application payload body length mismatch");
        }
        String packetName;
        String contentType;
        // The generated profile's two text8 fields are fixed bytes; compare them in place and
        // decode text only for any other profile, which the view check then rejects.
        if (Reader.skipIfEquals(reader, FRAMEWORK_MULTIPART_PROFILE_FIELDS)) {
            packetName = ServiceWireConstants.FRAMEWORK_MULTIPART_PACKET_NAME;
            contentType = ServiceWireConstants.FRAMEWORK_MULTIPART_CONTENT_TYPE;
        } else {
            packetName = Reader.text8(reader, "packetName");
            contentType = Reader.text8(reader, "contentType");
        }
        if (Reader.intU32(reader, "payloadLength") != reader.remaining()) {
            throw protocol("application payload length mismatch");
        }
        return decodeMultipartView(packetName, contentType, reader);
    }

    private static ZLinkFrameworkMultipartView decodeMultipartView(
            String packetName, String contentType, ByteBuffer reader) {
        if (!ServiceWireConstants.FRAMEWORK_MULTIPART_PACKET_NAME.equals(packetName)
                || !ServiceWireConstants.FRAMEWORK_MULTIPART_CONTENT_TYPE.equals(contentType)) {
            throw protocol("framework application payload profile is unsupported");
        }

        long count = Reader.u32(reader, "frameworkMultipartPartCount");
        if (count == 0 || count > reader.remaining() / (long) Integer.BYTES) {
            throw protocol("framework multipart part count is invalid");
        }
        Object[] parts = new Object[(int) count];
        for (long index = 0; index < count; index++) {
            int size = Reader.intU32(reader, "frameworkMultipartPartLength");
            parts[(int) index] = Reader.buffer(reader, size, "frameworkMultipartPart");
        }
        Reader.end(reader);
        return new ZLinkFrameworkMultipartView(parts);
    }

    public ApplicationPayload decodeApplicationPayload(byte[] frame) {
        return decodeApplicationPayload(ByteBuffer.wrap(frame.clone()));
    }

    /** Borrows the frame; its owner must outlive payload decoding or encoding. */
    public ApplicationPayload decodeApplicationPayload(ByteBuffer frame) {
        ByteBuffer reader = Reader.view(frame);
        if (Reader.u8(reader, "version") != 1) {
            throw protocol("invalid application payload version");
        }
        long bodyLength = Reader.u32(reader, "bodyLength");
        if (bodyLength != reader.remaining()) {
            throw protocol("application payload body length mismatch");
        }
        String packetName = Reader.text8(reader, "packetName");
        String contentType = Reader.text8(reader, "contentType");
        int payloadLength = Reader.intU32(reader, "payloadLength");
        if (payloadLength != reader.remaining()) {
            throw protocol("application payload length mismatch");
        }
        ByteBuffer payload = Reader.buffer(reader, payloadLength, "payload");
        Reader.end(reader);
        return new ApplicationPayload(packetName, contentType, payload);
    }

    public Header decodeHeader(byte[] frame) {
        int header = decodeHeaderFields(frame);
        return new Header(header >>> Byte.SIZE, header & 0xff);
    }

    private static int decodeHeaderFields(byte[] frame) {
        if (frame == null || frame.length < ZLinkServiceWireCodec.PREFIX_BYTES) {
            throw protocol("truncated service wire prefix");
        }
        if (Byte.toUnsignedInt(frame[0]) != ServiceWireConstants.MAGIC_0
                || Byte.toUnsignedInt(frame[1]) != ServiceWireConstants.MAGIC_1) {
            throw protocol("invalid service wire magic");
        }
        if (Byte.toUnsignedInt(frame[2]) != ServiceWireConstants.WIRE_MAJOR) {
            throw protocol("unsupported service wire major");
        }
        return (Byte.toUnsignedInt(frame[3]) << Byte.SIZE) | Byte.toUnsignedInt(frame[4]);
    }

    public byte[] encodeReject(int reason) {
        if (reason < 1 || reason > 12) {
            throw protocol("invalid reject reason");
        }
        return prefix(ServiceWireConstants.COMMAND_REJECT, 0, Integer.BYTES).putInt(reason).array();
    }

    public int decodeReject(byte[] frame) {
        int header = decodeHeaderFields(frame);
        if ((header >>> Byte.SIZE) != ServiceWireConstants.COMMAND_REJECT || (header & 0xff) != 0) {
            throw protocol("invalid reject record");
        }
        ByteBuffer reader = Reader.view(frame, ZLinkServiceWireCodec.PREFIX_BYTES);
        int reason = Reader.intU32(reader, "reason");
        Reader.end(reader);
        if (reason < 1 || reason > 12) {
            throw protocol("invalid reject reason");
        }
        return reason;
    }

    private static ByteBuffer prefix(int command, int flags, int bodySize) {
        if ((flags & ~ServiceWireConstants.FLAG_METADATA) != 0) {
            throw protocol("M6A application header contains an unknown flag");
        }
        return ByteBuffer.allocate(ZLinkServiceWireCodec.PREFIX_BYTES + bodySize)
                .order(ByteOrder.BIG_ENDIAN)
                .put((byte) ServiceWireConstants.MAGIC_0)
                .put((byte) ServiceWireConstants.MAGIC_1)
                .put((byte) ServiceWireConstants.WIRE_MAJOR)
                .put((byte) command)
                .put((byte) flags);
    }

    private static void validateReply(long correlation, int terminal, int failure) {
        //  Schema terminal-failure-integrity (spec 51-internal-service-wire
        //  -protocol:43-47, service-wire-v1.schema.json): success is ok+none,
        //  boundary terminals carry none, a typed failure code must match its
        //  exact schema terminal, and an unknown failure code is rejected as a
        //  protocol error before dispatch. The generated predicate is the
        //  single source of that pairing table.
        if (correlation == 0 || !ServiceWireConstants.validTerminalFailure(terminal, failure)) {
            throw protocol("invalid reply terminal fields");
        }
    }

    private static void requireCorrelation(long correlation) {
        if (correlation == 0) {
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

    //  service-wire-v1.schema.json `runtime-state` (u8): preparing=0,
    //  serving=1, draining=2, stopped=3, error=4. The wire has no `retiring`
    //  value -- it is a host-internal transition whose remote admission
    //  meaning is `draining`, exactly as C++ (runtime_state_wire) and Node
    //  (stateToWire) encode it. Deriving the value from the Java enum's
    //  ordinal shifted every state by one and made a SERVING Java node
    //  advertise itself as `draining` to every peer, which kept the peer out
    //  of the remote ready-peer set forever.
    private static int stateToWire(ZLinkServiceNodeDescriptor.State value) {
        return switch (value) {
            case PREPARING -> (int) ServiceWireCodec.RuntimeState.PREPARING.wire;
            case SERVING -> (int) ServiceWireCodec.RuntimeState.SERVING.wire;
            case RETIRING, DRAINING -> (int) ServiceWireCodec.RuntimeState.DRAINING.wire;
            case STOPPED -> (int) ServiceWireCodec.RuntimeState.STOPPED.wire;
            case ERROR -> (int) ServiceWireCodec.RuntimeState.ERROR.wire;
        };
    }

    private static ZLinkServiceNodeDescriptor.State stateFromWire(int value) {
        if (value == ServiceWireCodec.RuntimeState.PREPARING.wire) {
            return ZLinkServiceNodeDescriptor.State.PREPARING;
        }
        if (value == ServiceWireCodec.RuntimeState.SERVING.wire) {
            return ZLinkServiceNodeDescriptor.State.SERVING;
        }
        if (value == ServiceWireCodec.RuntimeState.DRAINING.wire) {
            return ZLinkServiceNodeDescriptor.State.DRAINING;
        }
        if (value == ServiceWireCodec.RuntimeState.STOPPED.wire) {
            return ZLinkServiceNodeDescriptor.State.STOPPED;
        }
        if (value == ServiceWireCodec.RuntimeState.ERROR.wire) {
            return ZLinkServiceNodeDescriptor.State.ERROR;
        }
        throw protocol("invalid runtime state");
    }

    private static int roleToWire(ZLinkServiceNodeDescriptor.ObjectRole value) {
        return value.ordinal();
    }

    private static ZLinkServiceNodeDescriptor.ObjectRole roleFromWire(int value) {
        if (value < 0 || value >= ROLES.length) {
            throw protocol("invalid object role");
        }
        return ROLES[value];
    }

    private static ZLinkServiceWireException protocol(String message) {
        return new ZLinkServiceWireException(message);
    }

    public record Header(int command, int flags) {}

    public record ChannelRequest(long correlation, String channelName) {}

    public record Reply(long correlation, int terminalResult, int failureCode) {}

    public static final class ApplicationPayload {
        private final String packetName;
        private final String contentType;
        private final ByteBuffer payload;

        public ApplicationPayload(String packetName, String contentType, byte[] payload) {
            this(
                    packetName,
                    contentType,
                    ByteBuffer.wrap(Objects.requireNonNull(payload, "payload").clone()));
        }

        private ApplicationPayload(String packetName, String contentType, ByteBuffer payload) {
            if (packetName == null || packetName.isEmpty()) {
                throw new IllegalArgumentException("packetName is required");
            }
            if (contentType == null || contentType.isEmpty()) {
                throw new IllegalArgumentException("contentType is required");
            }
            this.packetName = packetName;
            this.contentType = contentType;
            this.payload = payload.slice().asReadOnlyBuffer();
        }

        public String packetName() {
            return packetName;
        }

        public String contentType() {
            return contentType;
        }

        public byte[] payload() {
            byte[] result = new byte[payload.remaining()];
            payloadForCodec().get(result);
            return result;
        }

        private ByteBuffer payloadForCodec() {
            return payload.asReadOnlyBuffer();
        }
    }

    private static final class Writer {
        private final ByteArrayOutputStream output = new ByteArrayOutputStream();

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
            output.write(value >>> 8);
            output.write(value);
        }

        void u32(long value) {
            if (value < 0 || value > 0xffff_ffffL) {
                throw protocol("value exceeds u32");
            }
            output.write((int) (value >>> 24));
            output.write((int) (value >>> 16));
            output.write((int) (value >>> 8));
            output.write((int) value);
        }

        void u64(long value) {
            if (value < 0) {
                throw protocol("value exceeds supported u64 range");
            }
            opaqueU64(value);
        }

        /**
         * Writes the raw 64-bit pattern of an opaque `nonzero-u64` equality token (see {@link
         * Reader#opaqueNonzeroU64}). Fields that carry a magnitude use the range-checked {@link
         * #u64} instead.
         */
        void opaqueU64(long value) {
            for (int shift = Long.SIZE - Byte.SIZE; shift >= 0; shift -= Byte.SIZE) {
                output.write((int) (value >>> shift));
            }
        }

        void text8(String value, String field) {
            byte[] bytes = text8Bytes(value, field);
            u8(bytes.length);
            raw(bytes);
        }

        private static byte[] text8Bytes(String value, String field) {
            byte[] bytes = text(value, field);
            if (bytes.length > 0xff) {
                throw protocol(field + " exceeds text8");
            }
            return bytes;
        }

        void text16(String value, String field) {
            byte[] bytes = text(value, field);
            if (bytes.length > MAX_ENDPOINT_BYTES) {
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
        private Reader() {}

        static ByteBuffer view(byte[] value) {
            return view(value, 0);
        }

        static ByteBuffer view(ByteBuffer value) {
            return Objects.requireNonNull(value, "value").slice().order(ByteOrder.BIG_ENDIAN);
        }

        static ByteBuffer view(byte[] value, int offset) {
            Objects.requireNonNull(value, "value");
            if (offset < 0 || offset > value.length) {
                throw protocol("invalid reader offset");
            }
            return ByteBuffer.wrap(value).order(ByteOrder.BIG_ENDIAN).position(offset);
        }

        static int u8(ByteBuffer input, String field) {
            require(input, 1, field);
            return Byte.toUnsignedInt(input.get());
        }

        static int u16(ByteBuffer input, String field) {
            require(input, 2, field);
            return Short.toUnsignedInt(input.getShort());
        }

        static long u32(ByteBuffer input, String field) {
            require(input, 4, field);
            return Integer.toUnsignedLong(input.getInt());
        }

        static int intU32(ByteBuffer input, String field) {
            long value = u32(input, field);
            if (value > Integer.MAX_VALUE) {
                throw protocol(field + " exceeds JVM integer range");
            }
            return (int) value;
        }

        static long nonzeroU64(ByteBuffer input, String field) {
            long value = nonnegativeU64(input, field);
            if (value == 0) {
                throw protocol(field + " must be non-zero");
            }
            return value;
        }

        /**
         * Reads a `nonzero-u64` whose value is an opaque equality token: the full unsigned 64-bit
         * range is valid and the raw bit pattern is kept in a long. Only fields that are compared
         * for ORDER (such as descriptorRevision) may use the range-checked {@link #nonzeroU64}.
         */
        static long opaqueNonzeroU64(ByteBuffer input, String field) {
            require(input, 8, field);
            long value = input.getLong();
            if (value == 0) {
                throw protocol(field + " must be non-zero");
            }
            return value;
        }

        static long nonnegativeU64(ByteBuffer input, String field) {
            require(input, 8, field);
            long value = input.getLong();
            if (value < 0) {
                throw protocol(field + " exceeds supported JVM u64 range");
            }
            return value;
        }

        static String text8(ByteBuffer input, String field) {
            if (!input.hasRemaining()) {
                throw protocol("truncated " + field + ".length");
            }
            int length = Byte.toUnsignedInt(input.get());
            if (length == 0) {
                throw protocol(field + " must be non-empty");
            }
            return decodeText(bytes(input, length, field), field);
        }

        static String text16(ByteBuffer input, String field) {
            if (input.remaining() < Short.BYTES) {
                throw protocol("truncated " + field + ".length");
            }
            int length = Short.toUnsignedInt(input.getShort());
            if (length == 0 || length > MAX_ENDPOINT_BYTES) {
                throw protocol(field + " has invalid length");
            }
            return decodeText(bytes(input, length, field), field);
        }

        /** Consumes {@code expected} when the next bytes equal it; otherwise reads nothing. */
        static boolean skipIfEquals(ByteBuffer input, byte[] expected) {
            int start = input.position();
            if (input.remaining() < expected.length) {
                return false;
            }
            for (int index = 0; index < expected.length; index++) {
                if (input.get(start + index) != expected[index]) {
                    return false;
                }
            }
            input.position(start + expected.length);
            return true;
        }

        static byte[] bytes(ByteBuffer input, int length, String field) {
            require(input, length, field);
            byte[] result = new byte[length];
            input.get(result);
            return result;
        }

        static ByteBuffer buffer(ByteBuffer input, int length, String field) {
            require(input, length, field);
            ByteBuffer result = input.slice(input.position(), length);
            input.position(input.position() + length);
            return result;
        }

        static void skipRemaining(ByteBuffer input) {
            input.position(input.limit());
        }

        static void end(ByteBuffer input) {
            if (input.hasRemaining()) {
                throw protocol("trailing bytes are forbidden");
            }
        }

        private static void require(ByteBuffer input, int length, String field) {
            if (length < 0 || input.remaining() < length) {
                throw protocol("truncated " + field);
            }
        }

        private static String decodeText(byte[] bytes, String field) {
            if (!isCanonicalUtf8WithoutNul(bytes)) {
                throw protocol(field + " is not canonical UTF-8");
            }
            return new String(bytes, StandardCharsets.UTF_8);
        }

        /**
         * Well-formed UTF-8 (RFC 3629: shortest form, no surrogates, at most U+10FFFF) and no NUL.
         * These are exactly the inputs that decode and re-encode to the same bytes.
         */
        private static boolean isCanonicalUtf8WithoutNul(byte[] bytes) {
            int index = 0;
            while (index < bytes.length) {
                int lead = bytes[index] & 0xff;
                if (lead < 0x80) {
                    if (lead == 0) return false;
                    index++;
                    continue;
                }
                int length;
                int minimum;
                if (lead >= 0xc2 && lead <= 0xdf) {
                    length = 2;
                    minimum = 0x80;
                } else if (lead >= 0xe0 && lead <= 0xef) {
                    length = 3;
                    minimum = 0x800;
                } else if (lead >= 0xf0 && lead <= 0xf4) {
                    length = 4;
                    minimum = 0x10000;
                } else {
                    return false;
                }
                if (index + length > bytes.length) return false;
                int codePoint = lead & (0xff >> (length + 1));
                for (int next = 1; next < length; next++) {
                    int continuation = bytes[index + next] & 0xff;
                    if ((continuation & 0xc0) != 0x80) return false;
                    codePoint = (codePoint << 6) | (continuation & 0x3f);
                }
                if (codePoint < minimum
                        || codePoint > 0x10ffff
                        || (codePoint >= 0xd800 && codePoint <= 0xdfff)) {
                    return false;
                }
                index += length;
            }
            return true;
        }
    }
}
