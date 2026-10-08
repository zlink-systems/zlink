using Systems.Zlink.Stream.Connector.Contracts;
using Zlink.Framework.Contracts.Errors;

namespace ZLink.Framework.Perf;

// Application cohort accounting only. No socket state, retry, transport polling or completion pump.
public sealed class Measurement(RoleConfig config, bool primary) : IDisposable
{
    private readonly object gate = new();
    private readonly ProcessSampler sampler = new();
    private Histogram latency = new();
    private readonly Dictionary<string, ulong> counts = [];
    private readonly Dictionary<string, ulong> byKind = [],
        harness = [],
        language = [];
    private readonly List<object> errors = [];
    private readonly List<object> publicStateSamples = [];
    private readonly Dictionary<string, ulong> directional = [];
    private ulong inflight,
        maxInflight;
    private int activeHandlers;
    private TaskCompletionSource? drainWaiter;
    private long start,
        end;
    private string? startUnix,
        endUnix;
    private string phase = "setup",
        resetSeq = "0";
    private bool sealedResults;
    private ResetReply? resetAck;
    private readonly Dictionary<string, PerfTriggerReply> starts = [];
    private Task phaseTask = Task.CompletedTask;
    public RoleConfig Config => config;
    public PayloadPattern Pattern { get; } = new(config.workload.payloadSize);
    public string Phase
    {
        get
        {
            lock (gate)
                return phase;
        }
    }
    public string ResetSeq
    {
        get
        {
            lock (gate)
                return resetSeq;
        }
    }
    public long EndTicks
    {
        get
        {
            lock (gate)
                return end;
        }
    }
    public long StartTicks
    {
        get
        {
            lock (gate)
                return start;
        }
    }
    public Task PhaseTask
    {
        get
        {
            lock (gate)
                return phaseTask;
        }
    }

    public long CallDeadlineTicks()
    {
        lock (gate)
            return start == 0
                ? checked(
                    PerfClock.Now
                    + (long)config.workload.setupTimeoutMs * PerfClock.NanosecondsPerMillisecond
                )
                : checked(
                    end + (long)config.workload.drainTimeoutMs * PerfClock.NanosecondsPerMillisecond
                );
    }

    public TimeSpan CallTimeout(bool driver = false)
    {
        var remaining = Math.Max(1L, CallDeadlineTicks() - PerfClock.Now);
        var milliseconds = Math.DivRem(
            remaining,
            PerfClock.NanosecondsPerMillisecond,
            out var remainder
        );
        if (remainder != 0)
            milliseconds++;
        if (driver)
            milliseconds += config.workload.drainTimeoutMs;
        return TimeSpan.FromMilliseconds(milliseconds);
    }

    private bool IsDrained() => inflight == 0 && activeHandlers == 0;

    public Task WaitForOperationsAsync()
    {
        lock (gate)
            return IsDrained()
                ? Task.CompletedTask
                : (drainWaiter ??= new(TaskCreationOptions.RunContinuationsAsynchronously)).Task;
    }

    public bool CanIssue
    {
        get
        {
            lock (gate)
                return !sealedResults && start != 0 && PerfClock.Now < end;
        }
    }
    public bool HasErrors
    {
        get
        {
            lock (gate)
                return byKind.Count + harness.Count + language.Count != 0;
        }
    }
    public object[] ErrorEvidence
    {
        get
        {
            lock (gate)
                return errors.ToArray();
        }
    }
    public ulong Connected { get; set; }
    public ulong ConnectionFailures { get; set; }
    public object[] SetupEvidence { get; set; } = [];
    public string? CoreVersion { get; set; }
    public Func<object>? SamplePublicState { get; set; }

    // A scenario's own counters: cleared with the window at reset, and added to every snapshot (family metrics, §14).
    public Action? OnReset { get; set; }
    public Action<PerfMetricsSnapshot>? EnrichSnapshot { get; set; }

    // True only while the runner reads the final snapshot of a phase.
    public bool FinalSnapshot { get; set; }

    // The typed messages this scenario's measured path carries, one serializedMessageBytes row each (§15.2).
    public (string direction, string packetName)[] MessageTypes { get; set; } =
    [("request", nameof(PerfEchoRequest)), ("reply", nameof(PerfEchoReply))];

