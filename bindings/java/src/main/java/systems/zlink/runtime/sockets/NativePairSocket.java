/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.runtime.sockets;

import systems.zlink.contracts.sockets.*;

import systems.zlink.contracts.core.Context;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.contracts.messaging.Received;
import systems.zlink.contracts.messaging.SendOperation;
import systems.zlink.runtime.messaging.MessageOperations;
import java.util.List;
final class NativePairSocket extends NativeSocketBase implements PairSocket {
    NativePairSocket(Context ctx) {
        super(ctx, SocketType.PAIR);
    }

    public void bind(String endpoint) { runtime().bind(endpoint); }
    public void connect(String endpoint) { runtime().connect(endpoint); }
    public void unbind(String endpoint) { runtime().unbind(endpoint); }
    public void disconnect(String endpoint) { runtime().disconnect(endpoint); }
    public void disconnectRid(RoutingId routingId) {
        runtime().disconnectRid(routingId);
    }

    public SendOperation send() {
        return MessageOperations.send(
            (part, flags) -> runtime().send(part, SendFlag.fromValue(flags.value())),
            (parts, flags) -> runtime().send(parts, SendFlag.fromValue(flags.value())));
    }
    SendResult sendNoWaitResult(Message part) { return runtime().sendNoWaitResult(part); }
    SendResult sendNoWaitResult(List<Message> parts) { return runtime().sendNoWaitResult(parts); }
    Received recv(RecvFlags flags) { return runtime().recv(ReceiveFlag.fromValue(flags.value())); }
    /**
     * Receives into caller-provided {@link Received} storage.
     *
     * <p>HOT PATH: PAIR single-part recv fills {@code result} in place and
     * avoids allocating a fresh {@link Received} plus immutable parts list for
     * each message.
     */
    public boolean recv(Received result, RecvFlags flags) {
        java.util.Objects.requireNonNull(result, "result");
        java.util.Objects.requireNonNull(flags, "flags");
        return runtime().recvInto(result, ReceiveFlag.fromValue(flags.value()));
    }
    public void setSendReadyHandler(SendReadyHandler handler) { runtime().setSendReadyHandler(handler); }
}
