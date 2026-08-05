/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.runtime.sockets;
import systems.zlink.internal.sockets.SocketOptionKey;
import systems.zlink.internal.sockets.SocketOptions;
import systems.zlink.internal.sockets.SocketOptionValueType;

import systems.zlink.internal.ContractAccess;

import systems.zlink.contracts.core.Context;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.contracts.eventing.MonitorEventType;
import systems.zlink.contracts.eventing.SocketMonitor;
import systems.zlink.contracts.messaging.Received;
import systems.zlink.contracts.errors.ZlinkRecvException;
import systems.zlink.runtime.nativeapi.RecvScratch;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.errors.ZlinkSubmitException;
import systems.zlink.contracts.sockets.*;
import systems.zlink.contracts.messaging.SubscriptionEntry;
import systems.zlink.contracts.messaging.SubscriptionEvent;
import systems.zlink.contracts.messaging.TopicMessage;
import systems.zlink.contracts.errors.ZlinkException;
import systems.zlink.runtime.nativeapi.InternalAccess;
import systems.zlink.runtime.nativeapi.Native;
import systems.zlink.runtime.nativeapi.NativeErrno;
import systems.zlink.runtime.nativeapi.NativeHelpers;
import systems.zlink.runtime.nativeapi.NativeLayouts;
import systems.zlink.runtime.nativeapi.RequestProgressPump;
import io.netty.buffer.ByteBuf;
import java.lang.foreign.Arena;
import java.lang.foreign.MemorySegment;
import java.lang.foreign.ValueLayout;
import java.nio.ByteBuffer;
import java.util.List;
import java.util.Objects;
import java.util.Optional;

/**
 * Abstract common socket base for zlink typed socket facades.
 */
final class NativeSocketRuntime implements AutoCloseable {
    static final int DEFAULT_IO_BUFFER_SIZE = 8192;
    static final int TOPIC_CAPACITY = 256;
    private final SocketCore socketCore;
    private final TopicPlane topicPlane;
    private final ReceivePlane receivePlane;
    private final NettySocketPlane nettyPlane;
    private final SocketSendPlane sendPlane;
    private final SocketOptionSupport optionSupport;
    private MemorySegment handle;
    private final boolean own;
    private final SocketType socketTypeHint;
    private final ThreadLocal<RecvScratch> recvScratch =
      ThreadLocal.withInitial(RecvScratch::new);

    public void disconnectRid(RoutingId peerRid) {
        socketCore.disconnectRid(peerRid);
    }

    public static boolean inCallbackContext() {
        return SocketCore.inCallback();
    }

    public static void enterCallbackContext() {
        SocketCore.enterCallback();
    }

    public static void leaveCallbackContext() {
        SocketCore.leaveCallback();
    }

    NativeSocketRuntime(Context ctx, SocketType type) {
        this.handle = Native.socket(InternalAccess.contextHandle(ctx), type.getValue());
        if (handle == null || handle.address() == 0)
            throw ZlinkException.fromLastError(systems.zlink.contracts.errors.ErrorCategory.CONFIG);
        this.own = true;
        this.socketTypeHint = type;
        this.socketCore = new SocketCore(this);
        this.topicPlane = new TopicPlane(this);
        this.receivePlane = new ReceivePlane(this);
        this.nettyPlane = new NettySocketPlane(this);
        this.sendPlane = new SocketSendPlane(this);
        this.optionSupport = new SocketOptionSupport(this);
    }

    NativeSocketRuntime(MemorySegment handle, boolean own, SocketType socketTypeHint) {
        this.handle = handle;
        this.own = own;
        this.socketTypeHint = socketTypeHint;
        this.socketCore = new SocketCore(this);
        this.topicPlane = new TopicPlane(this);
        this.receivePlane = new ReceivePlane(this);
        this.nettyPlane = new NettySocketPlane(this);
        this.sendPlane = new SocketSendPlane(this);
        this.optionSupport = new SocketOptionSupport(this);
    }

