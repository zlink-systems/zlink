from pathlib import Path
R=Path('/root/worktree/zlink-jenv/framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime')
def edit(name,old,new,count=1):
 p=R/name;s=p.read_text();assert s.count(old)==count,(name,s.count(old),old[:90]);s=s.replace(old,new);p.write_text(s)
# Descriptor implementation belongs to the existing envelope owner. No format fallback.
p=R/'messaging/ZLinkChannelEnvelope.java';s=p.read_text();at=s.index('    public static String newCorrelationId()')
code='''    /** Measurement-only major 2: native [descriptor, typed payload], flow off, empty metadata. */
    public static List<Message> encodeNative(Header header, ZLinkFrameworkMultipartView body, int operation) {
        if (!header.metadata().isEmpty() || header.flowId() != null || header.topic() != null
                || header.source() != null) {
            throw new IllegalArgumentException("prototype requires empty metadata and flow off");
        }
        byte[] channel = header.channelName().getBytes(StandardCharsets.UTF_8);
        byte[] name = header.messageName().getBytes(StandardCharsets.UTF_8);
        int codec = switch (header.contentType()) {
            case "application/json" -> 0;
            case "application/x-protobuf" -> 1;
            case "application/zlink-framework-json-v1" -> 2;
            default -> throw new IllegalArgumentException("unsupported prototype codec");
        };
        if (channel.length > 255 || name.length > 255 || header.correlationId() == null
                || header.correlationId().length() != 32) {
            throw new IllegalArgumentException("invalid prototype names or 128-bit identity");
        }
        long high = Long.parseUnsignedLong(header.correlationId().substring(0, 16), 16);
        long low = Long.parseUnsignedLong(header.correlationId().substring(16), 16);
        int flags = header.deadline() == null ? 0 : 1;
        byte[] errorCode = header.errorCode() == null ? new byte[0] : header.errorCode().getBytes(StandardCharsets.UTF_8);
        byte[] errorMessage = header.errorMessage() == null ? new byte[0] : header.errorMessage().getBytes(StandardCharsets.UTF_8);
        if (header.isError()) flags |= 2;
        ByteBuffer b = ByteBuffer.allocate(25 + channel.length + name.length
                + ((flags & 1) == 0 ? 0 : 8) + ((flags & 2) == 0 ? 0 : 8 + errorCode.length + errorMessage.length));
        b.put((byte)'Z').put((byte)'M').put((byte)2).put((byte)operation).put((byte)flags);
        b.put((byte)header.kind()).put((byte)codec).putLong(high).putLong(low);
        b.put((byte)channel.length).put(channel).put((byte)name.length).put(name);
        if ((flags & 1) != 0) b.putLong(java.time.Instant.parse(header.deadline()).toEpochMilli());
        if ((flags & 2) != 0) b.putInt(errorCode.length).put(errorCode).putInt(errorMessage.length).put(errorMessage);
        return ZLinkFrameworkMultipartView.encoded(b.array(), body);
    }

    public static int nativeOperation(ByteBuffer b) {
        int start = b.position();
        if (b.remaining() < 25 || b.get(start) != 'Z' || b.get(start + 1) != 'M'
                || b.get(start + 2) != 2) throw new IllegalArgumentException("invalid prototype descriptor");
        int operation = Byte.toUnsignedInt(b.get(start + 3));
        if (operation != 18 && operation != 19 && operation != 20)
            throw new IllegalArgumentException("invalid prototype operation");
        return operation;
    }

    public static String nativeChannel(ByteBuffer b) {
        nativeOperation(b);
        b.position(b.position() + 23);
        return nativeText(b);
    }

    private static String nativeText(ByteBuffer b) {
        byte[] text = new byte[Byte.toUnsignedInt(b.get())];
        b.get(text);
        return new String(text, StandardCharsets.UTF_8);
    }

    public static Header decodeNative(List<Message> parts) {
        if (parts.size() != 2) throw new IllegalArgumentException("prototype requires two native parts");
        ByteBuffer b = ZLinkFrameworkMultipartView.buffer(parts, 0);
        int operation = nativeOperation(b);
        b.position(b.position() + 4);
        int flags = Byte.toUnsignedInt(b.get());
        int kind = Byte.toUnsignedInt(b.get());
        String codec = switch (Byte.toUnsignedInt(b.get())) {
            case 0 -> "application/json";
            case 1 -> "application/x-protobuf";
            case 2 -> "application/zlink-framework-json-v1";
            default -> throw new IllegalArgumentException("invalid prototype codec");
        };
        if ((flags & ~3) != 0 || (operation == 20 ? kind != KIND_RESPONSE && kind != KIND_ERROR : kind != KIND_REQUEST))
            throw new IllegalArgumentException("invalid prototype kind or flags");
        String identity = ZLinkServiceOperationIds.correlationId(new UUID(b.getLong(), b.getLong()));
        String channel = nativeText(b);
        String name = nativeText(b);
        String deadline = (flags & 1) == 0 ? null : java.time.Instant.ofEpochMilli(b.getLong()).toString();
        String errorCode = null, errorMessage = null;
        if ((flags & 2) != 0) {
            byte[] text = new byte[b.getInt()]; b.get(text); errorCode = new String(text, StandardCharsets.UTF_8);
            text = new byte[b.getInt()]; b.get(text); errorMessage = new String(text, StandardCharsets.UTF_8);
        }
        if (b.hasRemaining()) throw new IllegalArgumentException("trailing prototype descriptor bytes");
        return new Header(kind, channel, name, codec, identity, deadline, null, errorCode, errorMessage, null, Map.of(), null, null);
    }

'''
s=s[:at]+code+s[at:];p.write_text(s)
# Explicit Node and Channel producers; ClientServer and Spot retain existing encoding.
p=R/'channels/ZLinkChannelRouteCalls.java';s=p.read_text()
a=s.index('    private List<Message> requestParts(java.util.UUID operationId, Map<String, String> metadata)')
b=s.index('    @Override',a)
t=s[a:b].replace('runtime.registeredEnvelopeParts(', 'runtime.nativeEnvelopeParts(').replace('ZLinkChannelEnvelope.KIND_REQUEST,','ZLinkChannelEnvelope.KIND_REQUEST,')
# The call signature has command in the first argument instead of kind.
t=t.replace('systems.zlink.framework.runtime.messaging.ZLinkChannelEnvelope.KIND_REQUEST,','18,')
s=s[:a]+t+s[b:]
a=s.index('    private <TReply> CompletionStage<TReply> submitMesh(');b=s.index('    private <TReply> ZLinkServiceOperationRegistry.Operation<TReply> requestResult(',a)
t=s[a:b].replace('runtime.registeredEnvelopeParts(', 'runtime.nativeEnvelopeParts(').replace('systems.zlink.framework.runtime.messaging.ZLinkChannelEnvelope.KIND_REQUEST,','19,')
s=s[:a]+t+s[b:];p.write_text(s)
p=R/'channels/ZLinkChannelCallRuntime.java';s=p.read_text();at=s.index('    static List<Message> envelopeParts(')
s=s[:at]+'''    List<Message> nativeEnvelopeParts(
            int operation, String channelName, Optional<String> packetName,
            ZLinkFrameworkMultipartView payload, String contentType, Map<String, String> metadata,
            UUID operationId, ZLinkChannelEnvelope.PreparedNames channelNames,
            ZLinkChannelEnvelope.PreparedNames names) {
        return ZLinkChannelEnvelope.encodeNative(
                ZLinkChannelEnvelope.create(ZLinkChannelEnvelope.KIND_REQUEST, channelName,
                        packetName.orElseThrow(), contentType, null, metadata,
                        ZLinkFlowContext.current(), operationId), payload, operation);
    }

'''+s[at:]
s=s.replace('ZLinkChannelEnvelope.tryDecodeHeader(reply.parts(), false);','ZLinkChannelEnvelope.decodeNative(reply.parts());')
p.write_text(s)
# The prepared Operation still owns the typed receiver/metrics, without hop registry enrollment.
p=R/'internal/service/ZLinkServiceOperationRegistry.java';s=p.read_text();at=s.index('        private <V> void receiveValue(')
s=s[:at]+'''        /** Completes a local typed operation directly from its binding request completion. */
        public <V> void receiveBinding(V value, Throwable failure, Consumer<? super V> discardValue) {
            try {
                if (failure == null) receiveValue(value, discardValue);
                else receiveFailure(failure);
            } finally {
                clearCancellation();
            }
        }

'''+s[at:];p.write_text(s)
# Mesh dispatcher is explicitly on the prototype request path; send dispatch stays unchanged.
p=R/'channels/ZLinkMeshApplicationDispatcher.java';s=p.read_text()
s=s.replace('''systems.zlink.framework.runtime.messaging.ZLinkChannelEnvelope
                            .decodeDispatchHeader(record.parts(), captureFlow);''','''(kind == RecordKind.NODE_REQUEST || kind == RecordKind.CHANNEL_REQUEST)
                            ? ZLinkChannelEnvelope.decodeNative(record.parts())
                            : ZLinkChannelEnvelope.decodeDispatchHeader(record.parts(), captureFlow);''')
