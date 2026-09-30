package systems.zlink.framework.perf;

import java.util.List;
import java.util.Map;

// The §14 keys every original carries. A family a cell does not measure stays null with NOT_APPLICABLE; the scenario
// that measures it replaces the null (ScenarioMetrics).
public final class MetricCatalog {
    private MetricCatalog() {}

    public static final List<String> LATENCY_SUFFIXES = List.of("meanMs", "p50Ms", "p95Ms", "p99Ms", "maxMs");
    public static final List<String> AUXILIARY_PREFIXES = List.of("actor.sourceAdmission.latency", "spot.remoteCallLatency",
            "driver.latency", "worker.callLatency", "worker.submitToStart", "worker.taskLatency",
            "worker.resultToContinuation", "fanout.deliveryLatency", "fanout.settleDeliveryLatency");
    public static final List<String> AUXILIARY_HISTOGRAMS = List.of("sourceAdmissionMs", "driverLatencyMs",
            "workerCallLatencyMs", "workerSubmitToStartMs", "workerTaskLatencyMs", "workerResultToContinuationMs",
            "fanoutDeliveryLatencyMs", "fanoutSettleDeliveryLatencyMs");
    public static final List<String> INAPPLICABLE = List.of("messages.admitted", "messages.expired",
            "messages.duplicateReply", "messages.lateReply", "messages.unknownCorrelation", "spot.applicationYieldCalls",
            "spot.applicationHandlerEntries", "driver.issued", "driver.notStarted", "driver.failed", "messages.published",
            "messages.publishedInWindow", "messages.settlePublished", "fanout.subscriberCount", "fanout.uniqueDelivered",
            "fanout.deliveredInWindow", "fanout.settleDelivered", "fanout.duplicateEvents", "fanout.outOfCohortEvents",
            "fanout.deliveryRatio", "fanout.publishOpsPerSec", "fanout.deliveryOpsPerSec", "spot.mailboxDepth.max",
            "spot.mailboxDepth.mean", "spot.suspendedTurns", "spot.resumedTurns", "spot.resumeLatency.p95Ms",
            "spot.resumeLatency.p99Ms", "worker.pool.queueDepth.max", "worker.pool.queueDepth.mean");
    public static final List<String> OUTCOMES =
            List.of("sent", "completed", "settleCompleted", "failed", "timeout", "cancelled", "unresolved");

    public static void nullValue(Map<String, Object> values, Map<String, NullReason> reasons, String container,
            String key, String code, String reason) {
        values.put(key, null);
        reasons.put("/" + container + "/" + key, new NullReason(code, reason));
    }

    public static void baselineNulls(Map<String, Object> metrics, Map<String, Object> histograms,
            Map<String, NullReason> reasons) {
        for (String key : INAPPLICABLE) {
            nullValue(metrics, reasons, "metrics", key, "NOT_APPLICABLE",
                    "The phase 1 request baseline has no corresponding operation.");
        }
        for (String prefix : AUXILIARY_PREFIXES) {
            for (String suffix : LATENCY_SUFFIXES) {
                nullValue(metrics, reasons, "metrics", prefix + "." + suffix, "NOT_APPLICABLE",
                        "The phase 1 request baseline has no corresponding operation.");
            }
        }
        for (String key : AUXILIARY_HISTOGRAMS) {
            nullValue(histograms, reasons, "histograms", key, "NOT_APPLICABLE",
                    "The request baseline does not measure this interval.");
        }
        for (String suffix : List.of("p50Ms", "p95Ms", "p99Ms")) {
            nullValue(metrics, reasons, "metrics", "host.queueWaitLatency." + suffix, "PUBLIC_OBSERVATION_UNSUPPORTED",
                    "Public status provides no exact pre-receive to handler queue-wait hook.");
        }
    }
}