    /** Binds the socket to the endpoint. */
    public void bind(String endpoint) {
        socketCore.bind(endpoint);
    }

    /** Connects the socket to the endpoint. */
    public void connect(String endpoint) {
        socketCore.connect(endpoint);
    }

    /** Unbinds the socket from the endpoint. */
    public void unbind(String endpoint) {
        socketCore.unbind(endpoint);
    }

    /** Disconnects the socket from the endpoint. */
    public void disconnect(String endpoint) {
        socketCore.disconnect(endpoint);
    }

    public void attachStreamPacket(StreamFramedPacketHandler handler) {
        socketCore.attachStreamPacket(handler);
    }

    public void attachStreamPacket(StreamUInt32FramedPacketHandler handler) {
        socketCore.attachStreamPacket(handler);
    }

    public void attachStreamPacket(StreamUInt32FramedNativeHandler handler) {
        socketCore.attachStreamPacket(handler);
    }

    public void setSockOpt(int optionId, String optionName, byte[] value) {
        socketCore.setSockOpt(optionId, optionName, value);
    }

    public void setSockOpt(int optionId, String optionName, byte[] value,
                           int offset, int length) {
        socketCore.setSockOpt(optionId, optionName, value, offset, length);
    }

    public void setSockOpt(int optionId, String optionName, ByteBuffer value) {
        socketCore.setSockOpt(optionId, optionName, value);
    }

    public void setSockOpt(int optionId, String optionName, int value) {
        socketCore.setSockOpt(optionId, optionName, value);
    }

    public byte[] getSockOptBytes(int optionId, String optionName, int maxLen) {
        return socketCore.getSockOptBytes(optionId, optionName, maxLen);
    }

    public int getSockOptInt(int optionId, String optionName) {
        return socketCore.getSockOptInt(optionId, optionName);
    }

    public void setOption(SocketOptionKey<Integer> option, int value) {
        socketCore.setOption(option, value);
    }

    public void setOption(SocketOptionKey<Long> option, long value) {
        socketCore.setOptionLong(option, value);
    }

    public void setOption(SocketOptionKey<String> option, String value) {
        socketCore.setOptionString(option, value);
    }

    public void setOption(SocketOptionKey<byte[]> option, byte[] value) {
        socketCore.setOptionBytes(option, value);
    }

    void setDealerIntOption(int option, int value) {
        optionSupport.setDealerIntOption(option, value);
    }

    int getDealerIntOption(int option) {
        return optionSupport.getDealerIntOption(option);
    }

    int getRouterIntOption(int option) {
        return optionSupport.getRouterIntOption(option);
    }

    void setRouterIntOption(int option, int value) {
        optionSupport.setRouterIntOption(option, value);
    }

    @SuppressWarnings("unchecked")
    public <T> T getOption(SocketOptionKey<T> option) {
        return socketCore.getOption(option);
    }

    /** Opens a socket monitor for all events. */
    public SocketMonitor monitorOpen() {
        return monitorOpen(MonitorEventType.ALL);
    }

    /** Opens a socket monitor for the requested event types. */
    public SocketMonitor monitorOpen(MonitorEventType... events) {
        return socketCore.monitorOpen(resolveMonitorEvents(events));
    }

    public final void setTlsServer(String certPem, String keyPem,
                                   boolean requireClientCert) {
        socketCore.setTlsServer(certPem, keyPem, requireClientCert);
    }

    public final void setTlsClient(String caCertPem, String hostname,
                                   boolean trustSystem) {
        socketCore.setTlsClient(caCertPem, hostname, trustSystem);
    }

    private static int resolveMonitorEvents(MonitorEventType... events) {
        if (events == null || events.length == 0) {
            return MonitorEventType.ALL.getValue();
        }
        int mask = 0;
        for (MonitorEventType event : events) {
            Objects.requireNonNull(event, "events");
            mask |= event.getValue();
        }
        return mask;
    }

