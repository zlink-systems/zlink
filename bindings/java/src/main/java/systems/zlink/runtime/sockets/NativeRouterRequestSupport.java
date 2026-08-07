/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.runtime.sockets;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.errors.ZlinkSubmitException;
import systems.zlink.contracts.errors.ZlinkException;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.contracts.messaging.Received;
import systems.zlink.contracts.sockets.RequestCallback;
import systems.zlink.contracts.sockets.RouterSocket;
import systems.zlink.contracts.sockets.SendFlags;
import systems.zlink.contracts.sockets.SubmitResult;
import systems.zlink.runtime.nativeapi.InternalAccess;
import systems.zlink.runtime.nativeapi.Native;
import systems.zlink.runtime.nativeapi.NativeErrno;
import systems.zlink.runtime.nativeapi.NativeLayouts;
import systems.zlink.runtime.nativeapi.NativeRoutingIds;
import systems.zlink.runtime.nativeapi.NativeSubmitErrors;
import systems.zlink.runtime.nativeapi.RequestReplySupport;
import systems.zlink.runtime.nativeapi.RoutedRequestSupport;
import java.lang.foreign.Arena;
import java.lang.foreign.MemorySegment;
import java.lang.foreign.ValueLayout;
import java.time.Duration;
import java.util.List;
import java.util.Objects;
import java.util.concurrent.CompletableFuture;

final class NativeRouterRequestSupport {

    private NativeRouterRequestSupport() {
    }

    public static CompletableFuture<List<Message>> requestStage(
            RouterSocket socket,
            RoutingId routingId,
            List<Message> parts,
            SendFlags flags,
            Duration timeout) {
        return request(socket, routingId, 0L, 0L, parts, flags, timeout)
            .thenApply(RequestReplySupport::takeReceivedParts);
    }

    public static CompletableFuture<List<Message>> requestStage(
            RouterSocket socket,
            RoutingId routingId,
            long transportPairId,
            long transportPairGeneration,
            List<Message> parts,
            SendFlags flags,
            Duration timeout) {
        return request(socket, routingId, transportPairId,
                       transportPairGeneration, parts, flags, timeout)
            .thenApply(RequestReplySupport::takeReceivedParts);
    }

    public static boolean requestCallback(RouterSocket socket,
                                          RoutingId routingId,
                                          List<Message> parts,
                                          RequestCallback callback,
                                          SendFlags flags,
                                          Duration timeout) {
        Objects.requireNonNull(callback, "callback");
        Objects.requireNonNull(socket, "socket");
        Objects.requireNonNull(routingId, "routingId");
        Objects.requireNonNull(parts, "parts");
        long timeoutMs = RequestReplySupport.timeoutMillis(timeout);
        return RequestSubmitLoop.submitCallback(timeoutMs,
            InternalAccess.socketHandle(socket),
            "zlink-router-request-progress", flags, callback::onComplete,
            (handler, userData) -> submitRequest(socket, routingId, parts,
                0L, 0L, timeoutMs, flags, handler, userData));
    }

    public static boolean requestCallback(RouterSocket socket,
                                          RoutingId routingId,
                                          long transportPairId,
                                          long transportPairGeneration,
                                          List<Message> parts,
                                          RequestCallback callback,
                                          SendFlags flags,
                                          Duration timeout) {
        Objects.requireNonNull(callback, "callback");
        Objects.requireNonNull(socket, "socket");
        Objects.requireNonNull(routingId, "routingId");
        Objects.requireNonNull(parts, "parts");
        long timeoutMs = RequestReplySupport.timeoutMillis(timeout);
        return RequestSubmitLoop.submitCallback(timeoutMs,
            InternalAccess.socketHandle(socket),
            "zlink-router-request-progress", flags, callback::onComplete,
            (handler, userData) -> submitRequest(socket, routingId, parts,
                transportPairId, transportPairGeneration, timeoutMs, flags,
                handler, userData));
    }

    public static void reply(RouterSocket socket,
                             RoutingId routingId,
                             long requestSequence,
                             List<Message> parts) {
        Objects.requireNonNull(socket, "socket");
        Objects.requireNonNull(routingId, "routingId");
        Objects.requireNonNull(parts, "parts");
        RequestSubmitLoop.submitErrnoParts(parts,
            (part, partFlag) -> routerReplyPartOnce(socket, routingId,
                requestSequence, part, partFlag),
            () -> submitFailure("zlink_router_reply_part"));
    }

