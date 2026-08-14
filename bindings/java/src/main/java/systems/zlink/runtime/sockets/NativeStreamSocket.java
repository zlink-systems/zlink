/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.runtime.sockets;

import systems.zlink.internal.ContractAccess;

import systems.zlink.contracts.sockets.*;

import systems.zlink.contracts.core.Context;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.contracts.messaging.Received;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.messaging.SendOperation;
import systems.zlink.runtime.messaging.MessageOperations;
import systems.zlink.runtime.framework.FrameworkStreamOperations;
import systems.zlink.runtime.nativeapi.InternalAccess;
import systems.zlink.runtime.sockets.StreamUInt32FramedNativeHandler;
import java.lang.foreign.MemorySegment;
import java.time.Duration;
import java.util.List;
import java.util.Objects;
import java.util.concurrent.CompletionStage;

final class NativeStreamSocket extends NativeSocketBase implements StreamSocket {
    static {
        FrameworkStreamOperations.register((socket, routingId, parts, timeout) ->
            ((NativeStreamSocket) socket).sendAsync(
                routingId, parts, timeout));
    }

    private final StreamSocketOptions options = ContractAccess.streamSocketOptions(this);
    private final OutboundRecordAttemptGate outboundRecordAttempts =
        new OutboundRecordAttemptGate();
    private final StreamAdmission streamAdmission;

    NativeStreamSocket(Context ctx) {
        super(ctx, SocketType.STREAM);
        streamAdmission = new StreamAdmission(
            runtime(), outboundRecordAttempts);
    }

    public void bind(String endpoint) { runtime().bind(endpoint); }
    public void unbind(String endpoint) { runtime().unbind(endpoint); }
    public void disconnectRid(RoutingId rid) { runtime().disconnectRid(rid); }
    public void setRoutingId(RoutingId rid) { runtime().setRoutingId(rid); }
    public RoutingId getRoutingId() { return runtime().getRoutingId(); }

    boolean send(int rid, Message part) {
        return outboundRecordAttempts.call(() -> runtime().send(
            RoutingId.from(Integer.toUnsignedLong(rid)), part,
            SendFlag.NONE));
    }
    boolean send(int rid, Message part, SendFlags flags) {
        return outboundRecordAttempts.call(() -> runtime().send(
            RoutingId.from(Integer.toUnsignedLong(rid)), part,
            SendFlag.fromValue(flags.value())));
    }
    SendResult sendNoWaitResult(int rid, Message part) {
        return outboundRecordAttempts.call(() -> runtime().sendNoWaitResult(
            RoutingId.from(Integer.toUnsignedLong(rid)), part));
    }
    SendResult sendNoWaitResult(RoutingId rid, Message part) {
        return outboundRecordAttempts.call(
            () -> runtime().sendNoWaitResult(rid, part));
    }
    int send(int rid, MemorySegment payload, int length, SendFlags flags) {
        return outboundRecordAttempts.call(
            () -> runtime().send(rid, payload, length, flags.value()));
    }
    int sendCopied(int rid, MemorySegment payload, int length,
                   SendFlags flags) {
        return outboundRecordAttempts.call(
            () -> runtime().sendCopied(
                rid, payload, length, flags.value()));
    }
    SendResult sendNoWaitResult(RoutingId rid, List<Message> parts) {
        return outboundRecordAttempts.call(
            () -> runtime().sendNoWaitResult(rid, parts));
    }
    public SendOperation send(RoutingId rid) {
        Objects.requireNonNull(rid, "rid");
        return MessageOperations.send((parts, flags) ->
            outboundRecordAttempts.call(() -> runtime().send(rid, parts,
                SendFlag.fromValue(flags.value()))));
    }

    private CompletionStage<Void> sendAsync(
        RoutingId routingId,
        List<Message> parts,
        Duration timeout) {
        int timeoutMillis = timeout == null
            ? runtime().getOption(
                systems.zlink.internal.sockets.SocketOptions.SNDTIMEO)
            : normalizedTimeoutMillis(timeout);
        return streamAdmission.send(routingId, parts, timeoutMillis);
    }
    /** Receives into caller-provided storage. */
    public boolean recv(Received result, RecvFlags flags) {
        Objects.requireNonNull(result, "result");
        Objects.requireNonNull(flags, "flags");
        Received fresh = runtime().recv(ReceiveFlag.fromValue(flags.value()));
        if (fresh == null) return false;
        ContractAccess.receivedAdoptFrom(result, fresh);
        result.getRoutingId().ifPresent(rid ->
            InternalAccess.receivedSetSendSender(result, (parts, sendFlags) -> runtime().send(rid, parts,
                SendFlag.fromValue(sendFlags.value()))));
        return true;
    }
    public boolean recvRetained(Received result, RecvFlags flags) {
        Objects.requireNonNull(result, "result");
        Objects.requireNonNull(flags, "flags");
        boolean ok = runtime().recvRetainedInto(result,
            ReceiveFlag.fromValue(flags.value()));
        if (!ok) return false;
        result.getRoutingId().ifPresent(rid ->
            InternalAccess.receivedSetSendSender(result, (parts, sendFlags) ->
                runtime().send(rid, parts,
                    SendFlag.fromValue(sendFlags.value()))));
        return true;
    }
    public void setSendReadyHandler(SendReadyHandler handler) {
        streamAdmission.setObserver(handler);
    }
    public void onPacket(StreamPacketHandler handler) {
        Objects.requireNonNull(handler, "handler");
        runtime().attachStreamPacket((StreamFramedPacketHandler)
            (routingId, header, body) -> handler.onPacket(routingId, header,
                body));
    }
    void onFramedPacket(StreamFramedPacketHandler handler) {
        runtime().attachStreamPacket(handler);
    }
    void onFramedPacket(StreamUInt32FramedPacketHandler handler) {
        runtime().attachStreamPacket(handler);
    }
    void onFramedPacketNative(StreamUInt32FramedNativeHandler handler) {
        runtime().attachStreamPacket(handler);
    }
    @Override
    public void close() {
        streamAdmission.prepareClose();
        boolean closed = false;
        try {
            outboundRecordAttempts.run(runtime()::close);
            closed = true;
            streamAdmission.commitClose();
        } finally {
            if (closed) {
                streamAdmission.finishClose();
            } else {
                streamAdmission.abortClose();
            }
        }
    }
    @Override public StreamSocketOptions options() { return options; }

    private static int normalizedTimeoutMillis(Duration timeout) {
        if (timeout.isZero() || timeout.isNegative()) {
            throw new IllegalArgumentException(
                "STREAM submission timeout must be positive");
        }
        long seconds = timeout.getSeconds();
        if (seconds > Integer.MAX_VALUE / 1000L) {
            throw new IllegalArgumentException(
                "STREAM submission timeout exceeds Integer.MAX_VALUE ms");
        }
        long millis = seconds * 1000L
            + (timeout.getNano() + 999_999L) / 1_000_000L;
        if (millis < 1L || millis > Integer.MAX_VALUE) {
            throw new IllegalArgumentException(
                "STREAM submission timeout must normalize to 1..Integer.MAX_VALUE ms");
        }
        return (int) millis;
    }
}