    public PerfEchoRequest Request(int stream, ulong sequence, bool probe = false) =>
        new()
        {
            runId = config.runId,
            cellId = config.cellId,
            resetSeq = probe ? "0" : ResetSeq,
            phase = probe || Phase == "warmup" ? "warmup" : "measured",
            clientId = stream,
            sequence = DecimalText.Of(sequence),
            correlationId =
                $"{config.cellId}/{(probe || Phase == "warmup" ? "warmup" : "measured")}/{stream}/{sequence}",
            sentTicks = DecimalText.Of(PerfClock.Now),
            clockDomainId = PerfClock.Domain,
            returnSpotId = null,
            returnChannel = null,
            payload = Pattern.Base64,
        };

    // A send/send request names its return address (§10.4, §10.6); an echo request names none.
    public void ValidateRequest(
        PerfEchoRequest request,
        string? returnChannel = null,
        string? returnSpotId = null
    )
    {
        if (
            request.runId != config.runId
            || request.cellId != config.cellId
            || request.clientId < 0
            || request.phase is not ("warmup" or "measured")
            || request.returnSpotId != returnSpotId
            || request.returnChannel != returnChannel
            || request.correlationId
                != $"{request.cellId}/{request.phase}/{request.clientId}/{request.sequence}"
            || string.IsNullOrEmpty(request.clockDomainId)
        )
            throw new PerfValidationException(
                "IdentityMismatch",
                "Request identity does not match the cell."
            );
        DecimalText.U64(request.sequence);
        DecimalText.I64(request.sentTicks);
        var seq = DecimalText.U64(request.resetSeq);
        if (request.phase == "warmup" ? seq != 0 : seq == 0 || request.resetSeq != ResetSeq)
            throw new PerfValidationException(
                "PhaseMismatch",
                "Request reset sequence does not match the phase."
            );
        Pattern.Validate(request.payload);
    }

    public PerfTriggerReply Start(PerfTriggerRequest trigger, Func<Task>? workload)
    {
        lock (gate)
        {
            if (
                trigger.runId != config.runId
                || trigger.cellId != config.cellId
                || trigger.phase is not ("warmup" or "measured")
                || trigger.resetSeq != resetSeq
                || (trigger.phase == "warmup" ? resetSeq != "0" : resetSeq == "0")
            )
                return Ack(trigger, false, "rejected", "Identity, resetSeq or phase is invalid.");
            var key = trigger.phase + "/" + trigger.resetSeq;
            if (starts.TryGetValue(key, out var previous))
                return previous with { state = "alreadyStarted" };
            if (
                !phaseTask.IsCompleted
                || !IsDrained()
                || (trigger.phase == "warmup" ? phase != "setup" : phase != "reset")
            )
                return Ack(trigger, false, "rejected", "Previous phase has not drained and reset.");
            phase = trigger.phase;
            sealedResults = false;
            start = PerfClock.Now;
            end = checked(
                start
                + (long)(
                    (
                        phase == "warmup"
                            ? config.workload.warmupSeconds
                            : config.workload.durationSeconds
                    ) * 1e9
                )
            );
            startUnix = PerfClock.UnixMs;
            sampler.Start();
            var ack = Ack(trigger, true, "started", null);
            starts.Add(key, ack);
            // Launching on the pool lets the HTTP/control acknowledgement leave before load consumes threads.
            phaseTask = Task.Run(() => RunPhase(workload));
            return ack;
        }
    }

    private PerfTriggerReply Ack(
        PerfTriggerRequest trigger,
        bool accepted,
        string state,
        string? reason
    ) =>
        new()
        {
            runId = trigger.runId,
            cellId = trigger.cellId,
            resetSeq = trigger.resetSeq,
            phase = trigger.phase,
            accepted = accepted,
            state = state,
            reason = reason,
            configHash = config.configHash,
        };