    private static CompletableFuture<Received> request(
            RouterSocket socket,
            RoutingId routingId,
            long transportPairId,
            long transportPairGeneration,
            List<Message> parts,
            SendFlags flags,
            Duration timeout) {
        Objects.requireNonNull(socket, "socket");
        Objects.requireNonNull(routingId, "routingId");
        Objects.requireNonNull(parts, "parts");
        long timeoutMs = RequestReplySupport.timeoutMillis(timeout);
        return RequestSubmitLoop.submitFuture(timeoutMs,
            InternalAccess.socketHandle(socket),
            "zlink-router-request-progress",
            (handler, userData) -> submitRequest(socket, routingId, parts,
                transportPairId, transportPairGeneration, timeoutMs, flags,
                handler, userData));
    }

    private static void submitRequest(RouterSocket socket,
                                      RoutingId routingId,
                                      List<Message> payload,
                                      long transportPairId,
                                      long transportPairGeneration,
                                      long timeoutMs,
                                      SendFlags flags,
                                      MemorySegment handler,
                                      MemorySegment userData) {
        int nativeFlags = flags == null ? 0 : flags.value();
        int timeout = RequestReplySupport.toTimeoutInt(timeoutMs);
        RequestSubmitLoop.submitResultParts(payload,
            (part, partFlag) -> routerRequestPartOnce(socket, routingId, part,
                transportPairId, transportPairGeneration, nativeFlags,
                partFlag, timeout, handler, userData));
    }

    private static int routerRequestPartOnce(RouterSocket socket,
                                             RoutingId routingId,
                                             Message part,
                                             long transportPairId,
                                             long transportPairGeneration,
                                             int flags,
                                             int partFlag,
                                             int timeoutMs,
                                             MemorySegment handler,
                                             MemorySegment userData) {
        try (Arena arena = Arena.ofConfined()) {
            MemorySegment nativeRid = nativeRoutingId(arena, routingId);
            MemorySegment nativeMsg = arena.allocate(NativeLayouts.MESSAGE_LAYOUT);
            InternalAccess.messageCopyTo(part, nativeMsg);
            if (transportPairId != 0 && transportPairGeneration != 0) {
                return Native.routerRequestTransportPairPart(
                    InternalAccess.socketHandle(socket), nativeRid,
                    transportPairId, transportPairGeneration, nativeMsg,
                    flags, partFlag, timeoutMs, handler, userData);
            }
            return Native.routerRequestPart(
                InternalAccess.socketHandle(socket), nativeRid, nativeMsg,
                flags, partFlag, timeoutMs, handler, userData);
        }
    }

    private static int routerReplyPartOnce(RouterSocket socket,
                                           RoutingId routingId,
                                           long requestSequence,
                                           Message part,
                                           int partFlag) {
        try (Arena arena = Arena.ofConfined()) {
            MemorySegment nativeRid = nativeRoutingId(arena, routingId);
            MemorySegment nativeMsg = arena.allocate(NativeLayouts.MESSAGE_LAYOUT);
            InternalAccess.messageTransferTo(part, nativeMsg);
            try {
                int rc = Native.routerReplyPart(
                    InternalAccess.socketHandle(socket), nativeRid,
                    requestSequence, nativeMsg, partFlag);
                if (rc != 0) {
                    InternalAccess.messageRestoreFromNative(part, nativeMsg,
                        false, null);
                }
                return rc;
            } catch (RuntimeException ex) {
                InternalAccess.messageRestoreFromNative(part, nativeMsg, false,
                    null);
                throw ex;
            }
        }
    }

    private static MemorySegment nativeRoutingId(Arena arena,
                                                 RoutingId routingId) {
        return NativeRoutingIds.allocate(arena, routingId);
    }

    private static ZlinkSubmitException submitFailure(String apiName) {
        int errno = Native.errno();
        ZlinkSubmitException submit =
            NativeSubmitErrors.submitExceptionOrNull(errno);
        if (submit != null) {
            return submit;
        }
        throw ZlinkException.fromLastError(
            systems.zlink.contracts.errors.ErrorCategory.SUBMIT);
    }
}
