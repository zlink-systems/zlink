package systems.zlink.samples.zoneworld.server.ops;

import java.util.List;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.CopyOnWriteArrayList;
import systems.zlink.framework.streams.ZLinkSessionContext;
import systems.zlink.samples.zoneworld.shared.Messages;

public final class OpsConsoleRegistry {
    private final List<ZLinkSessionContext> sessions = new CopyOnWriteArrayList<>();
    private final List<Messages.NodeAlertNotify> alerts = new CopyOnWriteArrayList<>();

    public void add(ZLinkSessionContext context) { sessions.add(context); }
    public void remove(ZLinkSessionContext context) { sessions.remove(context); }
    public void record(Messages.NodeAlertNotify alert) { alerts.add(alert); }

    public void broadcast(Object message) {
        for (ZLinkSessionContext session : List.copyOf(sessions)) {
            session.client().send(message).submit().exceptionally(error -> null);
        }
    }

    public CompletionStage<Void> replay(
        ZLinkSessionContext context,
        List<Messages.NodeView> nodes) {
        CompletionStage<Void> sends = CompletableFuture.completedFuture(null);
        for (Messages.NodeView node : List.copyOf(nodes)) {
            Messages.NodeStatusNotify status = new Messages.NodeStatusNotify(
                node.nodeId(), node.registered(), node.connected(), node.maintenance(),
                node.zones(), node.playerCount());
            sends = sends.thenCompose(ignored -> broadcastTo(context, status));
        }
        for (Messages.NodeAlertNotify alert : List.copyOf(alerts)) {
            sends = sends.thenCompose(ignored -> broadcastTo(context, alert));
        }
        return sends;
    }

    private static CompletionStage<Void> broadcastTo(
        ZLinkSessionContext context,
        Object message) {
        return context.client().send(message).submit();
    }
}
