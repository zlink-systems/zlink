using Zlink.Framework.Contracts.Errors;
using Systems.Zlink.Stream.Connector.Contracts;

namespace ZLink.Framework.Perf;

// Application cohort accounting only. No socket state, retry, transport polling or completion pump.
public sealed class Measurement(RoleConfig config, bool primary) : IDisposable
{
    private readonly object gate = new();
    private readonly ProcessSampler sampler = new(checked((int)Math.Ceiling(Math.Max(config.workload.durationSeconds, config.workload.warmupSeconds) * 10)) + 2);
    private Histogram latency = new(), settleLatency = new();
    private readonly Dictionary<string, ulong> counts = [];
    private readonly Dictionary<string, ulong> byKind = [], harness = [], language = [];
    private readonly List<object> errors = [];
    private readonly List<object> publicStateSamples = [];
    private readonly Dictionary<string, ulong> directional = [];
    private ulong inflight, maxInflight, sloMet, sloWindowMet;
    private SequenceEvidence sequenceEvidence = new();
    private readonly Dictionary<string, Histogram> auxiliary = [];
    private readonly Dictionary<string, string>[] intervalCounts = Enumerable.Range(0, checked((int)Math.Ceiling(Math.Max(config.workload.durationSeconds, config.workload.warmupSeconds) * 10))).Select(_ => new Dictionary<string, string>()).ToArray();
    private int activeHandlers;
    private long start, end, settledAt;
    private string? startUnix, endUnix;
    private string phase = "setup", resetSeq = "0";
    private bool sealedResults;
    private ResetReply? resetAck;
    private readonly Dictionary<string, PerfTriggerReply> starts = [];
    private Task phaseTask = Task.CompletedTask;
    public RoleConfig Config => config;
    public PayloadPattern Pattern { get; } = new(config.mode is "send" or "send-send" or "publish" ? config.workload.sendPayloadBytes : config.workload.requestPayloadBytes);
    public PayloadPattern ReplyPattern { get; } = new(config.workload.responsePayloadBytes);
    public bool OneWay => config.mode is "send" or "publish";
    public string Phase { get { lock (gate) return phase; } }
    public string ResetSeq { get { lock (gate) return resetSeq; } }
    public long EndTicks { get { lock (gate) return end; } }
    public Task PhaseTask { get { lock (gate) return phaseTask; } }
    public bool CanIssue { get { lock (gate) return !sealedResults && start != 0 && PerfClock.Now < end; } }
    public bool HasErrors { get { lock (gate) return byKind.Count + harness.Count + language.Count != 0; } }
    public object[] ErrorEvidence { get { lock (gate) return errors.ToArray(); } }
    public ulong Connected { get; set; }
    public ulong ConnectionFailures { get; set; }
    public object[] SetupEvidence { get; set; } = [];
    public Func<object>? SamplePublicState { get; set; }

    public PerfEchoRequest Request(int stream, ulong sequence, bool probe = false) => new()
    {
        runId = config.runId, cellId = config.cellId, resetSeq = probe ? "0" : ResetSeq,
        phase = probe || Phase == "warmup" ? "warmup" : "measured", clientId = stream,
        sequence = DecimalText.Of(sequence), correlationId = $"{config.cellId}/{(probe || Phase == "warmup" ? "warmup" : "measured")}/{stream}/{sequence}",
        sentTicks = DecimalText.Of(PerfClock.Now), clockDomainId = PerfClock.Domain,
        returnSpotId = null, returnChannel = null, payload = Pattern.Base64
    };

    public void ValidateRequest(PerfEchoRequest request)
    {
        if (request.phase == "warmup" && ResetSeq != "0") lock (gate) Increment(counts, "lateWarmupMessages");
        if (request.runId != config.runId || request.cellId != config.cellId ||
            request.clientId < 0 || request.phase is not ("warmup" or "measured") ||
                        request.correlationId != $"{request.cellId}/{request.phase}/{request.clientId}/{request.sequence}" ||
            string.IsNullOrEmpty(request.clockDomainId))
            throw new PerfValidationException("IdentityMismatch", "Request identity does not match the cell.");
        DecimalText.U64(request.sequence);
        DecimalText.I64(request.sentTicks);
        var seq = DecimalText.U64(request.resetSeq);
        if (request.phase == "warmup" ? seq != 0 : seq == 0 || request.resetSeq != ResetSeq)
            throw new PerfValidationException("PhaseMismatch", "Request reset sequence does not match the phase.");
        Pattern.Validate(request.payload);
    }