    public boolean send(Message part) {
        return send(part, SendFlag.NONE);
    }

    public boolean send(Message part, SendFlag flags) {
        Objects.requireNonNull(flags, "flags");
        if ((flags.getValue() & SendFlag.DONTWAIT.getValue()) != 0) {
            return trySendResult(sendNoWaitResult(part));
        }
        Objects.requireNonNull(part, "part");
        sendMessageFrame(part, flags);
        return true;
    }

    public void sendMessageFrame(RoutingId routingId, Message message, SendFlag flag) {
        sendPlane.sendMessageFrame(routingId, message, flag);
    }

    public boolean send(List<Message> parts) {
        return send(parts, SendFlag.NONE);
    }

    boolean send(List<Message> parts, SendFlag flags) {
        Objects.requireNonNull(flags, "flags");
        if ((flags.getValue() & SendFlag.DONTWAIT.getValue()) != 0) {
            return trySendResult(sendNoWaitResult(parts));
        }
        Objects.requireNonNull(parts, "parts");
        sendParts(null, parts, flags, false);
        return true;
    }

    SendResult sendNoWaitResult(Message part) {
        Objects.requireNonNull(part, "part");
        return sendMessageFrameNoWaitResult(part);
    }

    SendResult sendMessageFrameNoWaitResult(RoutingId routingId, Message message) {
        return sendPlane.sendMessageFrameNoWaitResult(routingId, message);
    }

    SendResult sendNoWaitResult(List<Message> parts) {
        Objects.requireNonNull(parts, "parts");
        return sendNoWaitPartsResult(null, parts);
    }

    boolean send(RoutingId rid, Message part) {
        return send(rid, part, SendFlag.NONE);
    }

    boolean send(RoutingId rid, Message part, SendFlag flags) {
        Objects.requireNonNull(flags, "flags");
        if ((flags.getValue() & SendFlag.DONTWAIT.getValue()) != 0) {
            return trySendResult(sendNoWaitResult(rid, part));
        }
        Objects.requireNonNull(part, "part");
        sendMessageFrame(rid, part, flags);
        return true;
    }

    boolean send(byte[] routingIdBytes, Message part, SendFlag flags) {
        return sendPlane.send(routingIdBytes, part, flags);
    }

    void send(int rid, Message part, SendFlag flags) {
        sendPlane.send(rid, part, flags);
    }

    int send(int rid, MemorySegment payload, int length, int sendFlags) {
        return sendPlane.send(rid, payload, length, sendFlags);
    }

    int sendCopied(int rid, MemorySegment payload, int length, int sendFlags) {
        return sendPlane.sendCopied(rid, payload, length, sendFlags);
    }

    public boolean send(RoutingId rid, List<Message> parts) {
        return send(rid, parts, SendFlag.NONE);
    }

    public boolean send(RoutingId rid, List<Message> parts, SendFlag flags) {
        Objects.requireNonNull(flags, "flags");
        if ((flags.getValue() & SendFlag.DONTWAIT.getValue()) != 0) {
            return trySendResult(sendNoWaitResult(rid, parts));
        }
        Objects.requireNonNull(parts, "parts");
        sendParts(rid, parts, flags, false);
        return true;
    }

    SendResult sendNoWaitResult(RoutingId rid, Message part) {
        Objects.requireNonNull(part, "part");
        return sendMessageFrameNoWaitResult(rid, part);
    }

    SendResult sendNoWaitResult(RoutingId rid, List<Message> parts) {
        Objects.requireNonNull(parts, "parts");
        return sendNoWaitPartsResult(rid, parts);
    }

    /** Publishes a single payload part to a topic-aware socket. */
    public boolean publish(String topicId, Message part) {
        topicPlane.publish(topicId, part);
        return true;
    }

    /** Publishes a single payload part with explicit send flags. */
    public boolean publish(String topicId, Message part, SendFlag flags) {
        Objects.requireNonNull(flags, "flags");
        if ((flags.getValue() & SendFlag.DONTWAIT.getValue()) != 0) {
            return trySendResult(publishNoWaitResult(topicId, part));
        }
        topicPlane.publish(topicId, part, flags);
        return true;
    }

