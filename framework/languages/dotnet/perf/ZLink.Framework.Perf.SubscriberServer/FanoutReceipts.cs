using Zlink.Framework.Contracts.Channels;
using Zlink.Framework.Contracts.Configuration;

namespace ZLink.Framework.Perf;

// §10.11 Subscriber evidence: which measured sequences this process first received inside its own window
// (§15.4). It counts nothing the Publisher published; the runner intersects both originals.
public sealed class FanoutReceipts
{
    private readonly IZLinkFanoutRuntime fanoutRuntime;
    private readonly Measurement measurement;
    private readonly ObjectsReadiness objects;
    private readonly RoleConfig config;
    private readonly string sequenceFile;
    private readonly object gate = new();
    private Round round = new();

    private sealed class Round
    {
        public readonly SequenceBitSet Window = new();
        public readonly SequenceBitSet Seen = new();
        public long Duplicates, WarmupEvents, IgnoredWarmupInMeasured, OutsideWindow;
    }

    public FanoutReceipts(IZLinkFanoutRuntime fanoutRuntime, Measurement measurement, ObjectsReadiness objects, string cellDirectory)
    {
        this.fanoutRuntime = fanoutRuntime;
        this.measurement = measurement;
        this.objects = objects;
        config = measurement.Config;
        sequenceFile = Path.Combine(cellDirectory, $"subscriber-{config.roleInstance}-sequences.json");
        measurement.OnReset = () => { lock (gate) round = new(); };
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

    public void Record(PerfPublishEvent message, long receivedTicks)
    {
        var resetSeq = measurement.ResetSeq;
        var startTicks = measurement.StartTicks;
        var endTicks = measurement.EndTicks;
        if (message.runId != config.runId || message.cellId != config.cellId || message.topic != FanoutMetrics.Topic ||
            message.phase is not ("warmup" or "measured") || string.IsNullOrEmpty(message.clockDomainId))
            throw new PerfValidationException("IdentityMismatch", "Fanout event identity does not match the cell.");
        var sequence = DecimalText.U64(message.sequence);
        DecimalText.I64(message.sentTicks);
        measurement.Pattern.Validate(message.payload);
        var messageResetSeq = DecimalText.U64(message.resetSeq);
        if (message.phase == "warmup" ? messageResetSeq != 0 : message.resetSeq != resetSeq)
            throw new PerfValidationException("PhaseMismatch", "Fanout event resetSeq differs from this epoch.");
        if (message.phase == "warmup" && measurement.SetupEvidence.Length == 0)
            measurement.SetupEvidence =
                [new { kind = "warmupMarker", source = "IZLinkFanoutHandler<PerfPublishEvent>", observedValue = message.sequence }];
        lock (gate)
        {
            if (message.phase == "warmup")
            {
                round.WarmupEvents++;
                if (resetSeq != "0") round.IgnoredWarmupInMeasured++;
                return;
            }
            if (!round.Seen.TrySet(sequence)) { round.Duplicates++; return; }
            if (receivedTicks < startTicks || receivedTicks >= endTicks) round.OutsideWindow++;
            else round.Window.TrySet(sequence);
        }
    }

    private void Enrich(PerfMetricsSnapshot snapshot)
    {
        lock (gate)
        {
            FanoutMetrics.ApplyCommon(snapshot, hasDeliveryOwner: true);
            var current = round;
            FanoutMetrics.Value(snapshot, "fanout.duplicateEvents", DecimalText.Of((ulong)current.Duplicates));
            snapshot.runtimeMetrics["fanoutReceipts"] = new { name = "subscriber receipts", unit = "event", type = "object", value = new
            {
                uniqueInWindow = DecimalText.Of(current.Window.Count),
                measuredEventsSeen = DecimalText.Of(current.Seen.Count),
                warmupEvents = DecimalText.Of((ulong)current.WarmupEvents),
                warmupInMeasuredEpoch = DecimalText.Of((ulong)current.IgnoredWarmupInMeasured),
                measuredOutsideWindow = DecimalText.Of((ulong)current.OutsideWindow) } };
            snapshot.provenance["fanout"] = new { channelName = config.channelName, topic = FanoutMetrics.Topic,
                subscribedTopics = Array.Empty<string>(), delivery = "typed IZLinkFanoutHandler<PerfPublishEvent>",
                sequenceEvidence = new { method = "one bit per measured sequence seen and one bit per window receipt",
                    retainedBytes = DecimalText.Of(current.Window.RetainedBytes + current.Seen.RetainedBytes),
                    timingEvidence = "not collected: no shared clock domain", original = $"subscriber-{config.roleInstance}-sequences.json" } };
            if (!measurement.FinalSnapshot || snapshot.phase != "complete" || snapshot.resetSeq != "1") return;
            FanoutMetrics.WriteOnce(sequenceFile, new SubscriberSequences
            {
                runId = config.runId, cellId = config.cellId, resetSeq = snapshot.resetSeq, phase = "measured",
                subscriberId = config.roleInstance, windowRanges = current.Window.Ranges(),
                duplicateEvents = (ulong)current.Duplicates,
                nullReasons = new() { ["/timingEvidence"] = new("CLOCK_DOMAIN_UNVERIFIED",
                    "Publisher and Subscriber use process-local monotonic clocks; no shared clock domain is verified (§15.2).") },
                timingEvidence = null
            });
        }
    }
}
