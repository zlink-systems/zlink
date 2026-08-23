package systems.zlink.samples.zoneworld.server.configuration;

import java.util.List;
import java.util.Map;
import java.util.concurrent.ConcurrentHashMap;

public final class NodeCensus {
    private final Map<String, Integer> counts = new ConcurrentHashMap<>();

    public void record(String zoneId, int count) {
        counts.put(zoneId, count);
    }

    public int total() {
        return counts.values().stream().mapToInt(Integer::intValue).sum();
    }

    public List<String> zoneIds() {
        return counts.keySet().stream().sorted().toList();
    }
}
