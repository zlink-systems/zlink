/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.runtime.sockets;

import systems.zlink.contracts.sockets.*;
import systems.zlink.internal.ContractAccess;

import systems.zlink.contracts.core.Context;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.contracts.messaging.Received;
import systems.zlink.contracts.messaging.RequestOperation;
import systems.zlink.contracts.messaging.SendOperation;
import systems.zlink.runtime.messaging.MessageOperations;
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.StandardOpenOption;
import java.time.Duration;
import java.util.List;
import java.util.concurrent.CompletableFuture;
import systems.zlink.runtime.nativeapi.InternalAccess;

final class NativeDealerSocket extends NativeSocketBase implements DealerSocket {
    private static final boolean DEBUG_REQREP =
      Boolean.getBoolean("zlink.reqrep.debug");
    private final DealerSocketOptions options = ContractAccess.dealerSocketOptions(this);

    NativeDealerSocket(Context ctx) {
        super(ctx, SocketType.DEALER);
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

    public SendOperation send() {
        return MessageOperations.send(
            (part, flags) -> runtime().send(part, SendFlag.fromValue(flags.value())),
            (parts, flags) -> runtime().send(parts, SendFlag.fromValue(flags.value())));
    }
    SendResult sendNoWaitResult(Message part) { return runtime().sendNoWaitResult(part); }
    SendResult sendNoWaitResult(List<Message> parts) { return runtime().sendNoWaitResult(parts); }
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
    public void setSendReadyHandler(SendReadyHandler handler) { runtime().setSendReadyHandler(handler); }
    public RequestOperation request() {
        return MessageOperations.request(this::requestStage, this::requestCallback);
    }

    private CompletableFuture<List<Message>> requestStage(List<Message> parts,
                                                          SendFlags flags,
                                                          Duration timeout) {
        return InternalAccess.dealerRequestAsync(this, parts, flags, timeout);
    }

    private boolean requestCallback(List<Message> parts,
                                    RequestCallback callback,
                                    SendFlags flags,
                                    Duration timeout) {
        return InternalAccess.dealerRequestCallback(this, parts, callback, flags,
            timeout);
    }
    @Override
    public void close() {
        debug("dealer close begin");
        try {
            runtime().close();
        } finally {
            debug("dealer close end");
        }
    }
    @Override public DealerSocketOptions options() { return options; }

    private static void debug(String message) {
        if (DEBUG_REQREP) {
            try {
                Files.writeString(Path.of("/tmp/zlink-reqrep.log"),
                    "[dealer-socket] " + message + System.lineSeparator(),
                    StandardOpenOption.CREATE, StandardOpenOption.APPEND);
            } catch (Exception ignored) {
            }
        }
    }

}
