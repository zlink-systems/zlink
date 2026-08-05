/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.runtime.sockets;

import systems.zlink.contracts.sockets.*;

import systems.zlink.contracts.core.Context;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.contracts.messaging.Received;
import systems.zlink.contracts.messaging.ReplyOperation;
import systems.zlink.contracts.messaging.RequestOperation;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.messaging.SendOperation;
import systems.zlink.contracts.errors.ZlinkSubmitException;
import systems.zlink.internal.ContractAccess;
import systems.zlink.runtime.messaging.MessageOperations;
import systems.zlink.runtime.nativeapi.InternalAccess;
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.StandardOpenOption;
import java.time.Duration;
import java.util.List;
import java.util.Objects;
import java.lang.foreign.Arena;
import java.lang.foreign.MemorySegment;
import systems.zlink.runtime.nativeapi.Native;
import systems.zlink.runtime.nativeapi.NativeLayouts;
import systems.zlink.runtime.nativeapi.MessagePartsBuffer;
import systems.zlink.runtime.nativeapi.NativeRoutingIds;
import java.util.concurrent.CompletableFuture;

final class NativeRouterSocket extends NativeSocketBase implements RouterSocket {
    private static final boolean DEBUG_REQREP =
      Boolean.getBoolean("zlink.reqrep.debug");
    private final RouterSocketOptions options = ContractAccess.routerSocketOptions(this);
    private final Object routedRequests =
      InternalAccess.routerReceiveSupport(this, false);

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
        return MessageOperations.send(
            (part, flags) -> sendInternal(rid, part, flags),
            (parts, flags) -> sendInternal(rid, parts, flags));
    }

    private boolean sendInternal(RoutingId rid, Message part, SendFlags flags) {
        return runtime().send(rid, part, SendFlag.fromValue(flags.value()));
    }
    private boolean sendInternal(RoutingId rid, List<Message> parts, SendFlags flags) {
        return runtime().send(rid, parts, SendFlag.fromValue(flags.value()));
    }
    /** Receives into caller-provided storage. */
    public boolean recv(Received result, RecvFlags flags) {
        java.util.Objects.requireNonNull(result, "result");
        java.util.Objects.requireNonNull(flags, "flags");
        if (flags == RecvFlags.DONT_WAIT) {
            boolean ok = InternalAccess.routerRecvInto(routedRequests, result,
                flags);
            if (ok) attachSendRouter(result);
            return ok;
        }
        Received fresh = InternalAccess.routerRecv(routedRequests, flags);
        if (fresh == null) return false;
        ContractAccess.receivedAdoptFrom(result, fresh);
        attachSendRouter(result);
        return true;
    }

    private void attachSendRouter(Received result) {
        if (result.getRoutingId().isPresent()) {
            attachSendSender(result);
        }
    }

    void attachSendSender(Received result) {
        RoutingId nodeRid = result.getRoutingId().orElse(null);
        if (nodeRid == null) return;
        ContractAccess.receivedSetSendSender(result, (parts, flags) ->
            sendInternal(nodeRid, parts, flags));
    }
    public void setSendReadyHandler(SendReadyHandler handler) { runtime().setSendReadyHandler(handler); }

    public RequestOperation request(RoutingId rid) {
        return MessageOperations.request(
            (parts, flags, timeout) -> requestStage(rid, parts, flags, timeout),
            (parts, callback, flags, timeout) ->
                requestCallback(rid, parts, callback, flags, timeout));
    }

    private CompletableFuture<List<Message>> requestStage(RoutingId rid,
                                                          List<Message> parts,
                                                          SendFlags flags,
                                                          Duration timeout) {
        return InternalAccess.routerRequestAsync(this, rid, parts, flags,
            timeout);
    }

    private boolean requestCallback(RoutingId rid,
                                    List<Message> parts,
                                    RequestCallback callback,
                                    SendFlags flags,
                                    Duration timeout) {
        return InternalAccess.routerRequestCallback(this, rid, parts, callback,
            flags, timeout);
    }

    public ReplyOperation reply(RoutingId rid, long requestSequence) {
        return MessageOperations.reply(parts ->
            InternalAccess.routerReply(this, rid, requestSequence, parts));
    }

    public boolean trySendCompletionControl(RoutingId peerRid,
                                            List<Message> parts) {
        Objects.requireNonNull(peerRid, "peerRid");
        Objects.requireNonNull(parts, "parts");
        if (parts.isEmpty())
            throw new IllegalArgumentException("parts must not be empty");

        try (Arena arena = Arena.ofConfined()) {
            MemorySegment nativeRid = NativeRoutingIds.allocate(arena, peerRid);
            MessagePartsBuffer validated = new MessagePartsBuffer();
            for (int i = 0; i < parts.size(); i++) {
                validated.add(Objects.requireNonNull(parts.get(i),
                    "parts[" + i + "]"));
            }
            MemorySegment nativeParts = validated.copyToNativeArray(arena);
            long stride = NativeLayouts.MESSAGE_LAYOUT.byteSize();
            try {
                for (int i = 0; i < parts.size(); i++) {
                    MemorySegment nativePart = nativeParts.asSlice(
                        i * stride, stride);
                    int partFlag = i + 1 < parts.size()
                        ? Native.PART_MORE : Native.PART_FINAL;
                    int rc = Native.routerCompletionControlPart(
                        InternalAccess.socketHandle(this), nativeRid,
                        nativePart, partFlag);
                    int nativeErrno = rc == SubmitResult.OK.value()
                        ? 0 : Native.errno();
                    if (rc == SubmitResult.OK.value())
                        continue;
                    if (rc == SubmitResult.BACKPRESSURED.value())
                        return false;
                    throw new ZlinkSubmitException(SubmitResult.fromValue(rc),
                        nativeErrno);
                }
                return true;
            } finally {
                MessagePartsBuffer.closeNativeArray(nativeParts, parts.size());
            }
        }
    }

    public void setCompletionControlHandler(CompletionControlHandler handler) {
        runtime().setCompletionControlHandler(handler);
    }

    @Override
    public void close() {
        debug("router close begin");
        InternalAccess.routerReceiveBeginClose(routedRequests);
        try {
            runtime().close();
        } finally {
            InternalAccess.routerReceiveFinishClose(routedRequests);
        }
        debug("router close end");
    }
    @Override public RouterSocketOptions options() { return options; }

    private static void debug(String message) {
        if (DEBUG_REQREP) {
            try {
                Files.writeString(Path.of("/tmp/zlink-reqrep.log"),
                    "[router-socket] " + message + System.lineSeparator(),
                    StandardOpenOption.CREATE, StandardOpenOption.APPEND);
            } catch (Exception ignored) {
            }
        }
    }

}