    private async Task RunPhase(Func<Task>? workload)
    {
        var currentPhase = Phase;
        Task operations;
        try
        {
            operations = workload?.Invoke() ?? Task.CompletedTask;
        }
        catch (Exception error)
        {
            RecordDiagnostic(error);
            operations = Task.CompletedTask;
        }
        var observedOperations = operations.ContinueWith(
            completed =>
            {
                if (completed.IsFaulted)
                    RecordDiagnostic(completed.Exception!.GetBaseException());
                else if (completed.IsCanceled)
                    RecordDiagnostic(new OperationCanceledException());
            },
            CancellationToken.None,
            TaskContinuationOptions.ExecuteSynchronously,
            TaskScheduler.Default
        );
        var initialPublicState = SamplePublicState?.Invoke();
        if (initialPublicState is not null)
            lock (gate)
                publicStateSamples.Add(initialPublicState);
        while (true)
        {
            var remaining = end - PerfClock.Now;
            if (remaining <= 0)
                break;
            await Task.Delay(TimeSpan.FromMilliseconds(Math.Min(100, remaining / 1e6)))
                .ConfigureAwait(false);
            // Public runtime status may marshal to a runtime lane; never call it under the counter lock.
            var publicState = SamplePublicState?.Invoke();
            lock (gate)
            {
                sampler.Sample();
                if (publicState is not null)
                    publicStateSamples.Add(publicState);
            }
        }
        // Warmup must drain before reset; measured seals the window with unresolved work counted as inflight.
        if (currentPhase == "warmup")
        {
            await observedOperations.ConfigureAwait(false);
            await WaitForOperationsAsync().ConfigureAwait(false);
        }
        lock (gate)
        {
            sampler.End();
            endUnix = PerfClock.UnixMs;
            sealedResults = true;
            phase = "complete";
        }
    }

    public ResetReply Reset(ResetRequest request, Func<ulong>? resetCapacity)
    {
        var requested = DecimalText.U64(request.resetSeq);
        lock (gate)
        {
            var drained =
                phaseTask.IsCompleted && (start == 0 || PerfClock.Now >= end) && IsDrained();
            if (
                request.runId == config.runId
                && request.cellId == config.cellId
                && resetAck?.resetSeq == request.resetSeq
                && drained
            )
                return resetAck;
            string? reason =
                request.runId != config.runId || request.cellId != config.cellId
                    ? "Different run or cell."
                : requested == 0 || requested <= DecimalText.U64(resetSeq)
                    ? "resetSeq must advance."
                : !drained || phase == "setup" ? "Warmup or measured operations have not drained."
                : HasErrors ? "Previous phase failed."
                : null;
            if (reason is not null)
                return new(
                    false,
                    request.runId,
                    request.cellId,
                    config.role,
                    config.roleInstance,
                    request.resetSeq,
                    PerfClock.UnixMs,
                    null,
                    reason,
                    []
                );
            counts.Clear();
            byKind.Clear();
            harness.Clear();
            language.Clear();
            errors.Clear();
            directional.Clear();
            publicStateSamples.Clear();
            latency = new();
            maxInflight = 0;
            drainWaiter = null;
            start = end = 0;
            startUnix = endUnix = null;
            sealedResults = false;
            OnReset?.Invoke();
            resetSeq = request.resetSeq;
            phase = "reset";
            var resetAt = PerfClock.UnixMs;
            var epoch = resetCapacity?.Invoke();
            Dictionary<string, NullReason> reasons = [];
            if (epoch is null)
                reasons["/capacityEpoch"] = new(
                    "NOT_APPLICABLE",
                    "The client owns no Framework host."
                );
            return resetAck = new(
                true,
                config.runId,
                config.cellId,
                config.role,
                config.roleInstance,
                resetSeq,
                resetAt,
                epoch.HasValue ? DecimalText.Of(epoch.Value) : null,
                null,
                reasons
            );
        }
    }

    public bool BeginOperation(out long started, string direction = "request")
    {
        lock (gate)
        {
            started = PerfClock.Now;
            if (sealedResults || start == 0 || started >= end)
                return false;
            Increment(counts, "sent");
            inflight = checked(inflight + 1);
            maxInflight = Math.Max(maxInflight, inflight);
            Increment(directional, direction);
            return true;
        }
    }

