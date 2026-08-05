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
import systems.zlink.runtime.nativeapi.InternalAccess;
import systems.zlink.runtime.sockets.StreamUInt32FramedNativeHandler;
import java.lang.foreign.MemorySegment;
import java.util.List;
import java.util.Objects;

final class NativeStreamSocket extends NativeSocketBase implements StreamSocket {
    private final StreamSocketOptions options = ContractAccess.streamSocketOptions(this);

    NativeStreamSocket(Context ctx) {
        super(ctx, SocketType.STREAM);
    }

    public void bind(String endpoint) { runtime().bind(endpoint); }
    public void unbind(String endpoint) { runtime().unbind(endpoint); }
    public void setRoutingId(RoutingId rid) { runtime().setRoutingId(rid); }
    public RoutingId getRoutingId() { return runtime().getRoutingId(); }

    boolean send(int rid, Message part) {
        return runtime().send(RoutingId.from(Integer.toUnsignedLong(rid)), part,
            SendFlag.NONE);
    }
    boolean send(int rid, Message part, SendFlags flags) {
        return runtime().send(RoutingId.from(Integer.toUnsignedLong(rid)), part,
            SendFlag.fromValue(flags.value()));
    }
    SendResult sendNoWaitResult(int rid, Message part) {
        return runtime().sendNoWaitResult(RoutingId.from(Integer.toUnsignedLong(rid)),
            part);
    }
    SendResult sendNoWaitResult(RoutingId rid, Message part) {
        return runtime().sendNoWaitResult(rid, part);
    }
    int send(int rid, MemorySegment payload, int length, SendFlags flags) {
        return runtime().send(rid, payload, length, flags.value());
    }
    int sendCopied(int rid, MemorySegment payload, int length,
                   SendFlags flags) {
        return runtime().sendCopied(rid, payload, length, flags.value());
    }
    SendResult sendNoWaitResult(RoutingId rid, List<Message> parts) {
        return runtime().sendNoWaitResult(rid, parts);
    }
    public SendOperation send(RoutingId rid) {
        Objects.requireNonNull(rid, "rid");
        return MessageOperations.send((parts, flags) ->
            runtime().send(rid, parts,
                SendFlag.fromValue(flags.value())));
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
    public void setSendReadyHandler(SendReadyHandler handler) { runtime().setSendReadyHandler(handler); }
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
        runtime().close();
    }
    @Override public StreamSocketOptions options() { return options; }
}
