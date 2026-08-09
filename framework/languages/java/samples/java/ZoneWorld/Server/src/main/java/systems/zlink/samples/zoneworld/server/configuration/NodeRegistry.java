package systems.zlink.samples.zoneworld.server.configuration;

import java.util.ArrayList;
import java.util.Comparator;
import java.util.List;
import java.util.Map;
import java.util.concurrent.ConcurrentHashMap;
import systems.zlink.samples.zoneworld.shared.Messages;
import systems.zlink.samples.zoneworld.shared.ZoneWorldSpec;
public final class NodeRegistry {
    private final Map<String, Messages.NodeView> nodes = new ConcurrentHashMap<>();

    public void report(Messages.ReportNodeStatusMsg report) {
        nodes.put(report.nodeId(), new Messages.NodeView(
            report.nodeId(), true, true, report.maintenance(), report.zones(), report.playerCount()));
        System.out.println("report node=" + report.nodeId()
            + " zones=" + report.zones() + " players=" + report.playerCount());
    }

    public void alert(Messages.ReportSpotEventMsg report) {
        System.out.println("node alert node=" + report.nodeId()
            + " kind=" + report.kind() + " detail=" + report.detail());
    }

    public void setMaintenance(String nodeId, boolean enabled) {
        nodes.compute(nodeId, (ignored, current) -> {
            if (current == null) {
                return new Messages.NodeView(
                    nodeId, false, false, enabled,
                    ZoneWorldSpec.zonesOf(nodeId), 0);
            }
            return new Messages.NodeView(
                current.nodeId(), current.registered(), current.connected(), enabled,
                current.zones(), current.playerCount());
        });
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

    private static final class ListCopy {
        private static <T> List<T> copy(List<T> value) {
            return value == null ? List.of() : List.copyOf(new ArrayList<>(value));
        }
    }
}
