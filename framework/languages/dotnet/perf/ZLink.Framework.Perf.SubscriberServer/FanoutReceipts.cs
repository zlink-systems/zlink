using Zlink.Framework.Contracts.Channels;
using Zlink.Framework.Contracts.Configuration;

namespace ZLink.Framework.Perf;

// §10.11 Subscriber evidence: which measured sequences this process received first inside its own window or settle
// (§15.4). It counts nothing the Publisher published; the runner intersects both originals.
public sealed class FanoutReceipts
{
    private readonly IZLinkFanoutRuntime fanoutRuntime;
    private readonly Measurement measurement;
    private readonly ObjectsReadiness objects;
    private readonly RoleConfig config;
    private readonly string sequenceFile;
    private volatile Round round = new();

    private sealed class Round
    {
        public readonly SequenceBitSet Window = new(), Settle = new();
        public long Duplicates, WarmupEvents, IgnoredWarmupInMeasured, OutsideWindow;
        public volatile bool Sealed; // set when the runner collects the final snapshot: later events are missing deliveries
    }

    public FanoutReceipts(IZLinkFanoutRuntime fanoutRuntime, Measurement measurement, ObjectsReadiness objects, string cellDirectory)
    {
        this.fanoutRuntime = fanoutRuntime;
        this.measurement = measurement;
        this.objects = objects;
        config = measurement.Config;
        sequenceFile = Path.Combine(cellDirectory, $"subscriber-{config.roleInstance}-sequences.json");
        measurement.OnReset = () => round = new();
        measurement.MessageTypes = [("event", nameof(PerfPublishEvent))];
        measurement.EnrichSnapshot = Enrich;
    }

    // objectsReady: this Subscriber's public fanout status shows a Ready publisher (§16.1).
    public async Task PrepareAsync(CancellationToken stopping)
    {
        using var timeout = CancellationTokenSource.CreateLinkedTokenSource(stopping);
        timeout.CancelAfter(config.workload.setupTimeoutMs);
        try
        {
            while (true)
            {
                var status = fanoutRuntime.GetStatus(config.channelName!);
                if (status.IsReady && status.ReadyPublisherCount > 0)
                {
                    objects.Set(true, "", [new { kind = "fanoutStatus", source = "IZLinkFanoutRuntime.GetStatus", observedValue = status }]);
                    return;
                }
                await Task.Delay(10, timeout.Token);
            }
        }
        catch (Exception error) { measurement.RecordDiagnostic(error); }
    }

    public void Record(PerfPublishEvent message)
    {
        var current = round;
        if (message.runId != config.runId || message.cellId != config.cellId || message.topic != FanoutMetrics.Topic ||
            message.phase is not ("warmup" or "measured") || string.IsNullOrEmpty(message.clockDomainId))
            throw new PerfValidationException("IdentityMismatch", "Fanout event identity does not match the cell.");
        var sequence = DecimalText.U64(message.sequence);
        DecimalText.I64(message.sentTicks);
        measurement.Pattern.Validate(message.payload);
        if (message.phase == "warmup")
        {
            if (DecimalText.U64(message.resetSeq) != 0) throw new PerfValidationException("PhaseMismatch", "Warmup event carries a measured resetSeq.");
            Interlocked.Increment(ref current.WarmupEvents);
            if (measurement.SetupEvidence.Length == 0) measurement.SetupEvidence =
                [new { kind = "warmupMarker", source = "IZLinkFanoutHandler<PerfPublishEvent>", observedValue = message.sequence }];
            if (measurement.ResetSeq != "0") Interlocked.Increment(ref current.IgnoredWarmupInMeasured);
            return;
        }
        if (message.resetSeq != measurement.ResetSeq) throw new PerfValidationException("PhaseMismatch", "Measured event resetSeq differs from this epoch.");
        // The runner ends the settle (§4.1): receipts after the window are settle until it reads the final snapshot.
        var target = current.Sealed ? null : measurement.Phase switch { "measured" => current.Window, "settle" or "complete" => current.Settle, _ => null };
        if (target is null) Interlocked.Increment(ref current.OutsideWindow);
        else if (current.Window.Contains(sequence) || current.Settle.Contains(sequence) || !target.TrySet(sequence))
            Interlocked.Increment(ref current.Duplicates);
    }

    private void Enrich(PerfMetricsSnapshot snapshot)
    {
        var current = round;
        FanoutMetrics.ApplyCommon(snapshot, hasDeliveryOwner: true);
        FanoutMetrics.Value(snapshot, "fanout.duplicateEvents", DecimalText.Of((ulong)Interlocked.Read(ref current.Duplicates)));
        snapshot.runtimeMetrics["fanoutReceipts"] = new { name = "subscriber receipts", unit = "event", type = "object", value = new
        {
            uniqueInWindow = DecimalText.Of(current.Window.Count), uniqueInSettle = DecimalText.Of(current.Settle.Count),
            warmupEvents = DecimalText.Of((ulong)Interlocked.Read(ref current.WarmupEvents)),
            warmupInMeasuredEpoch = DecimalText.Of((ulong)Interlocked.Read(ref current.IgnoredWarmupInMeasured)),
            measuredOutsideWindow = DecimalText.Of((ulong)Interlocked.Read(ref current.OutsideWindow)) } };
        snapshot.provenance["fanout"] = new { channelName = config.channelName, topic = FanoutMetrics.Topic,
            subscribedTopics = Array.Empty<string>(), delivery = "typed IZLinkFanoutHandler<PerfPublishEvent>",
            sequenceEvidence = new { method = "one bit per received sequence, window and settle sets", retainedBytes = DecimalText.Of(current.Window.RetainedBytes + current.Settle.RetainedBytes),
                timingEvidence = "not collected: no shared clock domain", original = $"subscriber-{config.roleInstance}-sequences.json" } };
        if (!measurement.FinalSnapshot || snapshot.phase != "complete" || snapshot.resetSeq != "1") return;
        current.Sealed = true;
        FanoutMetrics.WriteOnce(sequenceFile, new SubscriberSequences
        {
            runId = config.runId, cellId = config.cellId, resetSeq = snapshot.resetSeq, phase = "measured",
            subscriberId = config.roleInstance, windowRanges = current.Window.Ranges(), settleRanges = current.Settle.Ranges(),
            duplicateEvents = (ulong)Interlocked.Read(ref current.Duplicates),
            nullReasons = new() { ["/timingEvidence"] = new("CLOCK_DOMAIN_UNVERIFIED",
                "Publisher and Subscriber use process-local monotonic clocks; no shared clock domain is verified (§15.2).") },
            timingEvidence = null
        });
    }
}
