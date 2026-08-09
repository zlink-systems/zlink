package systems.zlink.samples.zoneworld.server.configuration;

import java.util.ArrayList;
import java.util.Comparator;
import java.util.List;
import java.util.Map;
import java.util.concurrent.ConcurrentHashMap;
import systems.zlink.samples.zoneworld.shared.Messages;
import systems.zlink.samples.zoneworld.shared.ZoneWorldSpec;
/**
 * What the Ops console knows about each node. Two sources feed it and neither is enough
 * alone: the mesh runtime says which nodes are present right now, and a node's own report
 * says which zones it holds, how many players are on it and whether it is closed.
 *
 * <p>A node that has stopped is not there to answer a request, so registration and
 * connection are never polled - they follow the runtime peer observation. Everything the
 * console learned from the node itself is dropped when the node leaves, so a value read
 * back after a restart can only have come from the restarted process.</p>
 */
public final class NodeRegistry {
    private final Map<String, Messages.NodeView> nodes = new ConcurrentHashMap<>();
    private final Map<String, String> routingIds = new ConcurrentHashMap<>();
    private volatile Map<String, String> live = Map.of();

    public synchronized void applyLivePeers(Map<String, String> observed) {
        live = Map.copyOf(observed);
        for (Map.Entry<String, String> entry : observed.entrySet()) {
            apply(entry.getKey(), entry.getValue(), true);
        }
        for (String nodeId : List.copyOf(nodes.keySet())) {
            if (observed.containsKey(nodeId)) continue;
            apply(nodeId, routingIds.get(nodeId), false);
        }
    }

    public synchronized void report(Messages.ReportNodeStatusMsg report) {
        boolean present = live.containsKey(report.nodeId());
        nodes.put(report.nodeId(), new Messages.NodeView(
            report.nodeId(), present, present,
            report.maintenance(), report.zones(), report.playerCount()));
        System.out.println("report node=" + report.nodeId()
            + " zones=" + report.zones() + " players=" + report.playerCount());
    }

    public void alert(Messages.ReportSpotEventMsg report) {
        System.out.println("node alert node=" + report.nodeId()
            + " kind=" + report.kind() + " detail=" + report.detail());
    }

    public Messages.NodeView find(String nodeId) {
        return nodes.get(nodeId);
    }

    public List<Messages.NodeView> snapshot() {
        return nodes.values().stream()
            .sorted(Comparator.comparing(Messages.NodeView::nodeId))
            .map(value -> new Messages.NodeView(
                value.nodeId(), value.registered(), value.connected(), value.maintenance(),
                ListCopy.copy(value.zones()), value.playerCount()))
            .toList();
    }

    private void apply(String nodeId, String routingId, boolean present) {
        Messages.NodeView current = nodes.get(nodeId);
        if (present && routingId != null) {
            routingIds.put(nodeId, routingId);
        }
        // A node the console can no longer see tells it nothing about its own runtime
        // state, so the reported fields are dropped rather than kept as a stale row.
        Messages.NodeView updated = present
            ? new Messages.NodeView(
                nodeId, true, true,
                current != null && current.maintenance(),
                current != null ? current.zones() : ZoneWorldSpec.zonesOf(nodeId),
                current != null ? current.playerCount() : 0)
            : new Messages.NodeView(
                nodeId, false, false, false, ZoneWorldSpec.zonesOf(nodeId), 0);
        nodes.put(nodeId, updated);
        if (current != null
            && current.registered() == updated.registered()
            && current.connected() == updated.connected()) {
            return;
        }
        System.out.println("node status observed. node=" + nodeId
            + ", rid=" + routingIds.getOrDefault(nodeId, "none")
            + ", registered=" + updated.registered()
            + ", connected=" + updated.connected());
    }

    private static final class ListCopy {
        private static <T> List<T> copy(List<T> value) {
            return value == null ? List.of() : List.copyOf(new ArrayList<>(value));
        }
    }
}