old='''ZLinkChannelDispatchReporter.replyParts(
                                                                envelope,
                                                                reply,
                                                                namespace.channelNames,
                                                                route != null
                                                                        ? route.envelopeNames()
                                                                        : channel.envelopeNames(),
                                                                framework.codecs())'''
assert old in s
s=s.replace(old,'''ZLinkChannelEnvelope.encodeNative(
                                                                ZLinkChannelEnvelope.reply(envelope), reply, 20)''')
p.write_text(s)
# Raw mesh transport integration: generic ordinary requests use native multipart and binding completion.
p=R/'binding/ZLinkJavaRawMeshNode.java';s=p.read_text()
a=s.index('        RouterSocket router = requireStarted();',s.index('    <R> CompletionStage<R> requestNode('));b=s.index('\n    CompletionStage<ZLinkBackendReceived> requestChannel(',a)
s=s[:a]+'''        return requestNative(requireStarted(), target, parts, timeout, null, requestAdmission, receiver);
    }
''' +s[b:]
a=s.index('        long correlation = allocateCorrelation();',s.index('    <R> CompletionStage<R> requestChannel('));b=s.index('        try {',a)
s=s[:a]+'''        List<Message> frames = parts;
'''+s[b:]
a=s.index('            return request(',s.index('    <R> CompletionStage<R> requestChannel('));b=s.index('        } catch',a)
s=s[:a]+'''            return requestNative(
                    router, target.orElseThrow().descriptor().nodeRoutingId(), frames, timeout,
                    target.orElseThrow().liveness(), requestAdmission, receiver);
'''+s[b:]
at=s.index('    Optional<Integer> classifyChannelTarget(')
s=s[:at]+'''    private <R> CompletionStage<R> requestNative(
            RouterSocket router, RoutingId target, List<Message> frames, Duration timeout,
            ZLinkServiceLivenessRegistry.PeerState admission, CompletableFuture<Void> requestAdmission,
            ZLinkServiceOperationRegistry.Operation<R> receiver) {
        port.requestMessagesTo(router, target, frames, timeout,
                reply -> {
                    if (reply.size() != 2 || ZLinkChannelEnvelope.nativeOperation(reply.getFirst().dataBuffer()) != 20)
                        throw new IllegalArgumentException("invalid prototype reply");
                    // The receiver decodes synchronously before the binding closes its reply parts.
                    return new ZLinkBackendReceived(ZLinkBackendRequestResult.OK, Optional.of(target),
                            Optional.empty(), Optional.empty(), reply);
                }, null,
                (reply, failure) -> receiver.receiveBinding(reply, failure, ignored -> {}),
                admission, requestAdmission);
        return receiver;
    }

'''+s[at:]
# This is a distinct, explicit major; service-control traffic keeps major 1.
s=s.replace('''header = wire.decodeHeader(head);''','''header = head[2] == 2
                                ? new ZLinkServiceM6AWireCodec.Header(
                                        ZLinkChannelEnvelope.nativeOperation(ByteBuffer.wrap(head)), 0)
                                : wire.decodeHeader(head);''',1)