    public PerfTriggerReply Start(PerfTriggerRequest trigger, Func<Task>? workload)
    {
        lock (gate)
        {
            if (trigger.runId != config.runId || trigger.cellId != config.cellId ||
                trigger.phase is not ("warmup" or "measured") || trigger.resetSeq != resetSeq ||
                (trigger.phase == "warmup" ? resetSeq != "0" : resetSeq == "0"))
                return Ack(trigger, false, "rejected", "Identity, resetSeq or phase is invalid.");
            var key = trigger.phase + "/" + trigger.resetSeq;
            if (starts.TryGetValue(key, out var previous)) return previous with { state = "alreadyStarted" };
            if (!phaseTask.IsCompleted || inflight != 0 || activeHandlers != 0 ||
                (trigger.phase == "warmup" ? phase != "setup" : phase != "reset"))
                return Ack(trigger, false, "rejected", "Previous phase has not drained and reset.");
            phase = trigger.phase;
            sealedResults = false;
            start = PerfClock.Now;
            end = checked(start + (long)((phase == "warmup" ? config.workload.warmupSeconds : config.workload.durationSeconds) * 1e9));
            startUnix = PerfClock.UnixMs;
            sampler.Start(start, end);
            var ack = Ack(trigger, true, "started", null);
            starts.Add(key, ack);
            // Launching on the pool lets the HTTP/control acknowledgement leave before load consumes threads.
            phaseTask = Task.Run(async () => { try { await RunPhase(workload).ConfigureAwait(false); } catch (Exception error) { RecordDiagnostic(error, true); throw; } });
            return ack;
        }
    }
    private PerfTriggerReply Ack(PerfTriggerRequest trigger, bool accepted, string state, string? reason) => new()
    {
        runId = trigger.runId, cellId = trigger.cellId, resetSeq = trigger.resetSeq, phase = trigger.phase,
        accepted = accepted, state = state, reason = reason, configHash = config.configHash
    };
    private async Task RunPhase(Func<Task>? workload)
    {
        Task operations;
        try { operations = workload?.Invoke() ?? Task.CompletedTask; }
        catch (Exception error) { RecordDiagnostic(error, true); operations = Task.CompletedTask; }
        var initialPublicState = SamplePublicState?.Invoke();
        if (initialPublicState is not null) lock (gate) publicStateSamples.Add(initialPublicState);
        while (true)
        {
            var remaining = end - PerfClock.Now;
            if (remaining <= 0) break;
            await Task.Delay(TimeSpan.FromMilliseconds(Math.Min(100, remaining / 1e6))).ConfigureAwait(false);
            // Public runtime status may marshal to a runtime lane; never call it under the counter lock.
            var publicState = SamplePublicState?.Invoke();
            lock (gate)
            {
                sampler.Sample();
                if (publicState is not null) publicStateSamples.Add(publicState);
            }
        }
        lock (gate) { sampler.End(); endUnix = PerfClock.UnixMs; phase = "settle"; }
        try
        {
            var remaining = Math.Max(0, end + config.workload.settleTimeoutMs * 1_000_000L - PerfClock.Now);
            if (!primary && OneWay) await Task.Delay(TimeSpan.FromTicks(remaining / 100)).ConfigureAwait(false);
            else await operations.WaitAsync(TimeSpan.FromTicks(remaining / 100)).ConfigureAwait(false);
        }
        catch (TimeoutException error) { RecordDiagnostic(new PerfValidationException("SettleIncomplete", error.Message), true); }
        catch (Exception error) { RecordDiagnostic(error, true); }
        lock (gate)
        {
            settledAt = PerfClock.Now;
            if (primary)
            {
                counts["unresolved"] = inflight;
                sealedResults = true;
            }
            phase = "complete";
        }
    }
    public ResetReply Reset(ResetRequest request, Func<ulong>? resetCapacity)
    {
        var requested = DecimalText.U64(request.resetSeq);
        lock (gate)
        {
            var drained = phaseTask.IsCompleted && (start == 0 || PerfClock.Now >= end) && inflight == 0 && activeHandlers == 0;
            if (request.runId == config.runId && request.cellId == config.cellId && resetAck?.resetSeq == request.resetSeq && drained)
                return resetAck;
            string? reason = request.runId != config.runId || request.cellId != config.cellId ? "Different run or cell." :
                requested == 0 || requested <= DecimalText.U64(resetSeq) ? "resetSeq must advance." :
                !drained || phase == "setup" ?
                "Warmup or measured operations have not drained." : harness.Count != 0 || counts.GetValueOrDefault("phaseDiagnosticFailures") != 0 || counts.GetValueOrDefault("lateWarmupMessages") != 0 ? "Previous instrumentation or phase failed." : null;
            if (reason is not null) return new(false, request.runId, request.cellId, config.role, config.roleInstance,
                request.resetSeq, PerfClock.UnixMs, null, reason, []);
            counts.Clear(); byKind.Clear(); harness.Clear(); language.Clear(); errors.Clear(); directional.Clear();
            sloMet = sloWindowMet = 0; sequenceEvidence = new(); auxiliary.Clear();
            foreach (var interval in intervalCounts) interval.Clear();
            publicStateSamples.Clear();
            latency = new(); settleLatency = new(); maxInflight = 0;
            start = end = settledAt = 0; startUnix = endUnix = null; sealedResults = false;
            resetSeq = request.resetSeq; phase = "reset";
            var resetAt = PerfClock.UnixMs;
            var epoch = resetCapacity?.Invoke();
            Dictionary<string, NullReason> reasons = [];
            if (epoch is null) reasons["/capacityEpoch"] = new("NOT_APPLICABLE", "The client owns no Framework host.");
            return resetAck = new(true, config.runId, config.cellId, config.role, config.roleInstance,
                resetSeq, resetAt, epoch.HasValue ? DecimalText.Of(epoch.Value) : null, null, reasons);
        }
    }
    public bool BeginOperation(out long started, PerfEchoRequest? request = null)
    {
        lock (gate)
        {
            started = PerfClock.Now;
            if (sealedResults || start == 0 || started >= end) return false;
            Increment(counts, "sent");
            inflight = checked(inflight + 1);
            maxInflight = Math.Max(maxInflight, inflight);
            Increment(directional, OneWay || config.mode == "send-send" ? (config.mode == "publish" ? "event" : "send") : "request");
            RecordInterval("messages.sent", started);
            if (!OneWay) RecordInterval("slo.eligible", started);
            if (request is not null && OneWay) sequenceEvidence.Attempt(request.clientId, DecimalText.U64(request.sequence));
            return true;
        }
    }
    public void CompleteOperation(long started, Exception? error = null, bool recordSlo = true)
    {
        var completed = PerfClock.Now;
        lock (gate)
        {
            if (sealedResults) return;
            inflight--;
            if (error is null)
            {
                var inWindow = completed < end;
                Increment(counts, inWindow ? "completed" : "settleCompleted");
                (inWindow ? latency : settleLatency).Record(completed - started);
                RecordInterval(inWindow ? "messages.completed" : "messages.settleCompleted", completed);
                if (recordSlo && completed - started <= config.workload.applicationDeadlineMs * 1_000_000L)
                { sloMet++; if (inWindow) { sloWindowMet++; RecordInterval("slo.met", completed); } }
            }
            else RecordError(error, true);
        }
    }
    public void HandlerEnter() { lock (gate) activeHandlers++; }
    public void HandlerExit() { lock (gate) activeHandlers--; }
    public void RecordRelay() { lock (gate) if (CanIssue) Increment(directional, "request"); }
    public void RecordReply(PerfEchoRequest request)
    {
        lock (gate)
            if (request.resetSeq == resetSeq && start != 0 && PerfClock.Now < end &&
                request.phase == (resetSeq == "0" ? "warmup" : "measured")) Increment(directional, config.mode == "send-send" ? "send" : "reply");
    }
    public void RecordDiagnostic(Exception error, bool phaseFailure = false) { lock (gate) { if (phaseFailure || phase == "setup") Increment(counts, "phaseDiagnosticFailures"); RecordError(error, false); } }
    private void RecordError(Exception error, bool outcome)
    {
        var category = "failed"; string errorKey;
        if (error is ZLinkFrameworkException framework)
        {
            // The public enum exactly matches the common error kind names and values 0..12.
            Increment(byKind, framework.Kind.ToString()); errorKey = "errors.byKind." + framework.Kind;
            if (framework.Kind == ZLinkFrameworkErrorKind.DeadlineExceeded) category = "timeout";
        }
        else if (error is PerfValidationException validation)
        { errorKey = "errors.harness." + validation.Kind; Increment(harness, validation.Kind); if (validation.Kind == "CorrelationExpired") category = "timeout"; }
        else if (error is ZlinkStreamException connector)
        {
            Increment(language, connector.GetType().FullName!); errorKey = "errors.language." + connector.GetType().FullName;
            if (connector.Error.Code == ZlinkStreamErrorCode.RequestTimeout) category = "timeout";
        }
        else
        {
            Increment(language, error.GetType().FullName ?? error.GetType().Name); errorKey = "errors.language." + (error.GetType().FullName ?? error.GetType().Name);
            if (error is OperationCanceledException) category = "cancelled";
            else if (error is TimeoutException) category = "timeout";
        }
        RecordInterval(errorKey, PerfClock.Now);
        if (outcome) { Increment(counts, category); RecordInterval("messages." + category, PerfClock.Now); }
        if (errors.Count < 32) errors.Add(new { type = error.GetType().FullName, error.Message,
            publicKind = (error as ZLinkFrameworkException)?.Kind.ToString(), harnessKind = (error as PerfValidationException)?.Kind,
            connectorCode = (error as ZlinkStreamException)?.Error.Code.ToString() });
    }
    private static void Increment(Dictionary<string, ulong> values, string key) => values[key] = checked(values.GetValueOrDefault(key) + 1);

