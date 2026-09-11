package systems.zlink.framework.runtime.internal.metrics;

import java.util.HashMap;
import java.util.Map;
import java.util.concurrent.ConcurrentHashMap;

/** Bounded, shared labels for confirmed mesh delivery and selection failures. */
public final class ZLinkMeshMessageMetrics {
    private static final ConcurrentHashMap<String, ZLinkMeshMessageMetrics> MESHES =
        new ConcurrentHashMap<>();
    private static final String[] DROP_REASONS = {
        "no_handler", "decode_error", "backpressure", "stale_target", "shutdown"
    };
    private final String meshName;
    private final Map<String, Map<String, Map<String, String>>> drops;
    private final ConcurrentHashMap<String, Map<String, Map<String, String>>>
        selections = new ConcurrentHashMap<>();

    private ZLinkMeshMessageMetrics(String meshName) {
        this.meshName = meshName;
        Map<String, Map<String, Map<String, String>>> surfaces = new HashMap<>();
        for (String surface : new String[] {
                "node", "channel", "spot", "instance_spot", "actor"}) {
            Map<String, Map<String, String>> reasons = new HashMap<>();
            for (String reason : DROP_REASONS) {
                reasons.put(reason, Map.of(
                    "mesh_name", meshName, "surface", surface,
                    "message_kind", "send", "reason", reason));
            }
            surfaces.put(surface, Map.copyOf(reasons));
        }
        drops = Map.copyOf(surfaces);
    }

    public static ZLinkMeshMessageMetrics forMesh(String meshName) {
        ZLinkMeshMessageMetrics existing = MESHES.get(meshName);
        if (existing != null) {
            return existing;
        }
        return MESHES.computeIfAbsent(meshName, ZLinkMeshMessageMetrics::new);
    }

    public void dropped(String surface, String reason) {
        if (ZLinkRuntimeMetrics.enabled()) {
            ZLinkRuntimeMetrics.increment(
                "zlink.mesh_node.messages.dropped", drops.get(surface).get(reason));
        }
    }

    public void selectionFailed(String channelName, String reason) {
        if (!ZLinkRuntimeMetrics.enabled()) {
            return;
        }
        Map<String, Map<String, String>> tags = selections.get(channelName);
        if (tags == null) {
            tags = selections.computeIfAbsent(channelName, this::selectionTags);
        }
        ZLinkRuntimeMetrics.increment(
            "zlink.mesh_node.channel.selection_failures", tags.get(reason));
    }

    private Map<String, Map<String, String>> selectionTags(String channelName) {
        Map<String, Map<String, String>> tags = new HashMap<>();
        for (String reason : new String[] {"no_member", "not_ready", "draining"}) {
            tags.put(reason, Map.of(
                "mesh_name", meshName, "channel_name", channelName, "reason", reason));
        }
        return Map.copyOf(tags);
    }
}
