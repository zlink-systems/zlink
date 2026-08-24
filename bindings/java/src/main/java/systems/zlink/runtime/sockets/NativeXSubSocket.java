/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.runtime.sockets;

import systems.zlink.contracts.sockets.*;
import systems.zlink.internal.ContractAccess;

import systems.zlink.contracts.core.Context;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.messaging.SubscriptionEntry;
import systems.zlink.contracts.messaging.TopicMessage;
import java.util.Optional;

final class NativeXSubSocket extends NativeSocketBase implements XSubSocket {
    private final SubSocketOptions options = ContractAccess.subSocketOptions(this);

    NativeXSubSocket(Context ctx) {
        super(ctx, SocketType.XSUB);
    }

    public void bind(String endpoint) { runtime().bind(endpoint); }
    public void connect(String endpoint) { runtime().connect(endpoint); }
    public void unbind(String endpoint) { runtime().unbind(endpoint); }
    public void disconnect(String endpoint) { runtime().disconnect(endpoint); }
    public void disconnectRid(RoutingId routingId) {
        runtime().disconnectRid(routingId);
    }
    public void setSubscription(String filter) {
        runtime().setSubscription(filter);
    }
    public void unsetSubscription(String filter) {
        runtime().unsetSubscription(filter);
    }

    public Optional<SubscriptionEntry> subscriptionAt(int index) {
        if (index < 0)
            throw new IndexOutOfBoundsException("subscription index " + index);
        var entries = runtime().subscriptions();
        return index < entries.size() ? Optional.of(entries.get(index))
          : Optional.empty();
    }
    public boolean subscribe(TopicMessage result, RecvFlags flags) { return runtime().subscribe(result, ReceiveFlag.fromValue(flags.value())); }
    @Override public SubSocketOptions options() { return options; }
}