    public bool CompleteOperation(
        long started,
        Exception? error = null,
        long? completedTicks = null
    )
    {
        var completed = completedTicks ?? PerfClock.Now;
        lock (gate)
        {
            if (sealedResults)
                return false;
            inflight--;
            if (IsDrained())
                drainWaiter?.TrySetResult();
            if (completed >= end)
                return false;
            if (error is null)
            {
                Increment(counts, "completed");
                latency.Record(completed - started);
                return true;
            }
            RecordError(error, true);
            return false;
        }
    }

    public void HandlerEnter()
    {
        lock (gate)
            activeHandlers++;
    }

    public void HandlerExit()
    {
        lock (gate)
        {
            activeHandlers--;
            if (IsDrained())
                drainWaiter?.TrySetResult();
        }
    }

    public void RecordReply(PerfEchoRequest request) => RecordApplicationCall(request, "reply");

    // A public call this process starts (send) or a typed reply it returns, counted once inside its own window.
    public void RecordApplicationCall(PerfEchoRequest request, string direction)
    {
        lock (gate)
            if (
                request.resetSeq == resetSeq
                && start != 0
                && PerfClock.Now < end
                && request.phase == (resetSeq == "0" ? "warmup" : "measured")
            )
                Increment(directional, direction);
    }

    public void RecordDiagnostic(Exception error)
    {
        lock (gate)
        {
            if (!sealedResults)
                RecordError(error, false);
        }
    }

    private void RecordError(Exception error, bool outcome)
    {
        var category = "failed";
        if (error is ZLinkFrameworkException framework)
        {
            // The public enum carries the Framework error kind.
            Increment(byKind, framework.Kind.ToString());
            if (framework.Kind == ZLinkFrameworkErrorKind.DeadlineExceeded)
                category = "timeout";
        }
        else if (error is PerfValidationException validation)
        {
            Increment(harness, validation.Kind);
            if (validation.Kind == "CorrelationExpired")
                category = "timeout"; // §13: an expired correlation is a timeout
        }
        else if (error is ZlinkStreamException connector)
        {
            Increment(language, connector.GetType().FullName!);
            if (connector.Error.Code == ZlinkStreamErrorCode.RequestTimeout)
                category = "timeout";
        }
        else
        {
            Increment(language, error.GetType().FullName ?? error.GetType().Name);
            if (error is OperationCanceledException)
                category = "cancelled";
            else if (error is TimeoutException)
                category = "timeout";
        }
        if (outcome)
            Increment(counts, category);
        if (errors.Count < 32)
            errors.Add(
                new
                {
                    type = error.GetType().FullName,
                    error.Message,
                    publicKind = (error as ZLinkFrameworkException)?.Kind.ToString(),
                    harnessKind = (error as PerfValidationException)?.Kind,
                    connectorCode = (error as ZlinkStreamException)?.Error.Code.ToString(),
                }
            );
    }

    private static void Increment(Dictionary<string, ulong> values, string key) =>
        values[key] = checked(values.GetValueOrDefault(key) + 1);

