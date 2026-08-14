/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.runtime.sockets;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.errors.ZlinkSubmitException;
import systems.zlink.contracts.errors.ZlinkException;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.contracts.sockets.RouterSocket;
import systems.zlink.runtime.nativeapi.InternalAccess;
import systems.zlink.runtime.nativeapi.Native;
import systems.zlink.runtime.nativeapi.NativeLayouts;
import systems.zlink.runtime.nativeapi.NativeRoutingIds;
import systems.zlink.runtime.nativeapi.NativeSubmitErrors;
import java.lang.foreign.Arena;
import java.lang.foreign.MemorySegment;
import java.util.List;
import java.util.Objects;

final class NativeRouterRequestSupport {
    private NativeRouterRequestSupport() {
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
