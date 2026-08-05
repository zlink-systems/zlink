/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.runtime.sockets;

import systems.zlink.contracts.sockets.*;
import systems.zlink.internal.ContractAccess;

import systems.zlink.contracts.core.Context;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.messaging.SendOperation;
import systems.zlink.contracts.messaging.SubscriptionEvent;
import systems.zlink.runtime.messaging.MessageOperations;
final class NativeXPubSocket extends NativeSocketBase implements XPubSocket {
    private final PubSocketOptions options = ContractAccess.pubSocketOptions(this);

    NativeXPubSocket(Context ctx) {
        super(ctx, SocketType.XPUB);
    }

    public void bind(String endpoint) { runtime().bind(endpoint); }
    public void connect(String endpoint) { runtime().connect(endpoint); }
    public void unbind(String endpoint) { runtime().unbind(endpoint); }
    public void disconnect(String endpoint) { runtime().disconnect(endpoint); }
    public void disconnectRid(RoutingId routingId) {
        runtime().disconnectRid(routingId);
    }
    public void setRoutingId(RoutingId rid) { runtime().setRoutingId(rid); }

    public SendOperation publish(String topicId) {
        return MessageOperations.send((parts, flags) ->
            runtime().publish(topicId, parts, SendFlag.fromValue(flags.value())));
    }
    public boolean receiveSubscriptionEvent(SubscriptionEvent result, RecvFlags flags) { return runtime().receiveSubscriptionEvent(result, ReceiveFlag.fromValue(flags.value())); }
    public void setSendReadyHandler(SendReadyHandler handler) { runtime().setSendReadyHandler(handler); }
    @Override public PubSocketOptions options() { return options; }
}
