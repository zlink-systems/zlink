package systems.zlink.framework.runtime.internal.metrics;

import systems.zlink.framework.runtime.internal.diagnostics.ZLinkDispatchErrorReason;
import systems.zlink.framework.runtime.internal.diagnostics.ZLinkDispatchErrorSurface;
import systems.zlink.framework.runtime.internal.diagnostics.ZLinkDispatchMessageKind;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceTopologyRegistry.ChannelSelectionFailure;

import java.util.HashMap;
import java.util.Map;
import java.util.concurrent.ConcurrentHashMap;

/** Bounded, shared labels for confirmed mesh delivery and selection failures. */
public final class ZLinkMeshMessageMetrics {
    private static final ConcurrentHashMap<String, ZLinkMeshMessageMetrics> MESHES =
            new ConcurrentHashMap<>();
    private static final String[] DROP_REASONS = {
        ZLinkDispatchErrorReason.HANDLER_MISSING.traceName(),
        ZLinkDispatchErrorReason.PAYLOAD_DECODE_FAILED.traceName(),
        ZLinkDispatchErrorReason.BACKPRESSURE.traceName(),
        ZLinkDispatchErrorReason.STALE_TARGET.traceName(),
        ZLinkDispatchErrorReason.SHUTDOWN.traceName()
    };
    private final String meshName;
    private final Map<String, Map<String, Map<String, String>>> drops;
    private final ConcurrentHashMap<String, Map<String, Map<String, String>>> selections =
            new ConcurrentHashMap<>();

    private ZLinkMeshMessageMetrics(String meshName) {
        this.meshName = meshName;
        Map<String, Map<String, Map<String, String>>> surfaces = new HashMap<>();
        for (ZLinkDispatchErrorSurface metricSurface :
                new ZLinkDispatchErrorSurface[] {
                    ZLinkDispatchErrorSurface.NODE,
                    ZLinkDispatchErrorSurface.CHANNEL,
                    ZLinkDispatchErrorSurface.SPOT_ROUTE,
                    ZLinkDispatchErrorSurface.INSTANCE_SPOT,
                    ZLinkDispatchErrorSurface.SPOT_ACTOR
                }) {
            String surface = metricSurface.traceName();
            Map<String, Map<String, String>> reasons = new HashMap<>();
            for (String reason : DROP_REASONS) {
                reasons.put(
                        reason,
                        Map.of(
                                ZLinkRuntimeMetrics.Tag.MESH_NAME.wire(),
                                meshName,
                                ZLinkRuntimeMetrics.Tag.SURFACE.wire(),
                                surface,
                                ZLinkRuntimeMetrics.Tag.MESSAGE_KIND.wire(),
                                ZLinkDispatchMessageKind.SEND.traceName(),
                                ZLinkRuntimeMetrics.Tag.REASON.wire(),
                                reason));
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
                    ZLinkRuntimeMetrics.Metric.MESSAGES_DROPPED.metricName(),
                    drops.get(surface).get(reason));
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
                ZLinkRuntimeMetrics.Metric.CHANNEL_SELECTION_FAILURES.metricName(),
                tags.get(reason));
    }

    private Map<String, Map<String, String>> selectionTags(String channelName) {
        Map<String, Map<String, String>> tags = new HashMap<>();
        for (ChannelSelectionFailure failure : ChannelSelectionFailure.values()) {
            String reason = failure.wire();
            tags.put(
                    reason,
                    Map.of(
                            ZLinkRuntimeMetrics.Tag.MESH_NAME.wire(),
                            meshName,
                            ZLinkRuntimeMetrics.Tag.CHANNEL_NAME.wire(),
                            channelName,
                            ZLinkRuntimeMetrics.Tag.REASON.wire(),
                            reason));
        }
        return Map.copyOf(tags);
    }
}