    public void publishMessageFrame(String topicId, Message message, SendFlag flags) {
        sendPlane.publishMessageFrame(topicId, message, flags);
    }

    /** Publishes a multipart payload to a topic-aware socket. */
    public boolean publish(String topicId, List<Message> parts) {
        topicPlane.publish(topicId, parts);
        return true;
    }

    /** Publishes a multipart payload with explicit send flags. */
    public boolean publish(String topicId, List<Message> parts, SendFlag flags) {
        Objects.requireNonNull(flags, "flags");
        if ((flags.getValue() & SendFlag.DONTWAIT.getValue()) != 0) {
            return trySendResult(publishNoWaitResult(topicId, parts));
        }
        topicPlane.publish(topicId, parts, flags);
        return true;
    }

    public SendResult publishNoWaitResult(String topicId, Message part) {
        return topicPlane.publishNoWaitResult(topicId, part);
    }

    public SendResult publishMessageFrameNoWaitResult(String topicId, Message message) {
        return sendPlane.publishMessageFrameNoWaitResult(topicId, message);
    }

    public SendResult publishNoWaitResult(String topicId, List<Message> parts) {
        return topicPlane.publishNoWaitResult(topicId, parts);
    }

    public Received recv() {
        return recvLazy(ReceiveFlag.NONE);
    }

    public Received recv(ReceiveFlag flags) {
        Objects.requireNonNull(flags, "flags");
        if (flags == ReceiveFlag.DONTWAIT) {
            return recvLazyNoWaitOrNull();
        }
        return recvLazy(flags);
    }

    public Received recvNoWaitOrNull() {
        return recvLazyNoWaitOrNull();
    }

    public boolean recvInto(Received result, ReceiveFlag flags) {
        return receivePlane.recvInto(result, flags);
    }

    public Optional<Received> recvNoWait() {
        return Optional.ofNullable(recvNoWaitOrNull());
    }

    public Received recvLazy(ReceiveFlag flags) {
        return receivePlane.recvLazy(flags);
    }

    public Received recvLazyNoWaitOrNull() {
        return receivePlane.recvLazyNoWaitOrNull();
    }

    /** Receives a topic-aware delivery from a SUB/XSUB-style socket. */
    public TopicMessage subscribe() {
        return topicPlane.subscribe();
    }

    /** Receives a topic-aware delivery with explicit receive flags. */
    public TopicMessage subscribe(ReceiveFlag flags) {
        Objects.requireNonNull(flags, "flags");
        if (flags == ReceiveFlag.DONTWAIT) {
            return topicPlane.subscribeNoWait().orElse(null);
        }
        return topicPlane.subscribe(flags);
    }

    public boolean subscribe(TopicMessage result, ReceiveFlag flags) {
        return topicPlane.subscribe(result, flags);
    }

    public Optional<TopicMessage> subscribeNoWait() {
        return topicPlane.subscribeNoWait();
    }

    public SubscriptionEvent receiveSubscriptionEvent() {
        return topicPlane.subscriptionEvent(ReceiveFlag.NONE);
    }

    public SubscriptionEvent receiveSubscriptionEvent(ReceiveFlag flags) {
        return topicPlane.subscriptionEvent(flags);
    }

    public boolean receiveSubscriptionEvent(SubscriptionEvent result,
                                     ReceiveFlag flags) {
        Objects.requireNonNull(result, "result");
        SubscriptionEvent fresh;
        try {
            fresh = receiveSubscriptionEvent(flags);
        } catch (ZlinkRecvException ex) {
            if (flags == ReceiveFlag.DONTWAIT
                && ex.getResult() == RecvResult.NO_DATA) {
                return false;
            }
            throw ex;
        }
        if (fresh == null)
            return false;
        ContractAccess.subscriptionEventAdoptFrom(result, fresh);
        return true;
    }

