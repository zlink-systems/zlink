/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.runtime.sockets;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.errors.ZlinkSubmitException;
import systems.zlink.contracts.errors.ZlinkException;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.contracts.sockets.RouterSocket;
import systems.zlink.runtime.nativeapi.InternalAccess;
import systems.zlink.runtime.nativeapi.Native;
import systems.zlink.runtime.nativeapi.NativeRequestBridge;
import systems.zlink.runtime.nativeapi.NativeRoutingIds;
import systems.zlink.runtime.nativeapi.SendScratch;
import systems.zlink.runtime.nativeapi.NativeSubmitErrors;
import java.lang.foreign.MemorySegment;
import java.util.List;
import java.util.Objects;

final class NativeRouterRequestSupport {
    private static final ThreadLocal<SendScratch> REPLY_SCRATCH =
        ThreadLocal.withInitial(SendScratch::new);

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

    static void reply(RouterSocket socket,
                      byte[] routingIdBytes,
                      long requestSequence,
                      List<Message> parts) {
        Objects.requireNonNull(socket, "socket");
        Objects.requireNonNull(routingIdBytes, "routingIdBytes");
        Objects.requireNonNull(parts, "parts");
        if (parts.size() == 2 && NativeRequestBridge.replyTwoAvailable()) {
            SendScratch scratch = REPLY_SCRATCH.get();
            MemorySegment nativeRid = scratch.nativeRoutingId;
            if (scratch.lastRoutingIdBytes != routingIdBytes) {
                NativeRoutingIds.writeBytes(nativeRid, routingIdBytes);
                scratch.lastRoutingIdBytes = routingIdBytes;
                scratch.lastRoutingId = null;
            }
            submitTwo(socket, nativeRid, requestSequence, parts);
            return;
        }
        RequestSubmitLoop.submitErrnoParts(parts,
            (part, partFlag) -> routerReplyPartOnce(socket, routingIdBytes,
                requestSequence, part, partFlag),
            () -> submitFailure("zlink_router_reply_part"));
    }

    private static void submitTwo(RouterSocket socket,
                                  MemorySegment nativeRid,
                                  long requestSequence,
                                  List<Message> parts) {
        Message first = parts.get(0);
        Message second = parts.get(1);
        long outcome = NativeRequestBridge.replyTwo(
            InternalAccess.socketHandle(socket), nativeRid, requestSequence,
            InternalAccess.messageNativeHandle(first),
            InternalAccess.messageNativeHandle(second));
        int consumed = (int) (outcome >>> 32);
        int result = (int) outcome;
        if (consumed > 0) {
            InternalAccess.messageMarkTransferred(first);
        }
        if (consumed > 1) {
            InternalAccess.messageMarkTransferred(second);
        }
        if (result != 0) {
            throw submitFailure("zlink_java_router_reply_two");
        }
    }

    private static int routerReplyPartOnce(RouterSocket socket,
                                           RoutingId routingId,
                                           long requestSequence,
                                           Message part,
                                           int partFlag) {
        SendScratch scratch = REPLY_SCRATCH.get();
        MemorySegment nativeRid = scratch.nativeRoutingId;
        if (scratch.lastRoutingId != routingId) {
            NativeRoutingIds.write(nativeRid, routingId);
            scratch.lastRoutingId = routingId;
            scratch.lastRoutingIdBytes = null;
        }
        MemorySegment nativeMsg = scratch.nativeMsg;
        // HOT PATH: replies retain the existing move-and-restore ownership
        // boundary, but reuse native routing and message slots.
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

    private static int routerReplyPartOnce(RouterSocket socket,
                                           byte[] routingIdBytes,
                                           long requestSequence,
                                           Message part,
                                           int partFlag) {
        SendScratch scratch = REPLY_SCRATCH.get();
        MemorySegment nativeRid = scratch.nativeRoutingId;
        if (scratch.lastRoutingIdBytes != routingIdBytes) {
            NativeRoutingIds.writeBytes(nativeRid, routingIdBytes);
            scratch.lastRoutingIdBytes = routingIdBytes;
            scratch.lastRoutingId = null;
        }
        MemorySegment nativeMsg = scratch.nativeMsg;
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
