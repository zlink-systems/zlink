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
import systems.zlink.internal.sockets.SocketOptions;

final class NativeRouterSocket extends NativeSocketBase implements RouterSocket {
    private final RouterSocketOptions options = ContractAccess.routerSocketOptions(this);
    private final OutboundRecordAttemptGate outboundRecordAttempts =
        new OutboundRecordAttemptGate();
    private final RoutedAdmission routedAdmission;
    private final Object routedRequests =
      InternalAccess.routerReceiveSupport(this, false);
    private final ContractAccess.RoutedSingleSendInvoker receivedSingleSender =
      this::sendReceivedSingle;
    private final ContractAccess.RoutedMultipartSendInvoker receivedMultipartSender =
      this::sendReceivedMultipart;

    NativeRouterSocket(Context ctx) {
        super(ctx, SocketType.ROUTER);
        try {
            routedAdmission = new RoutedAdmission(handle(), false,
                outboundRecordAttempts);
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
        return MessageOperations.routedSend(parts -> routedAdmission.send(
            rid, parts, runtime().getOption(SocketOptions.SNDTIMEO)));
    }

    private boolean sendInternal(RoutingId rid, Message part, SendFlags flags) {
        return outboundRecordAttempts.call(() -> runtime().send(rid, part,
            SendFlag.fromValue(flags.value())));
    }
    private boolean sendInternal(RoutingId rid, List<Message> parts, SendFlags flags) {
        return outboundRecordAttempts.call(() -> runtime().send(rid, parts,
            SendFlag.fromValue(flags.value())));
    }
    private boolean sendReceivedSingle(byte[] routingIdBytes, Message part,
                                       SendFlags flags) {
        return outboundRecordAttempts.call(() -> runtime().send(
            routingIdBytes, part, SendFlag.fromValue(flags.value())));
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

    public boolean recvRetained(Received result, RecvFlags flags) {
        Objects.requireNonNull(result, "result");
        Objects.requireNonNull(flags, "flags");
        boolean ok = InternalAccess.routerRecvRetainedInto(routedRequests,
            result, flags);
        if (ok) attachSendSender(result);
        return ok;
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
    public void setSendReadyHandler(SendReadyHandler handler) { runtime().setSendReadyHandler(handler); }

    public RequestOperation request(RoutingId rid) {
        Objects.requireNonNull(rid, "rid");
        return MessageOperations.request((parts, timeout) ->
            routedAdmission.request(rid, parts, timeout));
    }

    public RequestOperation request(RoutingId rid,
                                    long transportPairId,
                                    long transportPairGeneration) {
        if (transportPairId == 0 || transportPairGeneration == 0) {
            throw new IllegalArgumentException(
                "transport pair identity must be non-zero");
        }
        RoutedAdmission.Target target = new RoutedAdmission.Target(rid,
            transportPairId, transportPairGeneration);
        return MessageOperations.request((parts, timeout) ->
            routedAdmission.request(target, parts, timeout));
    }

    public ReplyOperation reply(RoutingId rid, long requestSequence) {
        return MessageOperations.reply(parts -> submitReply(rid,
            requestSequence, parts));
    }

    void submitReply(RoutingId rid, long requestSequence,
                     List<Message> parts) {
        outboundRecordAttempts.run(() -> InternalAccess.routerReply(this, rid,
            requestSequence, parts));
    }

    @Override
    public void close() {
        routedAdmission.prepareClose();
        boolean closed = false;
        try {
            outboundRecordAttempts.run(runtime()::close);
            closed = true;
            routedAdmission.commitClose();
            InternalAccess.routerReceiveBeginClose(routedRequests);
        } finally {
            if (closed) {
                routedAdmission.finishClose();
                InternalAccess.routerReceiveFinishClose(routedRequests);
            } else {
                routedAdmission.abortClose();
            }
        }
    }
    @Override public RouterSocketOptions options() { return options; }

}