    public SubscriptionEvent subscriptionEvent(ReceiveFlag flags) {
        return topicPlane.subscriptionEvent(flags);
    }

    public Optional<SubscriptionEvent> tryReceiveSubscriptionEvent() {
        return topicPlane.trySubscriptionEvent();
    }

    public void setRoutingId(RoutingId rid) {
        topicPlane.setRoutingId(rid);
    }

    public RoutingId getRoutingId() {
        return topicPlane.getRoutingId();
    }

    public void setSubscription(String filter) {
        topicPlane.setSubscription(filter);
    }

    public void setSubscription(byte[] filter) {
        topicPlane.setSubscription(filter);
    }

    public void unsetSubscription(String filter) {
        topicPlane.unsetSubscription(filter);
    }

    public void unsetSubscription(byte[] filter) {
        topicPlane.unsetSubscription(filter);
    }

    public List<SubscriptionEntry> subscriptions() {
        return topicPlane.subscriptions();
    }

    public void onReceive(SocketMessageHandler handler) {
        socketCore.onReceive(handler);
    }

    public void setSendReadyHandler(SendReadyHandler handler) {
        socketCore.setSendReadyHandler(handler);
    }

    public void setCompletionControlHandler(CompletionControlHandler handler) {
        socketCore.setCompletionControlHandler(handler);
    }

    int send(byte[] data, int offset, int length, int sendFlags) {
        Objects.requireNonNull(data, "data");
        validateRange(data.length, offset, length, "data");
        try (Message msg = Message.from(data, offset, length)) {
            sendMessageFrame(msg, SendFlag.fromValue(sendFlags));
            return length;
        }
    }

    boolean sendNoWaitResult(byte[] data, int offset, int length, int sendFlags) {
        Objects.requireNonNull(data, "data");
        validateRange(data.length, offset, length, "data");
        try (Message msg = Message.from(data, offset, length)) {
            return sendMessageFrameNoWaitResult(msg, SendFlag.fromValue(sendFlags));
        }
    }

    int send(MemorySegment segment, long offset, long length,
             int sendFlags) {
        Objects.requireNonNull(segment, "segment");
        validateRange(segment.byteSize(), offset, length, "segment");
        try (Message msg = InternalAccess.messageFromSegment(segment, offset,
                 length)) {
            sendMessageFrame(msg, SendFlag.fromValue(sendFlags));
            return (int) length;
        }
    }

    boolean sendNoWaitResult(MemorySegment segment, long offset, long length,
                    int sendFlags) {
        Objects.requireNonNull(segment, "segment");
        validateRange(segment.byteSize(), offset, length, "segment");
        try (Message msg = InternalAccess.messageFromSegment(segment, offset,
                 length)) {
            return sendMessageFrameNoWaitResult(msg, SendFlag.fromValue(sendFlags));
        }
    }

    int send(ByteBuf buf, int sendFlags) {
        return nettyPlane.send(buf, sendFlags);
    }

    boolean sendNoWaitResult(ByteBuf buf, int sendFlags) {
        return nettyPlane.sendNoWaitResult(buf, sendFlags);
    }

    int recv(MemorySegment segment, long offset, long length,
             ReceiveFlag flags) {
        return receivePlane.recv(segment, offset, length, flags);
    }

    int recvNoWait(MemorySegment segment, long offset, long length,
                ReceiveFlag flags) {
        return receivePlane.recvNoWait(segment, offset, length, flags);
    }

    MemorySegment handle() {
        return handle;
    }

    public void close() {
        socketCore.close();
    }

    void closeInternal() {
        if (handle != null && handle.address() != 0) {
            RequestProgressPump.stopSocketProgress(handle);
            if (own) {
                int rc = Native.close(handle);
                if (rc != 0)
                    throw ZlinkException.fromLastError(systems.zlink.contracts.errors.ErrorCategory.CLOSE);
            }
            socketCore.closeCommonState();
            handle = MemorySegment.NULL;
            return;
        }
        socketCore.closeCommonState();
    }

