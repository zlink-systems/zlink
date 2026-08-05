package systems.zlink.framework.runtime.channels;

import java.util.Map;
import java.util.concurrent.ConcurrentHashMap;

/**
 * Pre-built label sets for the spec 25 mesh_node request instruments.
 *
 * <p>The labels are fixed per mesh and surface, and {@code outcome} only takes
 * three values, so every map is built once and shared. A request must not
 * allocate a label map or build a label string on its own path.
 */
final class ZLinkRequestMetricTags {
    //  Keyed by the caller's own name string. Building a composite key here
    //  would allocate on every request, so each surface owns its own cache.
    private static final ConcurrentHashMap<String, ZLinkRequestMetricTags> CHANNEL_CACHE =
        new ConcurrentHashMap<>();

    final Map<String, String> request;
    final Map<String, String> completed;
    final Map<String, String> failed;
    final Map<String, String> timedOut;

    private ZLinkRequestMetricTags(String meshName, String surface) {
        request = Map.of("mesh_name", meshName, "surface", surface);
        completed = Map.of(
            "mesh_name", meshName, "surface", surface, "outcome", "completed");
        failed = Map.of(
            "mesh_name", meshName, "surface", surface, "outcome", "failed");
        timedOut = Map.of(
            "mesh_name", meshName, "surface", surface, "outcome", "timed_out");
    }

    static ZLinkRequestMetricTags forChannel(String meshName) {
        return CHANNEL_CACHE.computeIfAbsent(
            meshName, name -> new ZLinkRequestMetricTags(name, "channel"));
    }

    Map<String, String> duration(boolean timedOut, boolean failed) {
        return timedOut ? this.timedOut : failed ? this.failed : completed;
    }
}
