/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.runtime.sockets;

import systems.zlink.contracts.sockets.*;
import systems.zlink.internal.ContractAccess;

import systems.zlink.contracts.core.Context;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.contracts.messaging.PublishOperation;
import systems.zlink.contracts.messaging.SubscriptionEvent;
import systems.zlink.contracts.errors.ZlinkSubmitException;
import systems.zlink.contracts.sockets.SendFlags;
import systems.zlink.runtime.messaging.MessageOperations;
import systems.zlink.runtime.nativeapi.Native;
import java.util.List;
final class NativeXPubSocket extends NativeSocketBase implements XPubSocket {
    private final PubSocketOptions options = ContractAccess.pubSocketOptions(this);
    private TopicSendInvoker cachedTopicInvoker;

    NativeXPubSocket(Context ctx) {
        super(ctx, SocketType.XPUB);
    }

    public void bind(String endpoint) { runtime().bind(endpoint); }
    public void connect(String endpoint) { runtime().connect(endpoint); }
    public void unbind(String endpoint) { runtime().unbind(endpoint); }
    public void disconnect(String endpoint) { runtime().disconnect(endpoint); }
    public void disconnectRid(RoutingId routingId) {
        runtime().disconnectRid(routingId);
    }
    public void setRoutingId(RoutingId rid) { runtime().setRoutingId(rid); }

    public PublishOperation publish(String topicId) {
        TopicSendInvoker invoker = cachedTopicInvoker;
        if (invoker == null || !invoker.matches(topicId)) {
            invoker = new TopicSendInvoker(topicId);
            cachedTopicInvoker = invoker;
        }
        return MessageOperations.publish(invoker);
    }
    private final class TopicSendInvoker
      implements MessageOperations.PublishInvoker {
        private final String topicId;

        private TopicSendInvoker(String topicId) {
            this.topicId = topicId;
        }

        private boolean matches(String value) {
            return topicId == value
                || (topicId != null && topicId.equals(value));
        }

        @Override
        public void submit(List<Message> parts, SendFlags flags) {
            SendResult result = runtime().publishNoWaitResult(topicId, parts);
            if (result == SendResult.SENT)
                return;
            int errno = Native.errno();
            throw result == SendResult.BACKPRESSURED
                ? new ZlinkSubmitException(SubmitResult.BACKPRESSURED, errno)
                : new ZlinkSubmitException(SubmitResult.NOT_CONNECTED, errno);
        }
    }
    public boolean receiveSubscriptionEvent(SubscriptionEvent result, RecvFlags flags) { return runtime().receiveSubscriptionEvent(result, ReceiveFlag.fromValue(flags.value())); }
    @Override
    public void close() {
        runtime().close();
    }
    @Override public PubSocketOptions options() { return options; }
}