    @SuppressWarnings("unchecked")
    public <T> T readOption(SocketOptionKey<T> option) {
        return optionSupport.readOption(option);
    }

    public int recvByteBufDirect(ByteBuf buf, ReceiveFlag flags) {
        return nettyPlane.recvByteBufDirect(buf, flags);
    }

    public int recvByteBufDirectNoWait(ByteBuf buf, ReceiveFlag flags) {
        return nettyPlane.recvByteBufDirectNoWait(buf, flags);
    }

    static void validateRange(int total, int offset, int length, String name) {
        if (offset < 0 || length < 0 || offset > total - length)
            throw new IndexOutOfBoundsException(name + " range out of bounds");
    }

    static void validateRange(long total, long offset, long length, String name) {
        if (offset < 0 || length < 0 || offset > total - length)
            throw new IndexOutOfBoundsException(name + " range out of bounds");
    }

    static void validateOptionType(SocketOptionKey<?> option,
                                           SocketOptionValueType expected) {
        if (option.valueType() != expected) {
            throw new IllegalArgumentException(
              option.name() + " expects " + option.valueType()
                + ", not " + expected);
        }
    }

    void validateAmbiguousOption(SocketOptionKey<?> option) {
        if (option.optionId() != SocketOptions.TLS_VERIFY.optionId())
            return;
        SocketType type = resolveSocketType();
        if (type == null)
            return;
        if (type == SocketType.XPUB) {
            if (option != SocketOptions.XPUB_MANUAL_LAST_VALUE) {
                throw new IllegalArgumentException(
                  "XPUB socket option id 98 must use "
                    + SocketOptions.XPUB_MANUAL_LAST_VALUE.name());
            }
            return;
        }
        if (option != SocketOptions.TLS_VERIFY) {
            throw new IllegalArgumentException(
              "Non-XPUB socket option id 98 must use "
                + SocketOptions.TLS_VERIFY.name());
        }
    }

    void validateOptionAccess(int optionId, String optionName) {
        SocketType type = resolveSocketType();
        if (type == null)
            return;
        if (optionId == SocketOptions.TLS_VERIFY.optionId()) {
            if (type == SocketType.XPUB) {
                if (SocketOptions.XPUB_MANUAL_LAST_VALUE.name().equals(optionName))
                    return;
                throw new IllegalArgumentException(
                  optionName + " is not supported by " + type + " sockets");
            }
            if (SocketOptions.TLS_VERIFY.name().equals(optionName))
                return;
            throw new IllegalArgumentException(
              optionName + " is not supported by " + type + " sockets");
        }
        if (supportsOption(type, optionId))
            return;
        throw new IllegalArgumentException(
          optionName + " is not supported by " + type + " sockets");
    }

    private static boolean supportsOption(SocketType type, int optionId) {
        return switch (optionId) {
            case 5 -> type == SocketType.DEALER || type == SocketType.ROUTER;
            case 6, 7 -> type == SocketType.SUB || type == SocketType.XSUB;
            case 33, 51, 56, 61 -> type == SocketType.DEALER
                || type == SocketType.ROUTER;
            case 40, 69, 71, 72, 78, 0x3308, 0x3309 -> type == SocketType.PUB
                || type == SocketType.XPUB;
            case 73 -> type == SocketType.STREAM;
            case 116 -> type == SocketType.PUB || type == SocketType.XPUB
                || type == SocketType.SUB || type == SocketType.XSUB;
            default -> true;
        };
    }

    SocketType resolveSocketType() {
        if (socketTypeHint != null)
            return socketTypeHint;
        try {
            return SocketType.fromValue(getSockOptInt(SocketOptions.TYPE.optionId()));
        } catch (RuntimeException ignored) {
            return null;
        }
    }

    void setSockOptRaw(int optionId, MemorySegment value, long len) {
        optionSupport.setSockOptRaw(optionId, value, len);
    }

