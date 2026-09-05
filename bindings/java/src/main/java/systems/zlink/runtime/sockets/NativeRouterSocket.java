/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.runtime.sockets;

import systems.zlink.contracts.sockets.*;

import systems.zlink.contracts.core.Context;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.contracts.messaging.Received;
import systems.zlink.contracts.messaging.ReplyOperation;
import systems.zlink.contracts.messaging.RequestOperation;
import systems.zlink.contracts.messaging.ReplyToken;
import systems.zlink.contracts.messaging.SendOperation;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.internal.ContractAccess;
import systems.zlink.runtime.messaging.MessageOperations;
import systems.zlink.runtime.nativeapi.InternalAccess;
import java.util.List;
import java.util.Objects;

final class NativeRouterSocket extends NativeSocketBase implements RouterSocket {
    private final RouterSocketOptions options = ContractAccess.routerSocketOptions(this);
    private final Object routedRequests =
      InternalAccess.routerReceiveSupport(this, false);
    private final ContractAccess.RoutedReplyInvoker receivedReplySender =
      this::submitReceivedReply;

    NativeRouterSocket(Context ctx) {
        super(ctx, SocketType.ROUTER);
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

    public SendOperation send(RoutingId rid) {
        Objects.requireNonNull(rid, "rid");
        return MessageOperations.send(
            parts -> runtime().submitSend(rid, parts),
            parts -> runtime().submitSendBlocking(rid, parts));
    }

    private void submitReceivedReply(byte[] routingIdBytes,
                                     long replyTokenValue,
                                     List<Message> parts) {
        runtime().submitReply(ContractAccess.routingIdFromTrusted(
            routingIdBytes), replyTokenValue, parts);
    }
    /** Receives into caller-provided storage. */
    public boolean recv(Received result, RecvFlags flags) {
        java.util.Objects.requireNonNull(result, "result");
        java.util.Objects.requireNonNull(flags, "flags");
        boolean ok = InternalAccess.routerRecvInto(routedRequests, result,
            flags);
        if (ok) attachSendSender(result);
        return ok;
    }

    void attachSendSender(Received result) {
        ContractAccess.receivedSetReplyTokenOwner(result, this);
        RoutingId captured = result.getRoutingId().orElse(null);
        if (captured != null) {
            ContractAccess.receivedSetSendSubmitters(result,
                parts -> runtime().submitSend(captured, parts),
                parts -> runtime().submitSendBlocking(captured, parts));
        }
        if (ContractAccess.receivedHasRoutingIdBytes(result)) {
            ContractAccess.receivedSetRoutedReplySender(result,
                receivedReplySender);
        }
    }
    public RequestOperation request(RoutingId rid) {
        Objects.requireNonNull(rid, "rid");
        return MessageOperations.request(
            (parts, timeout) -> runtime().submitRequest(rid, parts, timeout),
            (parts, timeout) -> runtime().submitRequestBlocking(rid, parts,
                timeout));
    }

    public ReplyOperation reply(RoutingId rid, ReplyToken token) {
        Objects.requireNonNull(rid, "rid");
        Objects.requireNonNull(token, "token");
        if (ContractAccess.replyTokenOwner(token) != this) {
            throw new IllegalArgumentException(
                "reply token belongs to a different router");
        }
        long value = ContractAccess.replyTokenValue(token);
        return MessageOperations.reply(parts -> runtime().submitReply(rid,
            value, parts));
    }

    void submitReply(RoutingId rid, long value, List<Message> parts) {
        runtime().submitReply(rid, value, parts);
    }

    @Override
    public void close() {
        runtime().close();
        InternalAccess.routerReceiveBeginClose(routedRequests);
        InternalAccess.routerReceiveFinishClose(routedRequests);
    }
    @Override public RouterSocketOptions options() { return options; }

}
