/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.runtime.sockets;

import systems.zlink.internal.ContractAccess;

import systems.zlink.contracts.sockets.*;

import systems.zlink.contracts.core.Context;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.contracts.messaging.Received;
import systems.zlink.contracts.messaging.StreamPacket;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.messaging.SendOperation;
import systems.zlink.runtime.messaging.MessageOperations;
import systems.zlink.runtime.framework.FrameworkStreamOperations;
import systems.zlink.runtime.nativeapi.InternalAccess;
import java.lang.foreign.MemorySegment;
import java.time.Duration;
import java.util.List;
import java.util.Objects;
import java.util.concurrent.CompletionStage;

final class NativeStreamSocket extends NativeSocketBase implements StreamSocket {
    static {
        FrameworkStreamOperations.register((socket, routingId, parts, timeout) ->
            ((NativeStreamSocket) socket).runtime().submitSend(
                routingId, parts));
    }

    private final StreamSocketOptions options = ContractAccess.streamSocketOptions(this);
    NativeStreamSocket(Context ctx) {
        super(ctx, SocketType.STREAM);
    }

    public void bind(String endpoint) { runtime().bind(endpoint); }
    public void unbind(String endpoint) { runtime().unbind(endpoint); }
    public void disconnectRid(RoutingId rid) { runtime().disconnectRid(rid); }
    public void setRoutingId(RoutingId rid) { runtime().setRoutingId(rid); }
    public RoutingId getRoutingId() { return runtime().getRoutingId(); }

    boolean send(int rid, Message part) {
        return runtime().send(RoutingId.from(Integer.toUnsignedLong(rid)),
            part, SendFlag.NONE);
    }
    boolean send(int rid, Message part, SendFlags flags) {
        return runtime().send(RoutingId.from(Integer.toUnsignedLong(rid)),
            part, SendFlag.fromValue(flags.value()));
    }
    SendResult sendNoWaitResult(int rid, Message part) {
        return runtime().sendNoWaitResult(
            RoutingId.from(Integer.toUnsignedLong(rid)), part);
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
        return MessageOperations.send(
            parts -> runtime().submitSend(rid, parts),
            parts -> runtime().submitSendBlocking(rid, parts));
    }
    /** Receives into caller-provided storage. */
    public boolean recv(Received result, RecvFlags flags) {
        Objects.requireNonNull(result, "result");
        Objects.requireNonNull(flags, "flags");
        Received fresh = runtime().recv(ReceiveFlag.fromValue(flags.value()));
        if (fresh == null) return false;
        ContractAccess.receivedAdoptFrom(result, fresh);
        result.getRoutingId().ifPresent(rid ->
            ContractAccess.receivedSetSendSubmitters(result,
                parts -> runtime().submitSend(rid, parts),
                parts -> runtime().submitSendBlocking(rid, parts)));
        return true;
    }
    public boolean recvPacket(StreamPacket result, RecvFlags flags) {
        Objects.requireNonNull(result, "result");
        Objects.requireNonNull(flags, "flags");
        ContractAccess.streamPacketBegin(result);
        Message header = InternalAccess.messageAcquireReceive();
        Message body = InternalAccess.messageAcquireReceive();
        try (java.lang.foreign.Arena arena =
                 java.lang.foreign.Arena.ofConfined()) {
            MemorySegment ridOut = arena.allocate(
                java.lang.foreign.ValueLayout.ADDRESS);
            int rc = systems.zlink.runtime.nativeapi.Native.streamRecvPacket(
                runtime().handle(), ridOut,
                InternalAccess.messageNativeHandle(header),
                InternalAccess.messageNativeHandle(body), flags.value());
            if (rc == RecvResult.NO_DATA.value()) {
                header.close();
                body.close();
                ContractAccess.streamPacketFail(result);
                return false;
            }
            if (rc != RecvResult.OK.value()) {
                throw new systems.zlink.contracts.errors.ZlinkRecvException(
                    RecvResult.fromValue(rc),
                    systems.zlink.runtime.nativeapi.Native.errno());
            }
            InternalAccess.messageFinishReceive(header, false);
            InternalAccess.messageFinishReceive(body, false);
            RoutingId rid = systems.zlink.runtime.nativeapi.NativeRoutingIds
                .readOut(ridOut);
            ContractAccess.streamPacketComplete(result, rid, header, body);
            return true;
        } catch (RuntimeException | Error failure) {
            try { header.close(); } catch (RuntimeException ignored) { }
            try { body.close(); } catch (RuntimeException ignored) { }
            ContractAccess.streamPacketFail(result);
            throw failure;
        }
    }
    @Override
    public void close() {
        runtime().close();
    }
    @Override public StreamSocketOptions options() { return options; }

}
