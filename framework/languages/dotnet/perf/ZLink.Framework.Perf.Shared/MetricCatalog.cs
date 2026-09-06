namespace ZLink.Framework.Perf;

public static class MetricCatalog
{
    public static readonly string[] LatencySuffixes = ["meanMs", "p50Ms", "p95Ms", "p99Ms", "maxMs"];
    public static readonly string[] AuxiliaryPrefixes = ["actor.sourceAdmission.latency", "spot.remoteCallLatency",
        "driver.latency", "worker.callLatency", "worker.submitToStart", "worker.taskLatency",
        "worker.resultToContinuation", "fanout.deliveryLatency", "fanout.settleDeliveryLatency"];
    public static readonly string[] AuxiliaryHistograms = ["sourceAdmissionMs", "driverLatencyMs",
        "workerCallLatencyMs", "workerSubmitToStartMs", "workerTaskLatencyMs", "workerResultToContinuationMs",
        "fanoutDeliveryLatencyMs", "fanoutSettleDeliveryLatencyMs"];
    public static readonly string[] Inapplicable = ["messages.admitted", "messages.expired", "messages.duplicateReply",
        "messages.lateReply", "messages.unknownCorrelation", "spot.applicationYieldCalls", "spot.applicationHandlerEntries",
        "driver.issued", "driver.notStarted", "driver.failed", "messages.published", "messages.publishedInWindow",
        "messages.settlePublished", "fanout.subscriberCount", "fanout.uniqueDelivered", "fanout.deliveredInWindow",
        "fanout.settleDelivered", "fanout.duplicateEvents", "fanout.outOfCohortEvents", "fanout.deliveryRatio",
        "fanout.publishOpsPerSec", "fanout.deliveryOpsPerSec", "spot.mailboxDepth.max", "spot.mailboxDepth.mean",
        "spot.suspendedTurns", "spot.resumedTurns", "spot.resumeLatency.p95Ms", "spot.resumeLatency.p99Ms",
        "worker.pool.queueDepth.max", "worker.pool.queueDepth.mean"];
    public static readonly string[] Outcomes = ["sent", "completed", "settleCompleted", "failed", "timeout", "cancelled", "unresolved"];
    public static void Null(Dictionary<string, object?> values, Dictionary<string, NullReason> reasons,
        string container, string key, string code, string reason)
    {
        values[key] = null;
        reasons["/" + container + "/" + key] = new(code, reason);
    }
    public static void BaselineNulls(Dictionary<string, object?> metrics, Dictionary<string, object?> histograms,
        Dictionary<string, NullReason> reasons)
    {
        foreach (var key in Inapplicable.Concat(AuxiliaryPrefixes.SelectMany(prefix => LatencySuffixes.Select(s => prefix + "." + s))))
            Null(metrics, reasons, "metrics", key, "NOT_APPLICABLE", "The phase 1 request baseline has no corresponding operation.");
        foreach (var key in AuxiliaryHistograms)
            Null(histograms, reasons, "histograms", key, "NOT_APPLICABLE", "The request baseline does not measure this interval.");
        foreach (var suffix in new[] { "p50Ms", "p95Ms", "p99Ms" })
            Null(metrics, reasons, "metrics", "host.queueWaitLatency." + suffix,
                "PUBLIC_OBSERVATION_UNSUPPORTED", "Public status provides no exact pre-receive to handler queue-wait hook.");
    }
}