    public PerfMetricsSnapshot Snapshot(object? publicStatus)
    {
        lock (gate)
        {
            Dictionary<string, object?> metrics = [], histograms = [], runtime = [];
            Dictionary<string, NullReason> reasons = [];
            MetricCatalog.BaselineNulls(metrics, histograms, reasons);
            foreach (var key in MetricCatalog.Outcomes)
                if (primary) metrics["messages." + key] = DecimalText.Of(key == "unresolved" && !sealedResults ? inflight : counts.GetValueOrDefault(key));
                else MetricCatalog.Null(metrics, reasons, "metrics", "messages." + key, "NOT_APPLICABLE", "Echo outcomes belong to the source process.");
            if (primary)
            {
                latency.Export("latencyMs", "latency", metrics, histograms, reasons);
                settleLatency.Export("settleLatencyMs", "settle.latency", metrics, histograms, reasons);
            }
            else
            {
                foreach (var prefix in new[] { "latency", "settle.latency" })
                    foreach (var suffix in MetricCatalog.LatencySuffixes)
                        MetricCatalog.Null(metrics, reasons, "metrics", prefix + "." + suffix, "NOT_APPLICABLE", "RTT belongs to the source process.");
                foreach (var key in new[] { "latencyMs", "settleLatencyMs" })
                    MetricCatalog.Null(histograms, reasons, "histograms", key, "NOT_APPLICABLE", "RTT belongs to the source process.");
            }
            var csClient = config.role == "client" && config.workload.connections is not null;
            foreach (var key in new[] { "requested", "connected", "failed" })
                if (csClient) metrics["connections." + key] = DecimalText.Of(key switch
                { "requested" => (ulong)(config.workload.connections!.Value / config.workload.clientCount +
                    (config.roleInstance < config.workload.connections.Value % config.workload.clientCount ? 1 : 0)),
                    "connected" => Connected, _ => ConnectionFailures });
                else MetricCatalog.Null(metrics, reasons, "metrics", "connections." + key, "NOT_APPLICABLE", "This process owns no physical connector pool.");
            foreach (var key in new[] { "logicalStreams", "inflightPerStream", "inflight.max" })
                if (primary && (key != "logicalStreams" || !csClient)) metrics["load." + key] = DecimalText.Of(key switch
                { "logicalStreams" => (ulong)config.workload.logicalStreams!.Value,
                    "inflightPerStream" => (ulong)config.workload.inflight, _ => maxInflight });
                else MetricCatalog.Null(metrics, reasons, "metrics", "load." + key, "NOT_APPLICABLE", "No server logical streams are owned here; CS slots are connector based.");
            foreach (var direction in new[] { "request", "send", "reply", "event" })
            {
                var count = directional.GetValueOrDefault(direction);
                metrics["applicationMessages." + direction] = DecimalText.Of(count);
                metrics["applicationPayloadBytes." + direction] = DecimalText.Of(checked(count * (ulong)PayloadBytes(direction)));
            }
            var seconds = start == 0 ? (double?)null : (end - start) / 1e9;
            var applicationCount = directional.Values.Aggregate(0UL, (a, b) => checked(a + b));
            metrics["throughput.kops"] = primary && seconds > 0 ? counts.GetValueOrDefault("completed") / seconds.Value / 1000 : null;
            metrics["throughput.messagesPerSec"] = seconds > 0 ? applicationCount / seconds.Value : null;
            metrics["throughput.megabytesPerSec"] = seconds > 0 ? directional.Sum(p => p.Value * (double)PayloadBytes(p.Key)) / seconds.Value / 1048576 : null;
            metrics["errors.byKind"] = byKind.ToDictionary(p => p.Key, p => DecimalText.Of(p.Value));
            metrics["errors.harness"] = harness.ToDictionary(p => p.Key, p => DecimalText.Of(p.Value));
            metrics["errors.language"] = language.ToDictionary(p => p.Key, p => DecimalText.Of(p.Value));
            foreach (var key in new[] { "throughput.kops", "throughput.messagesPerSec", "throughput.megabytesPerSec" })
                if (metrics[key] is null) reasons["/metrics/" + key] = new(start == 0 ? "PHASE_NOT_STARTED" : "NOT_APPLICABLE", "No applicable completed measurement window.");
            if (phase == "complete") sampler.Export(metrics, runtime);
            else foreach (var key in new[] { "process.cpuPercent", "process.rssMb", "process.allocatedMb", "gc.gen0", "gc.gen1", "gc.gen2" })
                MetricCatalog.Null(metrics, reasons, "metrics", key, "PHASE_NOT_STARTED", "Process window sampling has not completed.");
            runtime["setupEvidence"] = new { name = "setupEvidence", unit = "observation", type = "array", value = SetupEvidence.Where(item => item is not null).ToArray() };
            runtime["publicReadinessSamples"] = new { name = "public host readiness and pressure samples", unit = "observation", type = "array", value = publicStateSamples.ToArray() };
            runtime["errors"] = new { name = "firstErrors", unit = "observation", type = "array", value = errors.ToArray() };
            runtime["phaseDiagnosticFailures"] = new { name = "phaseDiagnosticFailures", unit = "count", type = "integer", value = DecimalText.Of(counts.GetValueOrDefault("phaseDiagnosticFailures")) };
            runtime["activeHandlers"] = new { name = "application active handlers", unit = "count", type = "integer", value = DecimalText.Of((ulong)activeHandlers) };
            var window = new Window(startUnix, endUnix, start == 0 ? null : DecimalText.Of(start),
                end == 0 ? null : DecimalText.Of(end), seconds, settledAt == 0 ? null : Math.Max(0, settledAt - end) / 1e9);
            foreach (var property in new (string key, object? value)[] { ("startedAtUnixMs", window.startedAtUnixMs),
                ("endedAtUnixMs", window.endedAtUnixMs), ("startTicks", window.startTicks), ("endTicks", window.endTicks),
                ("measuredSeconds", window.measuredSeconds), ("settleSeconds", window.settleSeconds) })
                if (property.value is null) reasons["/window/" + property.key] = new("PHASE_NOT_STARTED", "Window or settle has not completed.");
            foreach (var key in new[] { "alignmentMethod", "maxErrorNs", "validFromTicks", "validThroughTicks" })
                reasons["/clock/" + key] = new("NOT_APPLICABLE", "RTT uses the caller process clock only.");
            if (publicStatus is null) reasons["/publicStatus"] = new("NOT_APPLICABLE", "The client has no Framework host runtime.");
            SerializedMessageBytes[] serialized = config.mode == "publish"
                ? [new("event", nameof(PerfPublishEvent), DecimalText.Of((ulong)config.workload.sendPayloadBytes), null)]
                : config.mode == "send"
                    ? [new("send", nameof(PerfEchoRequest), DecimalText.Of((ulong)config.workload.sendPayloadBytes), null)]
                    : [new(config.mode == "send-send" ? "send" : "request", nameof(PerfEchoRequest),
                        DecimalText.Of((ulong)(config.mode == "send-send" ? config.workload.sendPayloadBytes : config.workload.requestPayloadBytes)), null),
                       new("reply", nameof(PerfEchoReply), DecimalText.Of((ulong)config.workload.responsePayloadBytes), null)];
            for (var i = 0; i < serialized.Length; i++) reasons[$"/serializedMessageBytes/{i}/observedSerializedBytes"] =
                new("PUBLIC_OBSERVATION_UNSUPPORTED", "No public per-DTO serialized byte observation; the measured message is serialized once by the Framework.");
            var provenance = new Dictionary<string, object?>(config.provenance)
            { ["pid"] = Environment.ProcessId, ["host"] = Environment.MachineName,
                ["messageCountScope"] = "application-call-boundaries", ["configHash"] = config.configHash,
                ["resetAcknowledgement"] = resetAck, ["primaryEchoOwner"] = primary, ["receiptCountScope"] = "raw receiver unique ranges; coordinator applies source-window admission intersection",
                ["runtimeVersion"] = Environment.Version.ToString(), ["effectiveProcessorCount"] = Environment.ProcessorCount };
            ThreadPool.GetMaxThreads(out var maxWorkerThreads, out var maxCompletionThreads);
            ThreadPool.GetMinThreads(out var minWorkerThreads, out var minCompletionThreads);
            provenance["workerOptions"] = new { minThreads = config.workload.workerPoolSize, maxThreads = config.workload.workerPoolSize, idleTimeoutMs = 60000, maxQueueLength = (int?)null, reason = "IZLinkWorkerOptions exposes no MaxQueueLength; no runtime bypass added." };
            provenance["executor"] = new { name = ".NET ThreadPool", maxWorkerThreads, maxCompletionThreads,
                minWorkerThreads, minCompletionThreads, currentThreadCount = ThreadPool.ThreadCount,
                serverGc = System.Runtime.GCSettings.IsServerGC, gcLatencyMode = System.Runtime.GCSettings.LatencyMode.ToString() };
            ExportScenario(metrics, histograms, reasons, runtime, seconds);
            provenance["evidenceRetention"] = new { method = "numeric HashSet per stream; maximal ranges at snapshot", retainedNumericBytes = DecimalText.Of((ulong)sequenceEvidence.RetainedBytes) };
            return new(3, config.runId, config.cellId, resetSeq, "dotnet", config.role, config.roleInstance,
                config.configHash, phase, window, PerfClock.Metadata, serialized, metrics, histograms,
                reasons, publicStatus, [], runtime, provenance,
                config.provenance.TryGetValue("comparisonKey", out var comparison) ? comparison?.ToString() ?? config.configHash : config.configHash, ExportTimeSeries());
        }
    }