    public void setSockOptBytes(int optionId, byte[] value, int offset,
                         int length) {
        optionSupport.setSockOptBytes(optionId, value, offset, length);
    }

    public void setTypedBytesOption(int optionId, byte[] value, int offset,
                             int length) {
        optionSupport.setTypedBytesOption(optionId, value, offset, length);
    }

    public void setRoutingIdBytes(byte[] value, int offset, int length) {
        optionSupport.setRoutingIdBytes(value, offset, length);
    }

    public byte[] getRoutingIdBytes() {
        return optionSupport.getRoutingIdBytes();
    }

    public void setSubscriptionBytes(byte[] value, int offset, int length,
                              boolean subscribe) {
        optionSupport.setSubscriptionBytes(value, offset, length, subscribe);
    }

    public void setSockOptInt(int optionId, int value) {
        optionSupport.setSockOptInt(optionId, value);
    }

    public void setSockOptLong(int optionId, long value) {
        optionSupport.setSockOptLong(optionId, value);
    }

    public byte[] getSockOptBytes(int optionId, int maxLen) {
        return optionSupport.getSockOptBytes(optionId, maxLen);
    }

    public int getSockOptInt(int optionId) {
        return optionSupport.getSockOptInt(optionId);
    }

    public void sendMessageFrame(Message message, SendFlag flag) {
        sendPlane.sendMessageFrame(message, flag);
    }

    public boolean sendMessageFrameNoWaitResult(Message message, SendFlag flag) {
        return sendPlane.sendMessageFrameNoWaitResult(message, flag);
    }

    public SendResult sendMessageFrameNoWaitResult(Message message) {
        return sendPlane.sendMessageFrameNoWaitResult(message);
    }

    public void recvMessageFrame(Message message, ReceiveFlag flag) {
        receivePlane.recvMessageFrame(message, flag);
    }

    public int recvMessageFrameNoWait(Message message, ReceiveFlag flag) {
        return receivePlane.recvMessageFrameNoWait(message, flag);
    }

    Message nextRecvFrame(ReceiveFlag flags, boolean nonBlocking) {
        return receivePlane.nextRecvFrame(flags, nonBlocking);
    }

    public void sendParts(RoutingId routingId, List<Message> parts,
                   SendFlag flags, boolean nonBlocking) {
        sendPlane.sendParts(routingId, parts, flags, nonBlocking);
    }

    public SendResult sendNoWaitPartsResult(RoutingId routingId, List<Message> parts) {
        return sendPlane.sendNoWaitPartsResult(routingId, parts);
    }

    public void publishParts(String topicId, List<Message> parts,
                      SendFlag flags, boolean nonBlocking) {
        sendPlane.publishParts(topicId, parts, flags, nonBlocking);
    }

    public SendResult publishNoWaitPartsResult(String topicId, List<Message> parts) {
        return sendPlane.publishNoWaitPartsResult(topicId, parts);
    }

    static ZlinkSubmitException submitExceptionFromSendResult(int rc) {
        return switch (rc) {
            case 1 -> new ZlinkSubmitException(SubmitResult.BACKPRESSURED, 0);
            case 2 -> new ZlinkSubmitException(SubmitResult.NOT_CONNECTED, 0);
            case 0 -> throw new IllegalArgumentException("send result indicates success");
            default -> throw new IllegalArgumentException(
                "invalid send result value: " + rc);
        };
    }

    static ZlinkSubmitException submitExceptionFromSendResult(SendResult result) {
        return switch (result) {
            case BACKPRESSURED -> new ZlinkSubmitException(SubmitResult.BACKPRESSURED, 0);
            case NOT_READY -> new ZlinkSubmitException(SubmitResult.NOT_CONNECTED, 0);
            case SENT -> throw new IllegalArgumentException("send result indicates success");
        };
    }

