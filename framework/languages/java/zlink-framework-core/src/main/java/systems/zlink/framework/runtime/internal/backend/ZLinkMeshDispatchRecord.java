package systems.zlink.framework.runtime.internal.backend;
import java.util.function.Consumer;
import java.util.concurrent.atomic.AtomicBoolean;

import java.util.List;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.framework.runtime.internal.binding.spot.ReadyRecord;
import systems.zlink.framework.runtime.internal.binding.spot.ReceiveRecord;

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
    Consumer<List<Message>> frameworkReply,
    Runnable terminalRelease) implements AutoCloseable {
    public ZLinkMeshDispatchRecord {
        parts = List.copyOf(parts);
        terminalRelease = once(terminalRelease);
    }

    public ZLinkMeshDispatchRecord(
        ReadyRecord owner,
        ReceiveRecord receive,
        List<Message> parts) {
        this(owner, receive, parts, null, () -> { });
    }

    public ZLinkMeshDispatchRecord(
        ReadyRecord owner,
        ReceiveRecord receive,
        List<Message> parts,
        Consumer<List<Message>> frameworkReply) {
        this(owner, receive, parts, frameworkReply, () -> { });
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

    /** Releases retained message parts. */
    public void closeParts() {
        parts.forEach(Message::close);
    }

    @Override
    public void close() {
        try {
            closeParts();
        } finally {
            terminalRelease.run();
        }
    }

    private static Runnable once(Runnable release) {
        java.util.Objects.requireNonNull(release, "terminalRelease");
        AtomicBoolean released = new AtomicBoolean();
        return () -> {
            if (released.compareAndSet(false, true)) {
                release.run();
            }
        };
    }
}
