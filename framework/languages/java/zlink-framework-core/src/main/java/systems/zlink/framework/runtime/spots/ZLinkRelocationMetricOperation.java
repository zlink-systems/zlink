package systems.zlink.framework.runtime.spots;

import java.util.Map;
import systems.zlink.framework.locations.ZLinkPlacementObjectKind;
import systems.zlink.framework.runtime.internal.metrics.ZLinkRuntimeMetrics;

/** Measurement of the existing handoff operation; it never decides its outcome. */
final class ZLinkRelocationMetricOperation {
    private final long started = System.nanoTime();
    private long sealed;
    private final Map<String, String> startTags;
    private final Map<String, String> unitTags;

    private enum Policy {
        RECREATE,
        SNAPSHOT
    }

    private enum Outcome {
        COMPLETED,
        ABORTED,
        FAILED,
        SHUTDOWN
    }

    ZLinkRelocationMetricOperation(
            String meshName, ZLinkPlacementObjectKind kind, boolean snapshot) {
        startTags =
                Map.of(
                        ZLinkRuntimeMetrics.Tag.MESH_NAME.wire(),
                        meshName,
                        ZLinkRuntimeMetrics.Tag.OBJECT_KIND.wire(),
                        ZLinkRuntimeMetrics.wire(kind),
                        ZLinkRuntimeMetrics.Tag.POLICY.wire(),
                        ZLinkRuntimeMetrics.wire(snapshot ? Policy.SNAPSHOT : Policy.RECREATE));
        unitTags = ZLinkRuntimeMetrics.relocationUnitTags(kind.value());
        ZLinkRuntimeMetrics.increment(
                ZLinkRuntimeMetrics.Metric.RELOCATION_STARTED.metricName(), startTags);
    }

    ZLinkRelocationMetricOperation(ZLinkSpotRetireControl.StageRequest request, long sealed) {
        this(request, objectKind(request), sealed);
    }

    private ZLinkRelocationMetricOperation(
            ZLinkSpotRetireControl.StageRequest request,
            ZLinkPlacementObjectKind kind,
            long sealed) {
        this(
                request.meshName(),
                kind,
                kind == ZLinkPlacementObjectKind.ACTOR
                        ? request.participants().get(0).restoreSnapshot()
                        : request.restoreSpotSnapshot());
        this.sealed = sealed;
    }

    private static ZLinkPlacementObjectKind objectKind(
            ZLinkSpotRetireControl.StageRequest request) {
        if (request.instanceSpot()) return ZLinkPlacementObjectKind.INSTANCE_SPOT;
        for (var participant : request.participants()) {
            if (participant.objectKind() == ZLinkPlacementObjectKind.USER_SPOT.value())
                return ZLinkPlacementObjectKind.USER_SPOT;
        }
        return ZLinkPlacementObjectKind.ACTOR;
    }

    void sealed(long nanos) {
        sealed = nanos;
    }

    void envelope(ZLinkSpotRetireControl.StageRequest request) {
        ZLinkRuntimeMetrics.record(
                ZLinkRuntimeMetrics.Metric.RELOCATION_BYTES.metricName(),
                request.relocationPayloadSize(),
                startTags);
    }

    void cutoverTerminal() {
        if (sealed != 0)
            ZLinkRuntimeMetrics.record(
                    ZLinkRuntimeMetrics.Metric.RELOCATION_INTERRUPTION.metricName(),
                    secondsSince(sealed),
                    unitTags);
    }

    void complete(ZLinkRelocationTransitionClient.Settlement settlement, boolean sourceStopped) {
        Outcome outcome =
                settlement != null
                        ? switch (settlement) {
                            case TARGET_COMMITTED -> Outcome.COMPLETED;
                            case SOURCE_PRESERVED -> Outcome.ABORTED;
                            case SOURCE_LEASE_EXPIRED -> Outcome.FAILED;
                        }
                        : sourceStopped ? Outcome.SHUTDOWN : Outcome.FAILED;
        var tags =
                Map.of(
                        ZLinkRuntimeMetrics.Tag.MESH_NAME.wire(),
                        startTags.get(ZLinkRuntimeMetrics.Tag.MESH_NAME.wire()),
                        ZLinkRuntimeMetrics.Tag.OBJECT_KIND.wire(),
                        startTags.get(ZLinkRuntimeMetrics.Tag.OBJECT_KIND.wire()),
                        ZLinkRuntimeMetrics.Tag.POLICY.wire(),
                        startTags.get(ZLinkRuntimeMetrics.Tag.POLICY.wire()),
                        ZLinkRuntimeMetrics.Tag.OUTCOME.wire(),
                        ZLinkRuntimeMetrics.wire(outcome));
        ZLinkRuntimeMetrics.increment(
                ZLinkRuntimeMetrics.Metric.RELOCATION_COMPLETED.metricName(), tags);
        ZLinkRuntimeMetrics.record(
                ZLinkRuntimeMetrics.Metric.RELOCATION_DURATION.metricName(),
                secondsSince(started),
                tags);
    }

    private static double secondsSince(long start) {
        return Math.max(0L, System.nanoTime() - start) / 1_000_000_000d;
    }
}