    private int PayloadBytes(string direction) => direction switch { "request" => config.workload.requestPayloadBytes,
        "reply" => config.workload.responsePayloadBytes, _ => config.workload.sendPayloadBytes };
    public void Count(string key) { lock (gate) Increment(counts, key); }
    public void RecordInterval(string key, long ticks)
    {
        if (start == 0 || ticks < start || ticks >= end) return;
        var index = (int)((ticks - start) / 100_000_000);
        if (index >= intervalCounts.Length) return;
        var values = intervalCounts[index];
        values[key] = DecimalText.Of(checked(DecimalText.U64(values.GetValueOrDefault(key, "0")) + 1));
    }
    public void RecordAuxiliary(string key, long elapsed, long completed)
    {
        lock (gate) if (start != 0 && completed < end && completed >= start)
        { if (!auxiliary.TryGetValue(key, out var histogram)) auxiliary.Add(key, histogram = new()); histogram.Record(elapsed); }
    }
    public void CompleteAdmission(PerfEchoRequest request, long started, Exception? error = null)
    {
        var completed = PerfClock.Now;
        lock (gate)
        {
            if (sealedResults) return;
            inflight--;
            if (error is not null) { RecordError(error, true); return; }
            var inWindow = completed < end;
            var key = config.mode == "publish" ? "published" : "admitted";
            Increment(counts, key);
            Increment(counts, config.mode == "publish" ? (inWindow ? "publishedInWindow" : "settlePublished") : (inWindow ? "admittedInWindow" : "settleAdmitted"));
            sequenceEvidence.Admission(request.clientId, DecimalText.U64(request.sequence), inWindow);
            if (inWindow) { RecordInterval("messages." + key, completed); if (config.mode == "send") RecordAuxiliary("sendAdmissionLatencyMs", completed - started, completed); }
        }
    }
    public void RecordSloSuccess(long started)
    {
        var completed = PerfClock.Now;
        lock (gate) if (!sealedResults && completed - started <= config.workload.applicationDeadlineMs * 1_000_000L)
        { sloMet++; if (completed < end) { sloWindowMet++; RecordInterval("slo.met", completed); } }
    }
    public void RecordInitialAdmission(long started, bool actor)
    {
        lock (gate) Increment(counts, "admitted");
        if (actor) RecordAuxiliary("sourceAdmissionMs", PerfClock.Now - started, PerfClock.Now);
    }
    public void RecordReceipt(PerfEchoRequest request, long received)
    {
        lock (gate)
        {
            if (request.phase == "warmup" && resetSeq != "0") { Increment(counts, "lateWarmupMessages"); return; }
            ValidateRequest(request);
            if (request.phase != "measured" || start == 0 || received < start) return;
            if (sealedResults || received >= end + config.workload.settleTimeoutMs * 1_000_000L)
            { Increment(counts, "outOfCohortEvents"); return; }
            var inWindow = received < end;
            if (!sequenceEvidence.Receipt(request.clientId, DecimalText.U64(request.sequence), inWindow)) return;
            Increment(counts, inWindow ? "deliveredInWindow" : "settleDelivered");
            RecordInterval("messages.delivered", received);
        }
    }
    public void RecordPublishReceipt(PerfPublishEvent message, long received)
    {
        RecordReceipt(new PerfEchoRequest { runId = message.runId, cellId = message.cellId, resetSeq = message.resetSeq,
            phase = message.phase, clientId = 0, sequence = message.sequence,
            correlationId = $"{message.cellId}/{message.phase}/0/{message.sequence}", sentTicks = message.sentTicks,
            clockDomainId = message.clockDomainId, returnSpotId = null, returnChannel = null, payload = message.payload }, received);
    }
    private void ExportScenario(Dictionary<string, object?> metrics, Dictionary<string, object?> histograms,
        Dictionary<string, NullReason> reasons, Dictionary<string, object?> runtime, double? seconds)
    {
        void Set(string key, object? value) { metrics[key] = value; if (value is not null) reasons.Remove("/metrics/" + key); else reasons.TryAdd("/metrics/" + key, new("PHASE_NOT_STARTED", "No measurement window has started.")); }
        foreach (var key in new[] { "expired", "duplicateReply", "lateReply", "unknownCorrelation", "admitted" })
            if (config.mode == "send-send") Set("messages." + key, DecimalText.Of(counts.GetValueOrDefault(key)));
        Set("load.lateWarmupMessages", DecimalText.Of(counts.GetValueOrDefault("lateWarmupMessages")));
        if (primary && !OneWay)
        {
            var eligible = counts.GetValueOrDefault("sent");
            Set("slo.eligible", DecimalText.Of(eligible)); Set("slo.met", DecimalText.Of(sloMet));
            Set("slo.missed", DecimalText.Of(eligible - sloMet));
            Set("slo.missRatio", eligible == 0 ? null : (eligible - sloMet) / (double)eligible);
            if (eligible == 0) reasons["/metrics/slo.missRatio"] = new("ZERO_DENOMINATOR", "No operations were started.");
            Set("slo.goodputOpsPerSec", seconds > 0 ? sloWindowMet / seconds.Value : null);
            if (seconds is null) reasons["/metrics/slo.goodputOpsPerSec"] = new("PHASE_NOT_STARTED", "Window has not started.");
        }
        else foreach (var key in new[] { "eligible", "met", "missed", "missRatio", "goodputOpsPerSec" })
            MetricCatalog.Null(metrics, reasons, "metrics", "slo." + key, OneWay ? "CLOCK_DOMAIN_UNVERIFIED" : "NOT_APPLICABLE",
                OneWay ? "Delivery SLO requires validated shared clocks; source admission is not delivery." : "Source owns the SLO.");
        if (OneWay)
        {
            foreach (var key in new[] { "completed", "settleCompleted" }) MetricCatalog.Null(metrics, reasons, "metrics", "messages." + key, "NOT_APPLICABLE", "One-way admission is not echo completion.");
            MetricCatalog.Null(metrics, reasons, "metrics", "throughput.kops", "NOT_APPLICABLE", "One-way throughput uses admission and delivery rates.");
            foreach (var prefix in new[] { "latency", "settle.latency" }) foreach (var suffix in MetricCatalog.LatencySuffixes)
                MetricCatalog.Null(metrics, reasons, "metrics", prefix + "." + suffix, "NOT_APPLICABLE", "No echo RTT.");
            foreach (var key in new[] { "latencyMs", "settleLatencyMs" }) MetricCatalog.Null(histograms, reasons, "histograms", key, "NOT_APPLICABLE", "No echo RTT.");
            var publish = config.mode == "publish";
            if (publish) Set("fanout.subscriberCount", DecimalText.Of((ulong)config.workload.subscriberCount));
            var prefixRate = publish ? "fanout" : "send";
            foreach (var prefix in publish ? new[] { "fanout.deliveryLatency", "fanout.settleDeliveryLatency" } : new[] { "send.deliveryLatency", "send.settleDeliveryLatency" })
                foreach (var suffix in MetricCatalog.LatencySuffixes) MetricCatalog.Null(metrics, reasons, "metrics", prefix + "." + suffix, "CLOCK_DOMAIN_UNVERIFIED", "Separate process clocks are not aligned.");
            foreach (var key in publish ? new[] { "fanoutDeliveryLatencyMs", "fanoutSettleDeliveryLatencyMs" } : new[] { "sendDeliveryLatencyMs", "sendSettleDeliveryLatencyMs" })
                MetricCatalog.Null(histograms, reasons, "histograms", key, "CLOCK_DOMAIN_UNVERIFIED", "Separate process clocks are not aligned.");
            if (primary)
            {
                foreach (var key in publish ? new[] { "published", "publishedInWindow", "settlePublished" } : new[] { "admitted", "admittedInWindow", "settleAdmitted" }) Set("messages." + key, DecimalText.Of(counts.GetValueOrDefault(key)));
                Set(prefixRate + (publish ? ".publishOpsPerSec" : ".admissionOpsPerSec"), seconds > 0 ? counts.GetValueOrDefault(publish ? "publishedInWindow" : "admittedInWindow") / seconds.Value : null);
                runtime[publish ? "publisherSequences" : "sourceEvidence"] = new { name = "measured source sequence evidence", unit = "sequence", type = "object", value = publish ? sequenceEvidence.Publisher(config.runId, config.cellId, resetSeq) : sequenceEvidence.SendSources() };
            }
            else
            {
                Set(prefixRate + ".uniqueDelivered", DecimalText.Of(sequenceEvidence.Unique));
                Set(prefixRate + ".duplicateEvents", DecimalText.Of(sequenceEvidence.Duplicates));
                Set(prefixRate + ".deliveredInWindow", DecimalText.Of(counts.GetValueOrDefault("deliveredInWindow")));
                Set(prefixRate + ".settleDelivered", DecimalText.Of(counts.GetValueOrDefault("settleDelivered")));
                Set(prefixRate + ".deliveryOpsPerSec", seconds > 0 ? counts.GetValueOrDefault("deliveredInWindow") / seconds.Value : null);
                runtime[publish ? "subscriberSequences" : "deliveryEvidence"] = new { name = "measured receipt sequence evidence", unit = "sequence", type = "object", value = publish ? sequenceEvidence.Subscriber(config.roleInstance, config.runId, config.cellId, resetSeq) : sequenceEvidence.SendReceivers() };
            }
            MetricCatalog.Null(metrics, reasons, "metrics", prefixRate + ".deliveryRatio", "MULTIPLE_OWNERS", "Coordinator intersects source window admissions with receiver unique ranges.");
        }
        foreach (var (key, prefix) in new[] { ("sourceAdmissionMs", "actor.sourceAdmission.latency"), ("driverLatencyMs", "driver.latency"),
            ("workerCallLatencyMs", "worker.callLatency"), ("workerSubmitToStartMs", "worker.submitToStart"),
            ("workerTaskLatencyMs", "worker.taskLatency"), ("workerResultToContinuationMs", "worker.resultToContinuation"),
            ("sendAdmissionLatencyMs", "send.admissionLatency") })
        {
            var applicable = key switch
            {
                "sourceAdmissionMs" => primary && config.role == "actorCaller" && config.mode == "send-send",
                "driverLatencyMs" => primary && config.role == "spot" && config.scenario.StartsWith("s2s-spot", StringComparison.Ordinal),
                "sendAdmissionLatencyMs" => primary && config.mode == "send",
                _ => config.scenario == "spot-worker-offload-echo" && config.role == "spot"
            };
            if (applicable) (auxiliary.GetValueOrDefault(key) ?? new Histogram()).Export(key, prefix, metrics, histograms, reasons);
        }
        if (config.role == "spot")
        {
            foreach (var key in new[] { "spot.mailboxDepth.max", "spot.mailboxDepth.mean", "spot.suspendedTurns", "spot.resumedTurns", "spot.resumeLatency.p95Ms", "spot.resumeLatency.p99Ms" })
                MetricCatalog.Null(metrics, reasons, "metrics", key, "PUBLIC_OBSERVATION_UNSUPPORTED", "Public Framework status does not expose per-Spot mailbox or turn transitions.");
            foreach (var key in new[] { "applicationYieldCalls", "applicationHandlerEntries" }) Set("spot." + key, DecimalText.Of(counts.GetValueOrDefault(key)));
            foreach (var key in new[] { "issued", "notStarted", "failed" }) Set("driver." + key, DecimalText.Of(counts.GetValueOrDefault("driver." + key)));
            if (config.scenario == "s2s-spot-to-channel-request-echo") foreach (var suffix in MetricCatalog.LatencySuffixes)
            { Set("spot.remoteCallLatency." + suffix, metrics["latency." + suffix]); if (metrics["latency." + suffix] is null) reasons["/metrics/spot.remoteCallLatency." + suffix] = reasons["/metrics/latency." + suffix]; }
        }
    }
    private TimeSeriesInterval[] ExportTimeSeries()
    {
        if (start == 0) return [];
        return intervalCounts.Select((values, index) => new TimeSeriesInterval(index * 100, Math.Min(100, (end - start) / 1e6 - index * 100),
            new(values), sampler.BinCpu(index), sampler.BinCpu(index) is null ? new() { ["/cpuPercent"] = new("NO_SAMPLES", "No actual CPU sample span starts in this bin.") } : new())).Where(i => i.durationMs > 0).ToArray();
    }
    public void Dispose() => sampler.Dispose();
}