    private static boolean trySendResult(SendResult result) {
        return switch (result) {
            case SENT -> true;
            case BACKPRESSURED -> false;
            case NOT_READY -> throw submitExceptionFromSendResult(result);
        };
    }

    static RoutingId toRoutingId(byte[] value) {
        if (value == null || value.length == 0)
            return null;
        return InternalAccess.routingIdFromTrusted(value);
    }

    static byte[] decodeRoutingIdPtr(MemorySegment nativeRidPtr) {
        if (nativeRidPtr == null || nativeRidPtr.address() == 0)
            return null;
        MemorySegment routingId = nativeRidPtr.reinterpret(
            NativeLayouts.ROUTING_ID_LAYOUT.byteSize());
        return decodeRoutingId(routingId);
    }

    static byte[] decodeRoutingId(MemorySegment nativeRid) {
        int size = nativeRid.get(ValueLayout.JAVA_BYTE,
            NativeLayouts.ROUTING_ID_SIZE_OFFSET) & 0xFF;
        if (size == 0)
            return null;
        byte[] value = new byte[size];
        MemorySegment.copy(nativeRid, NativeLayouts.ROUTING_ID_DATA_OFFSET,
            MemorySegment.ofArray(value), 0, size);
        return value;
    }

    static int normalizeTopicLength(MemorySegment topic, int capacity,
                                            long reportedLength) {
        long len = reportedLength;
        if (len < 0)
            len = 0;
        if (len > capacity)
            len = capacity;
        int bounded = (int) len;
        if (bounded > 0 && topic.get(ValueLayout.JAVA_BYTE, bounded - 1) == 0)
            bounded--;
        return bounded;
    }

    public void prepareRecvLikeOperation() {
        receivePlane.prepareRecvLikeOperation();
    }

    Runnable lazyReceiveCompletion(Received received) {
        return receivePlane.lazyReceiveCompletion(received);
    }

    public Received registerLazyReceive(Received received, boolean hasMore) {
        return receivePlane.registerLazyReceive(received, hasMore);
    }

    RecvScratch recvScratch() {
        return recvScratch.get();
    }

    int pendingFrameCount() {
        return receivePlane.pendingFrameCount();
    }

    byte[] subscriptionAt(long index, MemorySegment lenInOut,
                          MemorySegment isPatternOut, int initialCapacity) {
        int capacity = Math.max(initialCapacity, 0);
        while (true) {
            try (Arena arena = Arena.ofConfined()) {
                MemorySegment filterOut = capacity == 0 ? MemorySegment.NULL
                    : arena.allocate(capacity);
                lenInOut.set(ValueLayout.JAVA_LONG, 0, capacity);
                isPatternOut.set(ValueLayout.JAVA_INT, 0, 0);
                int rc = Native.subscriptionAt(handle, index, filterOut, lenInOut,
                    isPatternOut);
                if (rc == 0) {
                    int actual = toIntLength(lenInOut.get(ValueLayout.JAVA_LONG, 0));
                    if (actual == 0)
                        return new byte[0];
                    byte[] out = new byte[actual];
                    MemorySegment.copy(filterOut, 0, MemorySegment.ofArray(out), 0,
                        actual);
                    return out;
                }
                int errno = Native.errno();
                if (errno == NativeErrno.ENOENT)
                    return null;
                if (errno == NativeErrno.EINVAL) {
                    capacity = toIntLength(lenInOut.get(ValueLayout.JAVA_LONG, 0));
                    continue;
                }
                throw ZlinkException.fromLastError(systems.zlink.contracts.errors.ErrorCategory.CONFIG);
            }
        }
    }

    MemorySegment ensureSendScratch(int length) {
        if (length <= 0)
            return MemorySegment.NULL;
        return socketCore.ensureSendScratch(length);
    }

    static int toIntLength(long length) {
        if (length > Integer.MAX_VALUE) {
            throw new IllegalArgumentException("length too large: " + length);
        }
        return (int) length;
    }

    public void ensureOpen() {
        socketCore.ensureOpen();
    }

}
