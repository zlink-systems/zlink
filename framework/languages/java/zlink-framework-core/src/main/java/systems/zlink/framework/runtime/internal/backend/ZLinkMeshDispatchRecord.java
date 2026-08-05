package systems.zlink.framework.runtime.internal.backend;

import java.util.List;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.framework.runtime.internal.binding.spot.ReadyRecord;
import systems.zlink.framework.runtime.internal.binding.spot.ReceiveRecord;
import systems.zlink.framework.runtime.internal.dispatch.ZLinkInboundDispatchBudget;

/**
 * One retained RouteMesh dispatch message.
 *
 * <p>The receiver owns the retained message parts and must close this record
 * after dispatch completes.
 */
public record ZLinkMeshDispatchRecord(
    ReadyRecord owner,
    ReceiveRecord receive,
    List<Message> parts,
    java.util.function.Consumer<List<Message>> frameworkReply,
    ZLinkInboundDispatchBudget.Lease inboundDispatchLease) implements AutoCloseable {
    public ZLinkMeshDispatchRecord {
        parts = List.copyOf(parts);
    }

    public ZLinkMeshDispatchRecord(
        ReadyRecord owner,
        ReceiveRecord receive,
        List<Message> parts) {
        this(owner, receive, parts, null, null);
    }

    public ZLinkMeshDispatchRecord(
        ReadyRecord owner,
        ReceiveRecord receive,
        List<Message> parts,
        java.util.function.Consumer<List<Message>> frameworkReply) {
        this(owner, receive, parts, frameworkReply, null);
    }

    public boolean canReply() {
        return frameworkReply != null;
    }

    public void reply(List<Message> replyParts) {
        if (frameworkReply == null) {
            throw new IllegalStateException("dispatch record has no reply route");
        }
        frameworkReply.accept(List.copyOf(replyParts));
    }

    /** Releases retained message parts while transferring the admission lease. */
    public void closeParts() {
        parts.forEach(Message::close);
    }

    @Override
    public void close() {
        try {
            closeParts();
        } finally {
            if (inboundDispatchLease != null) {
                inboundDispatchLease.close();
            }
        }
    }
}
