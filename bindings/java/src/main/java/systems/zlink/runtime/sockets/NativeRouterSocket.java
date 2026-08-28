/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.runtime.sockets;

import systems.zlink.contracts.sockets.*;

import systems.zlink.contracts.core.Context;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.contracts.messaging.Received;
import systems.zlink.contracts.messaging.ReplyOperation;
import systems.zlink.contracts.messaging.RequestOperation;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.messaging.RoutedSendOperation;
import systems.zlink.internal.ContractAccess;
import systems.zlink.runtime.messaging.MessageOperations;
import systems.zlink.runtime.nativeapi.InternalAccess;
import java.util.List;
import java.util.Objects;

final class NativeRouterSocket extends NativeSocketBase implements RouterSocket {
    private final RouterSocketOptions options = ContractAccess.routerSocketOptions(this);
    private final CoreRequestSupport requestSupport;
    private final Object routedRequests =
      InternalAccess.routerReceiveSupport(this, false);
    private final ContractAccess.RoutedSingleSendInvoker receivedSingleSender =
      this::sendReceivedSingle;
    private final ContractAccess.RoutedMultipartSendInvoker receivedMultipartSender =
      this::sendReceivedMultipart;

    NativeRouterSocket(Context ctx) {
        super(ctx, SocketType.ROUTER);
        try {
            requestSupport = new CoreRequestSupport(runtime(), false);
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
    public void disconnectTransportPair(long transportPairId,
                                       long transportPairGeneration) {
        runtime().disconnectTransportPair(
            transportPairId, transportPairGeneration);
    }
    public void setRoutingId(RoutingId rid) { runtime().setRoutingId(rid); }
    public RoutingId getRoutingId() { return runtime().getRoutingId(); }

    public RoutedSendOperation send(RoutingId rid) {
        Objects.requireNonNull(rid, "rid");
        return MessageOperations.routedSend((parts, timeout) ->
            runtime().sendAsync(rid, parts, timeout),
            (parts, flags) -> runtime().send(rid, parts,
                SendFlag.fromValue(flags.value())));
    }

    public RoutedSendOperation send(RoutingId rid,
                                    long transportPairId,
                                    long transportPairGeneration) {
        if (transportPairId == 0 || transportPairGeneration == 0) {
            throw new IllegalArgumentException(
                "transport pair identity must be non-zero");
        }
        return MessageOperations.routedSend((parts, timeout) ->
            runtime().sendAsync(rid, transportPairId, transportPairGeneration,
                parts, timeout),
            (parts, flags) -> runtime().send(rid, transportPairId,
                transportPairGeneration, parts,
                SendFlag.fromValue(flags.value())));
    }

    private boolean sendInternal(RoutingId rid, Message part, SendFlags flags) {
        return runtime().send(rid, part,
            SendFlag.fromValue(flags.value()));
    }
    private boolean sendInternal(RoutingId rid, List<Message> parts, SendFlags flags) {
        return runtime().send(rid, parts,
            SendFlag.fromValue(flags.value()));
    }
    private boolean sendReceivedSingle(byte[] routingIdBytes, Message part,
                                       SendFlags flags) {
        return runtime().send(routingIdBytes, part,
            SendFlag.fromValue(flags.value()));
    }
    private boolean sendReceivedMultipart(byte[] routingIdBytes,
                                          List<Message> parts,
                                          SendFlags flags) {
        return sendInternal(ContractAccess.routingIdFromTrusted(routingIdBytes),
            parts, flags);
    }
    /** Receives into caller-provided storage. */
    public boolean recv(Received result, RecvFlags flags) {
        java.util.Objects.requireNonNull(result, "result");
        java.util.Objects.requireNonNull(flags, "flags");
        if (flags == RecvFlags.DONT_WAIT) {
            boolean ok = InternalAccess.routerRecvInto(routedRequests, result,
                flags);
            if (ok) attachSendSender(result);
            return ok;
        }
        Received fresh = InternalAccess.routerRecv(routedRequests, flags);
        if (fresh == null) return false;
        ContractAccess.receivedAdoptFrom(result, fresh);
        attachSendSender(result);
        return true;
    }

    void attachSendSender(Received result) {
        if (ContractAccess.receivedHasRoutingIdBytes(result)) {
            ContractAccess.receivedSetRoutedSenders(result,
                receivedSingleSender, receivedMultipartSender);
            return;
        }
        RoutingId nodeRid = result.getRoutingId().orElse(null);
        if (nodeRid == null) return;
        ContractAccess.receivedSetSendSender(result, (parts, flags) ->
            sendInternal(nodeRid, parts, flags));
        ContractAccess.receivedSetSingleSendSender(result, (part, flags) ->
            sendInternal(nodeRid, part, flags));
    }
    public RequestOperation request(RoutingId rid) {
        Objects.requireNonNull(rid, "rid");
        return MessageOperations.request((parts, timeout) ->
            runtime().requestAsync(requestSupport, rid, 0L, 0L, parts,
                timeout),
            (parts, timeout, flags) -> runtime().requestSync(requestSupport,
                rid, 0L, 0L, parts, timeout, flags));
    }

    public RequestOperation request(RoutingId rid,
                                    long transportPairId,
                                    long transportPairGeneration) {
        if (transportPairId == 0 || transportPairGeneration == 0) {
            throw new IllegalArgumentException(
                "transport pair identity must be non-zero");
        }
        return MessageOperations.request((parts, timeout) ->
            runtime().requestAsync(requestSupport, rid, transportPairId,
                transportPairGeneration, parts, timeout),
            (parts, timeout, flags) -> runtime().requestSync(requestSupport,
                rid, transportPairId, transportPairGeneration, parts,
                timeout, flags));
    }

    public ReplyOperation reply(RoutingId rid, long requestSequence) {
        return MessageOperations.reply(parts -> submitReply(rid,
            requestSequence, parts));
    }

    void submitReply(RoutingId rid, long requestSequence,
                     List<Message> parts) {
        InternalAccess.routerReply(this, rid, requestSequence, parts);
    }

    @Override
    public void close() {
        runtime().close();
        requestSupport.close();
        InternalAccess.routerReceiveBeginClose(routedRequests);
        InternalAccess.routerReceiveFinishClose(routedRequests);
    }
    @Override public RouterSocketOptions options() { return options; }

}
