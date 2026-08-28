/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.runtime.sockets;

import systems.zlink.contracts.sockets.*;
import systems.zlink.internal.ContractAccess;

import systems.zlink.contracts.core.Context;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.contracts.messaging.Received;
import systems.zlink.contracts.messaging.RequestOperation;
import systems.zlink.contracts.messaging.RoutedSendOperation;
import systems.zlink.runtime.messaging.MessageOperations;
import java.util.List;

final class NativeDealerSocket extends NativeSocketBase implements DealerSocket {
    private final DealerSocketOptions options = ContractAccess.dealerSocketOptions(this);
    private final CoreRequestSupport requestSupport;

    NativeDealerSocket(Context ctx) {
        super(ctx, SocketType.DEALER);
        try {
            requestSupport = new CoreRequestSupport(runtime(), true);
        } catch (RuntimeException error) {
            try {
                runtime().close();
            } catch (RuntimeException ignored) {
            }
            throw error;
        }
    }

    public void bind(String endpoint) { runtime().bind(endpoint); }
    public void connect(String endpoint) { runtime().connect(endpoint); }
    public void unbind(String endpoint) { runtime().unbind(endpoint); }
    public void disconnect(String endpoint) { runtime().disconnect(endpoint); }
    public void disconnectRid(RoutingId routingId) {
        runtime().disconnectRid(routingId);
    }
    public void setRoutingId(RoutingId rid) { runtime().setRoutingId(rid); }
    public RoutingId getRoutingId() { return runtime().getRoutingId(); }

    public RoutedSendOperation send() {
        return MessageOperations.routedSend((parts, timeout) ->
            runtime().sendAsync(parts, timeout),
            (parts, flags) -> runtime().send(parts,
                SendFlag.fromValue(flags.value())));
    }
    SendResult sendNoWaitResult(Message part) {
        return runtime().sendNoWaitResult(part);
    }
    SendResult sendNoWaitResult(List<Message> parts) {
        return runtime().sendNoWaitResult(parts);
    }
    /**
     * Receives into caller-provided {@link Received} storage.
     *
     * <p>HOT PATH: the DONT_WAIT single-part path fills {@code result} in
     * place and avoids allocating a fresh {@link Received}, {@code Message[]},
     * or immutable parts list for each message.
     *
     * @return {@code true} on success, {@code false} when
     * {@link RecvFlags#DONT_WAIT} finds no data.
     */
    public boolean recv(Received result, RecvFlags flags) {
        java.util.Objects.requireNonNull(result, "result");
        java.util.Objects.requireNonNull(flags, "flags");
        return runtime().recvInto(result, ReceiveFlag.fromValue(flags.value()));
    }
    public RequestOperation request() {
        return MessageOperations.request((parts, timeout) ->
            runtime().requestAsync(requestSupport, null, 0L, 0L, parts,
                timeout));
    }
    @Override
    public void close() {
        runtime().close();
        requestSupport.close();
    }
    @Override public DealerSocketOptions options() { return options; }

}
