package systems.zlink.samples.zoneworld.server.configuration;

import java.time.Duration;
import java.util.Comparator;
import java.util.List;
import java.util.Map;
import java.util.concurrent.ConcurrentHashMap;
import java.util.function.Consumer;
import systems.zlink.samples.zoneworld.shared.Messages;
import systems.zlink.samples.zoneworld.shared.ZoneWorldSpec;

/** Combines explicit node reports with independently observed mesh connectivity. */
public final class NodeRegistry {
    private static final long REPORT_TTL_NANOS =
        Duration.ofMillis(ZoneWorldSpec.NODE_STATUS_REPORT_TTL_MS).toNanos();

    private final Map<String, State> nodes = new ConcurrentHashMap<>();
    private final Map<String, String> nodeByRoutingId = new ConcurrentHashMap<>();
    private final Map<String, String> routingIdByNode = new ConcurrentHashMap<>();
    private volatile Map<String, Boolean> liveRoutingIds = Map.of();
    private volatile Consumer<Messages.NodeView> changed = ignored -> {};
    private volatile Consumer<Messages.NodeAlertNotify> alerted = ignored -> {};

    public void onChanged(Consumer<Messages.NodeView> handler) { changed = handler; }
    public void onAlert(Consumer<Messages.NodeAlertNotify> handler) { alerted = handler; }

    public synchronized void applyLiveRoutingIds(Map<String, Boolean> observed) {
        liveRoutingIds = Map.copyOf(observed);
        for (Map.Entry<String, State> entry : List.copyOf(nodes.entrySet())) {
            String rid = routingIdByNode.get(entry.getKey());
            boolean connected = rid != null && observed.containsKey(rid);
            updateConnection(entry.getKey(), connected);
        }
    }

    public synchronized void report(Messages.ReportNodeStatusMsg report, String routingId) {
        long now = System.nanoTime();
        String previousRid = routingIdByNode.put(report.nodeId(), routingId);
        if (previousRid != null && !previousRid.equals(routingId)) nodeByRoutingId.remove(previousRid);
        nodeByRoutingId.put(routingId, report.nodeId());
        boolean connected = liveRoutingIds.containsKey(routingId);
        Messages.NodeView view = new Messages.NodeView(
            report.nodeId(), true, connected, report.maintenance(),
            List.copyOf(report.zones()), report.playerCount());
        nodes.put(report.nodeId(), new State(view, now));
        changed.accept(view);
        System.out.println("node status observed. node=" + report.nodeId()
            + ", rid=" + routingId + ", registered=true, connected=" + connected);
    }

    public synchronized void expireStaleReports() {
        long now = System.nanoTime();
        for (Map.Entry<String, State> entry : List.copyOf(nodes.entrySet())) {
            State state = entry.getValue();
            if (!state.view().registered() || now - state.lastReportNanos() < REPORT_TTL_NANOS) continue;
            Messages.NodeView expired = copy(state.view(), false, state.view().connected());
            nodes.put(entry.getKey(), new State(expired, state.lastReportNanos()));
            changed.accept(expired);
        }
    }

    public void alert(Messages.ReportSpotEventMsg report) {
        Messages.NodeAlertNotify alert = new Messages.NodeAlertNotify(
            report.nodeId(), report.kind(), report.detail(), report.occurredAt());
        alerted.accept(alert);
        System.out.println("node alert node=" + report.nodeId()
            + " kind=" + report.kind() + " detail=" + report.detail());
    }

    public String nodeIdOf(String routingId) { return nodeByRoutingId.get(routingId); }
    public String routingIdOf(String nodeId) { return routingIdByNode.get(nodeId); }
    public Messages.NodeView find(String nodeId) {
        State state = nodes.get(nodeId);
        return state == null ? null : state.view();
    }

    public List<Messages.NodeView> snapshot() {
        expireStaleReports();
        return nodes.values().stream().map(State::view)
            .sorted(Comparator.comparing(Messages.NodeView::nodeId)).toList();
    }

    private void updateConnection(String nodeId, boolean connected) {
        State current = nodes.get(nodeId);
        if (current == null || current.view().connected() == connected) return;
        Messages.NodeView updated = copy(current.view(), current.view().registered(), connected);
        nodes.put(nodeId, new State(updated, current.lastReportNanos()));
        changed.accept(updated);
        System.out.println("node connection observed. node=" + nodeId + ", connected=" + connected);
    }

    private static Messages.NodeView copy(Messages.NodeView value, boolean registered, boolean connected) {
        return new Messages.NodeView(value.nodeId(), registered, connected, value.maintenance(),
            value.zones(), value.playerCount());
    }

    private record State(Messages.NodeView view, long lastReportNanos) {}
}
