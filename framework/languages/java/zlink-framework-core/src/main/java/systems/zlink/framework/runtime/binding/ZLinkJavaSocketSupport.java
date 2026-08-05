package systems.zlink.framework.runtime.binding;

import java.time.Duration;
import java.util.List;
import java.util.Optional;
import systems.zlink.contracts.errors.ZlinkRecvException;
import systems.zlink.contracts.errors.ZlinkSubmitException;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.contracts.messaging.Received;
import systems.zlink.contracts.messaging.ReplyOperation;
import systems.zlink.contracts.messaging.RequestOperation;
import systems.zlink.contracts.messaging.SendOperation;
import systems.zlink.contracts.sockets.RecvFlags;
import systems.zlink.contracts.sockets.RecvResult;
import systems.zlink.contracts.sockets.SendFlags;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendReceived;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendRecvMode;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendRequestCallback;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendRequestResult;

final class ZLinkJavaSocketSupport {
    private ZLinkJavaSocketSupport() {
    }

    static void validateChannelName(String channelName) {
        if (channelName == null || channelName.isBlank()) {
            throw new IllegalArgumentException("channelName must not be blank");
        }
    }

    static boolean recvOrNoData(java.util.function.BooleanSupplier receive) {
        try {
            return receive.getAsBoolean();
        } catch (ZlinkRecvException ex) {
            if (ex.getResult() == RecvResult.NO_DATA
                || ex.getResult() == RecvResult.BUSY
                || ex.getResult() == RecvResult.INTERNAL_ERROR) {
                return false;
            }
            throw ex;
        }
    }

    static RecvFlags map(ZLinkBackendRecvMode mode) {
        return mode == ZLinkBackendRecvMode.DONT_WAIT ? RecvFlags.DONT_WAIT : RecvFlags.NONE;
    }

    static boolean submit(SendOperation operation, List<Message> parts, SendFlags flags) {
        var submit = operation.message(parts.get(0));
        for (int i = 1; i < parts.size(); i++) {
            submit.message(parts.get(i));
        }
        return submit.flags(flags).submit();
    }

    static void submitReply(ReplyOperation operation, List<Message> parts) {
        var submit = operation.message(parts.get(0));
        for (int i = 1; i < parts.size(); i++) {
            submit.message(parts.get(i));
        }
        submit.submit();
    }

    static boolean submitRequest(
        RequestOperation operation,
        List<Message> parts,
        ZLinkBackendRequestCallback callback,
        SendFlags flags,
        Duration timeout) {
        var submit = operation.message(parts.get(0)).timeout(timeout).flags(flags);
        for (int i = 1; i < parts.size(); i++) {
            submit.message(parts.get(i));
        }
        try {
            return submit.submit((result, replyParts) -> callback.handle(new ZLinkBackendReceived(
                ZLinkBackendRequestResult.valueOf(result.name()),
                Optional.empty(),
                Optional.empty(),
                Optional.empty(),
                replyParts.stream().map(Message::from).toList())));
        } catch (ZlinkSubmitException ex) {
            // Preserve the typed submission result so Framework control-plane
            // callers can distinguish an expected admission race from a bug.
            throw ex;
        }
    }

    static ZLinkBackendReceived fromReceived(Received received) {
        return new ZLinkBackendReceived(
            received.getRoutingId(),
            Optional.empty(),
            received.requestSeq(),
            received.parts().stream().map(Message::from).toList(),
            replyParts -> submitReply(received.reply(), replyParts),
            received::close);
    }
}
