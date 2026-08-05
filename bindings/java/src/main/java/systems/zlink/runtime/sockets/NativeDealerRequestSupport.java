/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.runtime.sockets;

import systems.zlink.contracts.errors.ZlinkSubmitException;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.contracts.messaging.Received;
import systems.zlink.contracts.sockets.DealerSocket;
import systems.zlink.contracts.sockets.RecvFlags;
import systems.zlink.contracts.sockets.RequestCallback;
import systems.zlink.contracts.sockets.RequestResult;
import systems.zlink.contracts.sockets.RouterSocket;
import systems.zlink.contracts.sockets.SendFlags;
import systems.zlink.runtime.nativeapi.InternalAccess;
import systems.zlink.runtime.nativeapi.Native;
import systems.zlink.runtime.nativeapi.NativeLayouts;
import systems.zlink.runtime.nativeapi.RequestReplySupport;
import systems.zlink.runtime.nativeapi.RoutedRequestSupport;
import java.lang.foreign.Arena;
import java.lang.foreign.MemorySegment;
import java.time.Duration;
import java.util.List;
import java.util.Objects;
import java.util.concurrent.CompletableFuture;
import java.util.function.BiConsumer;

final class NativeDealerRequestSupport {
    static {
        InternalAccess.register(new InternalAccess.RuntimeSocketAccess() {
            @Override
            public CompletableFuture<List<Message>> dealerRequestAsync(
                    DealerSocket socket, List<Message> parts, SendFlags flags,
                    Duration timeout) {
                return NativeDealerRequestSupport.requestStage(socket, parts,
                    flags, timeout);
            }

            @Override
            public boolean dealerRequestCallback(
                    DealerSocket socket, List<Message> parts,
                    RequestCallback callback, SendFlags flags,
                    Duration timeout) {
                return NativeDealerRequestSupport.requestCallback(socket, parts,
                    callback, flags, timeout);
            }

            @Override
            public Object routerReceiveSupport(RouterSocket socket,
                                               boolean closeSocketOnClose) {
                return new NativeRouterReceiveSupport(socket,
                    closeSocketOnClose);
            }

            @Override
            public Received routerRecv(Object support, RecvFlags flags) {
                return ((NativeRouterReceiveSupport) support).recv(flags);
            }

            @Override
            public boolean routerRecvInto(Object support, Received target,
                                          RecvFlags flags) {
                return ((NativeRouterReceiveSupport) support).recvInto(target,
                    flags);
            }

            @Override
            public void routerOnReceive(Object support,
                                        SocketMessageHandler handler) {
                ((NativeRouterReceiveSupport) support).onReceive(handler);
            }

            @Override
            public void routerReceiveBeginClose(Object support) {
                ((NativeRouterReceiveSupport) support).beginClose();
            }

            @Override
            public void routerReceiveFinishClose(Object support) {
                ((NativeRouterReceiveSupport) support).finishClose();
            }

            @Override
            public CompletableFuture<List<Message>> routerRequestAsync(
                    RouterSocket socket, RoutingId routingId,
                    List<Message> parts, SendFlags flags, Duration timeout) {
                return NativeRouterRequestSupport.requestStage(socket,
                    routingId, parts, flags, timeout);
            }

            @Override
            public boolean routerRequestCallback(
                    RouterSocket socket, RoutingId routingId,
                    List<Message> parts, RequestCallback callback,
                    SendFlags flags, Duration timeout) {
                return NativeRouterRequestSupport.requestCallback(socket,
                    routingId, parts, callback, flags, timeout);
            }

            @Override
            public void routerReply(RouterSocket socket, RoutingId routingId,
                                    long requestSequence, List<Message> parts) {
                NativeRouterRequestSupport.reply(socket, routingId,
                    requestSequence, parts);
            }

        });
    }

    private NativeDealerRequestSupport() {
    }

    public static CompletableFuture<List<Message>> requestStage(
            DealerSocket socket,
            List<Message> parts,
            SendFlags flags,
            Duration timeout) {
        return request(socket, parts, timeout, flags)
            .thenApply(RequestReplySupport::takeReceivedParts);
    }

    public static boolean requestCallback(DealerSocket socket,
                                          List<Message> parts,
                                          RequestCallback callback,
                                          SendFlags flags,
                                          Duration timeout) {
        Objects.requireNonNull(callback, "callback");
        Objects.requireNonNull(socket, "socket");
        Objects.requireNonNull(parts, "parts");
        long timeoutMs = RequestReplySupport.timeoutMillis(timeout);
        return RequestSubmitLoop.submitCallback(timeoutMs,
            InternalAccess.socketHandle(socket),
            "zlink-dealer-request-progress", flags, callback::onComplete,
            (handler, userData) -> submitRequest(socket, parts, timeoutMs,
                flags, handler, userData));
    }

    private static CompletableFuture<Received> request(DealerSocket socket,
                                                       List<Message> parts,
                                                       Duration timeout,
                                                       SendFlags flags) {
        Objects.requireNonNull(socket, "socket");
        Objects.requireNonNull(parts, "parts");
        long timeoutMs = RequestReplySupport.timeoutMillis(timeout);
        return RequestSubmitLoop.submitFuture(timeoutMs,
            InternalAccess.socketHandle(socket),
            "zlink-dealer-request-progress",
            (handler, userData) -> submitRequest(socket, parts, timeoutMs,
                flags, handler, userData));
    }

    private static void submitRequest(DealerSocket socket,
                                      List<Message> payload,
                                      long timeoutMs,
                                      SendFlags flags,
                                      MemorySegment handler,
                                      MemorySegment userData) {
        int nativeFlags = flags == null ? 0 : flags.value();
        int timeout = RequestReplySupport.toTimeoutInt(timeoutMs);
        RequestSubmitLoop.submitResultParts(payload,
            (part, partFlag) -> dealerRequestPartOnce(socket, part,
                nativeFlags, partFlag, timeout, handler, userData));
    }

    private static int dealerRequestPartOnce(DealerSocket socket,
                                             Message part,
                                             int flags,
                                             int partFlag,
                                             int timeoutMs,
                                             MemorySegment handler,
                                             MemorySegment userData) {
        try (Arena arena = Arena.ofConfined()) {
            MemorySegment nativeMsg = arena.allocate(NativeLayouts.MESSAGE_LAYOUT);
            InternalAccess.messageCopyTo(part, nativeMsg);
            return Native.dealerRequestPart(
                InternalAccess.socketHandle(socket), nativeMsg, flags,
                partFlag, timeoutMs, handler, userData);
        }
    }
}
