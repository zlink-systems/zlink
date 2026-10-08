using Zlink.Framework.Contracts.Channels;
using Zlink.Framework.Contracts.Configuration;

namespace ZLink.Framework.Perf;

// §10.11 Publisher: one process issues every sequence of the run. Each logical stream awaits the public publish
// admission (IZLinkFanoutClient.Publish(...).Async()); nothing waits for a subscriber. Delivery is not observed here:
// the Subscribers' own originals are intersected with this process's window-success set by the runner (§15.4).
public sealed class PubSubFanoutEchoScenario
{
    private readonly IZLinkFanoutClient fanout;
    private readonly IZLinkFrameworkRuntime runtime;
    private readonly Measurement measurement;
    private readonly ObjectsReadiness objects;
    private readonly RoleConfig config;
    private readonly string sequenceFile;
    private ulong issued; // run-wide: warmup and measured ranges never overlap
    private ulong measuredBase; // `issued` when the measured epoch was reset
    private volatile PublishedSets sets = new();

    private sealed class PublishedSets
    {
        public readonly SequenceBitSet Window = new();
    }

    public PubSubFanoutEchoScenario(
        IZLinkFanoutClient fanout,
        IZLinkFrameworkRuntime runtime,
        Measurement measurement,
        ObjectsReadiness objects,
        string cellDirectory
    )
    {
        this.fanout = fanout;
        this.runtime = runtime;
        this.measurement = measurement;
        this.objects = objects;
        config = measurement.Config;
        sequenceFile = Path.Combine(cellDirectory, "publisher-sequences.json");
        measurement.OnReset = () =>
        {
            measuredBase = Volatile.Read(ref issued);
            sets = new();
        };
        measurement.MessageTypes = [("event", nameof(PerfPublishEvent))];
        measurement.EnrichSnapshot = Enrich;
    }

    // The Publisher has no subscriber-facing status: its host Ready plus the Subscribers' public Ready
    // (fanout runtime status, one per Subscriber process) is the prepared state.
    public async Task PrepareAsync(CancellationToken stopping)
    {
        using var timeout = CancellationTokenSource.CreateLinkedTokenSource(stopping);
        timeout.CancelAfter(config.workload.setupTimeoutMs);
        try
        {
            while (!runtime.Status.IsReady)
                await Task.Delay(10, timeout.Token);
            var status = runtime.Status;
            objects.Set(
                true,
                "",
                [
                    new
                    {
                        kind = "publisherHostReady",
                        source = "IZLinkFrameworkRuntime.Status",
                        observedValue = new
                        {
                            status.State,
                            status.IsReady,
                            status.AcceptingWork,
                        },
                    },
                ]
            );
        }
        catch (Exception error)
        {
            measurement.RecordDiagnostic(error);
        }
    }

    public Task RunAsync() =>
        ServerDrivenStreams.RunAdmissionsAsync(
            measurement,
            config.workload.logicalStreams!.Value,
            LoopAsync
        );

    private async Task LoopAsync(int stream)
    {
        if (!measurement.BeginOperation(out var started, "event"))
            return;
        var warmup = measurement.ResetSeq == "0";
        var sequence = Interlocked.Increment(ref issued);
        var message = new PerfPublishEvent
        {
            runId = config.runId,
            cellId = config.cellId,
            resetSeq = measurement.ResetSeq,
            phase = warmup ? "warmup" : "measured",
            sequence = DecimalText.Of(sequence),
            topic = FanoutMetrics.Topic,
            sentTicks = DecimalText.Of(started),
            clockDomainId = PerfClock.Domain,
            payload = measurement.Pattern.Base64,
        };
        try
        {
            await fanout.Publish(config.channelName!, FanoutMetrics.Topic, message).Async();
            if (measurement.CompleteOperation(started) && !warmup)
                sets.Window.TrySet(sequence);
            if (warmup && measurement.SetupEvidence.Length == 0)
                measurement.SetupEvidence =
                [
                    new
                    {
                        kind = "warmupMarkerPublished",
                        source = "IZLinkFanoutClient.Publish.Async",
                        observedValue = message.sequence,
                    },
                ];
        }
        catch (Exception error)
        {
            measurement.CompleteOperation(started, error);
        }
    }

    private void Enrich(PerfMetricsSnapshot snapshot)
    {
        FanoutMetrics.ApplyCommon(snapshot, hasDeliveryOwner: false);
        var current = sets;
        FanoutMetrics.Value(
            snapshot,
            "messages.publishedInWindow",
            DecimalText.Of(current.Window.Count)
        );
        var seconds = snapshot.window.measuredSeconds;
        if (seconds > 0)
            FanoutMetrics.Value(
                snapshot,
                "fanout.publishOpsPerSec",
                current.Window.Count / seconds.Value
            );
        else
            FanoutMetrics.Null(
                snapshot,
                "fanout.publishOpsPerSec",
                "PHASE_NOT_STARTED",
                "No measured window has run."
            );
        snapshot.provenance["fanout"] = new
        {
            channelName = config.channelName,
            topic = FanoutMetrics.Topic,
            noDrop = true,
            socketSendTimeoutMs = config.workload.socketSendTimeoutMs,
            publisherSequenceScope = "one counter per run; warmup and measured ranges are disjoint",
            sequenceOriginal = "publisher-sequences.json",
        };
        if (!measurement.FinalSnapshot || snapshot.phase != "complete" || snapshot.resetSeq != "1")
            return;
        var last = Volatile.Read(ref issued);
        FanoutMetrics.WriteOnce(
            sequenceFile,
            new PublisherSequences
            {
                runId = config.runId,
                cellId = config.cellId,
                resetSeq = snapshot.resetSeq,
                phase = "measured",
                attemptedRanges = last > measuredBase ? [new(measuredBase + 1, last)] : [],
                windowSuccessRanges = current.Window.Ranges(),
            }
        );
    }
}