    public PerfMetricsSnapshot Snapshot(object? publicStatus)
    {
        lock (gate)
        {
            Dictionary<string, object?> metrics = [],
                histograms = [],
                runtime = [];
            Dictionary<string, NullReason> reasons = [];
            MetricCatalog.BaselineNulls(metrics, histograms, reasons);
            var inflightAtEnd =
                counts.GetValueOrDefault("sent")
                - counts.GetValueOrDefault("completed")
                - counts.GetValueOrDefault("failed")
                - counts.GetValueOrDefault("timeout")
                - counts.GetValueOrDefault("cancelled");
            foreach (var key in MetricCatalog.Outcomes)
                if (key == "inflightAtEnd")
                    metrics["messages." + key] = DecimalText.Of(inflightAtEnd);
                else if (primary)
                    metrics["messages." + key] = DecimalText.Of(counts.GetValueOrDefault(key));
                else
                    MetricCatalog.Null(
                        metrics,
                        reasons,
                        "metrics",
                        "messages." + key,
                        "NOT_APPLICABLE",
                        "Echo outcomes belong to the source process."
                    );
            if (primary)
            {
                latency.Export("latencyMs", "latency", metrics, histograms, reasons);
            }
            else
            {
                foreach (var prefix in new[] { "latency" })
                foreach (var suffix in MetricCatalog.LatencySuffixes)
                    MetricCatalog.Null(
                        metrics,
                        reasons,
                        "metrics",
                        prefix + "." + suffix,
                        "NOT_APPLICABLE",
                        "RTT belongs to the source process."
                    );
                foreach (var key in new[] { "latencyMs" })
                    MetricCatalog.Null(
                        histograms,
                        reasons,
                        "histograms",
                        key,
                        "NOT_APPLICABLE",
                        "RTT belongs to the source process."
                    );
            }
            var csClient = config.role == "client" && config.workload.connections is not null;
            foreach (var key in new[] { "requested", "connected", "failed" })
                if (csClient)
                    metrics["connections." + key] = DecimalText.Of(
                        key switch
                        {
                            "requested" => (ulong)(
                                config.workload.connections!.Value / config.workload.clientCount
                                + (
                                    config.roleInstance
                                    < config.workload.connections.Value
                                        % config.workload.clientCount
                                        ? 1
                                        : 0
                                )
                            ),
                            "connected" => Connected,
                            _ => ConnectionFailures,
                        }
                    );
                else
                    MetricCatalog.Null(
                        metrics,
                        reasons,
                        "metrics",
                        "connections." + key,
                        "NOT_APPLICABLE",
                        "This process owns no physical connector pool."
                    );
            foreach (var key in new[] { "logicalStreams", "inflight.max" })
                if (primary && (key != "logicalStreams" || !csClient))
                    metrics["load." + key] = DecimalText.Of(
                        key switch
                        {
                            "logicalStreams" => (ulong)config.workload.logicalStreams!.Value,
                            _ => maxInflight,
                        }
                    );
                else
                    MetricCatalog.Null(
                        metrics,
                        reasons,
                        "metrics",
                        "load." + key,
                        "NOT_APPLICABLE",
                        "No server logical streams are owned here; CS slots are connector based."
                    );
            foreach (var direction in new[] { "request", "send", "reply", "event" })
            {
                var count = directional.GetValueOrDefault(direction);
                metrics["applicationMessages." + direction] = DecimalText.Of(count);
                metrics["applicationPayloadBytes." + direction] = DecimalText.Of(
                    checked(count * (ulong)config.workload.payloadSize)
                );
            }
            var seconds = start == 0 ? (double?)null : (end - start) / 1e9;
            var applicationCount = directional.Values.Aggregate(0UL, (a, b) => checked(a + b));
            metrics["throughput.kops"] =
                primary && seconds > 0
                    ? counts.GetValueOrDefault("completed") / seconds.Value / 1000
                    : null;
            metrics["throughput.messagesPerSec"] =
                seconds > 0 ? applicationCount / seconds.Value : null;
            metrics["throughput.megabytesPerSec"] =
                seconds > 0
                    ? applicationCount
                        * (double)config.workload.payloadSize
                        / seconds.Value
                        / 1048576
                    : null;
            metrics["errors.byKind"] = byKind.ToDictionary(
                p => p.Key,
                p => DecimalText.Of(p.Value)
            );
            metrics["errors.harness"] = harness.ToDictionary(
                p => p.Key,
                p => DecimalText.Of(p.Value)
            );
            metrics["errors.language"] = language.ToDictionary(
                p => p.Key,
                p => DecimalText.Of(p.Value)
            );
            foreach (
                var key in new[]
                {
                    "throughput.kops",
                    "throughput.messagesPerSec",
                    "throughput.megabytesPerSec",
                }
            )
                if (metrics[key] is null)
                    reasons["/metrics/" + key] = new(
                        start == 0 ? "PHASE_NOT_STARTED" : "NOT_APPLICABLE",
                        "No applicable completed measurement window."
                    );
            if (phase == "complete")
                sampler.Export(metrics, runtime);
            else
                foreach (
                    var key in new[]
                    {
                        "process.cpuPercent",
                        "process.rssMb",
                        "process.allocatedMb",
                        "gc.gen0",
                        "gc.gen1",
                        "gc.gen2",
                    }
                )
                    MetricCatalog.Null(
                        metrics,
                        reasons,
                        "metrics",
                        key,
                        "PHASE_NOT_STARTED",
                        "Process window sampling has not completed."
                    );
            runtime["setupEvidence"] = new
            {
                name = "setupEvidence",
                unit = "observation",
                type = "array",
                value = SetupEvidence,
            };
            runtime["publicReadinessSamples"] = new
            {
                name = "public host readiness and pressure samples",
                unit = "observation",
                type = "array",
                value = publicStateSamples.ToArray(),
            };
            runtime["errors"] = new
            {
                name = "firstErrors",
                unit = "observation",
                type = "array",
                value = errors.ToArray(),
            };
            runtime["activeHandlers"] = new
            {
                name = "application active handlers",
                unit = "count",
                type = "integer",
                value = DecimalText.Of((ulong)activeHandlers),
            };
            var window = new Window(
                startUnix,
                endUnix,
                start == 0 ? null : DecimalText.Of(start),
                end == 0 ? null : DecimalText.Of(end),
                seconds
            );
            foreach (
                var property in new (string key, object? value)[]
                {
                    ("startedAtUnixMs", window.startedAtUnixMs),
                    ("endedAtUnixMs", window.endedAtUnixMs),
                    ("startTicks", window.startTicks),
                    ("endTicks", window.endTicks),
                    ("measuredSeconds", window.measuredSeconds),
                }
            )
                if (property.value is null)
                    reasons["/window/" + property.key] = new(
                        "PHASE_NOT_STARTED",
                        "Measurement window has not completed."
                    );
            foreach (
                var key in new[]
                {
                    "alignmentMethod",
                    "maxErrorNs",
                    "validFromTicks",
                    "validThroughTicks",
                }
            )
                reasons["/clock/" + key] = new(
                    "NOT_APPLICABLE",
                    "RTT uses the caller process clock only."
                );
            if (publicStatus is null)
                reasons["/publicStatus"] = new(
                    "NOT_APPLICABLE",
                    "The client has no Framework host runtime."
                );
            var serialized = MessageTypes
                .Select(type => new SerializedMessageBytes(
                    type.direction,
                    type.packetName,
                    DecimalText.Of((ulong)config.workload.payloadSize),
                    null
                ))
                .ToArray();
            for (var i = 0; i < serialized.Length; i++)
                reasons[$"/serializedMessageBytes/{i}/observedSerializedBytes"] = new(
                    "PUBLIC_OBSERVATION_UNSUPPORTED",
                    "No public per-DTO serialized byte observation; the measured message is serialized once by the Framework."
                );
            var provenance = new Dictionary<string, object?>(config.provenance)
            {
                ["pid"] = Environment.ProcessId,
                ["host"] = Environment.MachineName,
                ["messageCountScope"] = "application-call-boundaries",
                ["configHash"] = config.configHash,
                ["resetAcknowledgement"] = resetAck,
                ["primaryEchoOwner"] = primary,
                ["runtimeVersion"] = Environment.Version.ToString(),
                ["effectiveProcessorCount"] = Environment.ProcessorCount,
            };
            if (CoreVersion is not null)
                provenance["coreVersion"] = CoreVersion;
            ThreadPool.GetMaxThreads(out var maxWorkerThreads, out var maxCompletionThreads);
            ThreadPool.GetMinThreads(out var minWorkerThreads, out var minCompletionThreads);
            provenance["executor"] = new
            {
                name = ".NET ThreadPool",
                maxWorkerThreads,
                maxCompletionThreads,
                minWorkerThreads,
                minCompletionThreads,
                currentThreadCount = ThreadPool.ThreadCount,
                serverGc = System.Runtime.GCSettings.IsServerGC,
                gcLatencyMode = System.Runtime.GCSettings.LatencyMode.ToString(),
            };
            PerfMetricsSnapshot snapshot = new(
                2,
                config.runId,
                config.cellId,
                resetSeq,
                config.language,
                config.role,
                config.roleInstance,
                config.configHash,
                phase,
                window,
                PerfClock.Metadata,
                serialized,
                metrics,
                histograms,
                reasons,
                publicStatus,
                [],
                runtime,
                provenance
            );
            EnrichSnapshot?.Invoke(snapshot);
            return snapshot;
        }
    }

    public void Dispose() => sampler.Dispose();
}
