package systems.zlink.samples.zoneworld.server.configuration;

import java.util.concurrent.ConcurrentHashMap;
import java.util.concurrent.ConcurrentMap;
public final class NodeMaintenanceState {
    private final ConcurrentMap<String, Boolean> states = new ConcurrentHashMap<>();

    public void apply(String nodeId, boolean enabled) {
        states.put(nodeId, enabled);
    }

    public boolean isUnderMaintenance(String nodeId) {
        return states.getOrDefault(nodeId, false);
    }

    public boolean rejectsArrival(String targetNodeId, String sourceNodeId) {
        return isUnderMaintenance(targetNodeId)
            && !targetNodeId.equals(sourceNodeId);
    }
}
