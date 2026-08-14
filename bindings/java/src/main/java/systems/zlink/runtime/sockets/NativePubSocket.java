/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.runtime.sockets;

import systems.zlink.contracts.sockets.*;
import systems.zlink.internal.ContractAccess;

import systems.zlink.contracts.core.Context;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.contracts.messaging.AsyncSendOperation;
import systems.zlink.contracts.messaging.SendOperation;
import systems.zlink.contracts.sockets.SendFlags;
import systems.zlink.internal.sockets.SocketOptions;
import systems.zlink.runtime.messaging.MessageOperations;
import java.util.List;

final class NativePubSocket extends NativeSocketBase implements PubSocket {
    private final PubSocketOptions options = ContractAccess.pubSocketOptions(this);
    private final OutboundRecordAttemptGate outboundRecordAttempts =
        new OutboundRecordAttemptGate();
    private final PublisherAdmission publisherAdmission;
    private TopicSendInvoker cachedTopicInvoker;

    NativePubSocket(Context ctx) {
        super(ctx, SocketType.PUB);
        publisherAdmission = new PublisherAdmission(
            runtime(), outboundRecordAttempts);
    }

    public void bind(String endpoint) { runtime().bind(endpoint); }
    public void connect(String endpoint) { runtime().connect(endpoint); }
    public void unbind(String endpoint) { runtime().unbind(endpoint); }
    public void disconnect(String endpoint) { runtime().disconnect(endpoint); }
    public void disconnectRid(RoutingId routingId) {
        runtime().disconnectRid(routingId);
    }
    public void setRoutingId(RoutingId rid) { runtime().setRoutingId(rid); }

    public SendOperation publish(String topicId) {
        TopicSendInvoker invoker = cachedTopicInvoker;
        if (invoker == null || !invoker.matches(topicId)) {
            invoker = new TopicSendInvoker(topicId);
            cachedTopicInvoker = invoker;
        }
        return MessageOperations.send(invoker, invoker);
    }

    public AsyncSendOperation publishAsync(String topicId) {
        return MessageOperations.asyncSend(parts -> publisherAdmission.publish(
            topicId, parts, runtime().getOption(SocketOptions.SNDTIMEO)));
    }

    /** Binds one public publish operation to its topic without two lambdas. */
    private final class TopicSendInvoker
      implements MessageOperations.SingleSendInvoker, MessageOperations.SendInvoker {
        private final String topicId;

        private TopicSendInvoker(String topicId) {
            this.topicId = topicId;
        }

        private boolean matches(String value) {
            return topicId == value || (topicId != null && topicId.equals(value));
        }

        @Override
        public boolean submit(Message part, SendFlags flags) {
            return outboundRecordAttempts.call(() -> runtime().publish(
                topicId, part, SendFlag.fromValue(flags.value())));
        }

        @Override
        public boolean submit(List<Message> parts, SendFlags flags) {
            return outboundRecordAttempts.call(() -> runtime().publish(
                topicId, parts, SendFlag.fromValue(flags.value())));
        }
    }
    public void setSendReadyHandler(SendReadyHandler handler) {
        publisherAdmission.setObserver(handler);
    }

    @Override
    public void close() {
        publisherAdmission.prepareClose();
        boolean closed = false;
        try {
            outboundRecordAttempts.run(runtime()::close);
            closed = true;
            publisherAdmission.commitClose();
        } finally {
            if (closed)
                publisherAdmission.finishClose();
            else
                publisherAdmission.abortClose();
        }
    }

    @Override public PubSocketOptions options() { return options; }
}