s=s.replace('''correlation = wire.decodeNodeRequestHeader(head);''','''if (head[2] != 2) throw new IllegalArgumentException("prototype requires major 2 Node request");''',1)
s=s.replace('''ZLinkServiceM6AWireCodec.ChannelRequest request =
                        wire.decodeChannelRequestHeader(head);
                correlation = request.correlation();
                channelName = request.channelName();''','''if (head[2] != 2) throw new IllegalArgumentException("prototype requires major 2 Channel request");
                channelName = ZLinkChannelEnvelope.nativeChannel(ByteBuffer.wrap(head));''',1)
s=s.replace('''            int payloadOffset = (flags & ServiceWireConstants.FLAG_METADATA) == 0 ? 1 : 2;
            if (frames.size() != payloadOffset + 1''','''            boolean nativeRequest = kind == RecordKind.NODE_REQUEST || kind == RecordKind.CHANNEL_REQUEST;
            int payloadOffset = nativeRequest ? 0 : (flags & ServiceWireConstants.FLAG_METADATA) == 0 ? 1 : 2;
            if (frames.size() != (nativeRequest ? 2 : payloadOffset + 1)''',1)
s=s.replace('''ZLinkServiceM6AWireCodec.decodeFrameworkMultipartFrameView(
                                inbound.received().parts().get(payloadOffset).dataBuffer());''','''nativeRequest
                                ? ZLinkFrameworkMultipartView.received(inbound.received())
                                : ZLinkServiceM6AWireCodec.decodeFrameworkMultipartFrameView(
                                        inbound.received().parts().get(payloadOffset).dataBuffer());''',1)
s=s.replace('''                    correlation == null
                            ? null
                            : replyParts -> {''','''                    inbound.requestSequence() == null
                            ? null
                            : replyParts -> {''',1)
s=s.replace('''                                            encodeApplicationFrames(
                                                    wire.encodeReplyHeader(
                                                            requestCorrelation, 0, 0),
                                                    null,
                                                    replyParts));''','''                                            nativeRequest ? replyParts : encodeApplicationFrames(
                                                    wire.encodeReplyHeader(requestCorrelation, 0, 0),
                                                    null, replyParts));''',1)
# Do not close moved-to-native reply parts twice while async submission owns them.
# replyMessages materializes/copies before return; existing close convention remains.
p.write_text(s)
