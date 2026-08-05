using System.Collections.Concurrent;
using System.Diagnostics;
using System.Diagnostics.Metrics;
using Microsoft.Extensions.Logging;
using SpotActorTransfer.Shared;
using Systems.Zlink;
using Zlink.Framework.Contracts.Dispatch;

namespace SpotActorTransfer.ActorNode;

using Zlink.Framework.E2E.Configuration;

internal sealed record ServerOptions(
    string Rid,
    string HttpUrl,
    string RedisEndpoint,
    string RedisKeyPrefix,
    string RouterEndpoint,
    string RouterAdvertiseHost,
    string EvidenceFile,
    string LogDir,
    int RequestTimeoutMilliseconds,
    bool CallerOnly = false,
    bool CrashAtTargetCompleteGate = false)
{
    public static ServerOptions Parse(string[] args, string role)
        => E2eConfiguration.Load<ServerOptions>(args);
}

internal sealed class EvidenceStore(string nodeRid, string path)
{
    private readonly object _fileGate = new();
    private readonly ConcurrentQueue<ActorEvidence> _items = new();

    public string NodeRid { get; } = nodeRid;

    public void Add(string scenario, string actorId, string kind, string value)
    {
        var item = new ActorEvidence(
            scenario,
            actorId,
            kind,
            value,
            NodeRid,
            DateTimeOffset.UtcNow.ToUnixTimeMilliseconds());
        _items.Enqueue(item);
        lock (_fileGate)
        {
            Directory.CreateDirectory(Path.GetDirectoryName(path)!);
            File.AppendAllLines(
                path,
                [$"{item.Scenario}|{item.ActorId}|{item.Kind}|{item.Value}|{item.NodeRid}"]);
        }
    }

    public ActorEvidence[] Snapshot() => _items.ToArray();

    public async ValueTask<ActorEvidence[]> WaitUntilAsync(
        Func<ActorEvidence[], bool> predicate,
        TimeSpan timeout,
        CancellationToken cancellationToken)
    {
        var deadline = DateTimeOffset.UtcNow + timeout;
        while (DateTimeOffset.UtcNow < deadline)
        {
            var snapshot = Snapshot();
            if (predicate(snapshot)) return snapshot;
            await Task.Delay(100, cancellationToken);
        }

        return Snapshot();
    }
}

internal sealed class RuntimeEvidenceStore
{
    private readonly ConcurrentQueue<string> _items = new();

    public void Add(string marker) => _items.Enqueue(marker);

    public async ValueTask<string[]> WaitUntilAsync(
        string[] containsAll,
        TimeSpan timeout,
        CancellationToken cancellationToken)
    {
        var deadline = DateTimeOffset.UtcNow + timeout;
        while (DateTimeOffset.UtcNow < deadline)
        {
            var snapshot = _items.ToArray();
            if (containsAll.All(expected => snapshot.Any(item =>
                    item.Contains(expected, StringComparison.Ordinal))))
                return snapshot;
            await Task.Delay(50, cancellationToken);
        }

        return _items.ToArray();
    }
}

internal sealed class RelocationInterruptionEvidenceStore : IDisposable
{
    private readonly ConcurrentQueue<RelocationInterruptionEvidence> _items =
        new();
    private readonly MeterListener _listener = new();

    internal RelocationInterruptionEvidenceStore()
    {
        _listener.InstrumentPublished = (instrument, listener) =>
        {
            if (instrument.Meter.Name == "zlink.framework"
                && instrument.Name
                == "zlink.relocation.interruption")
                listener.EnableMeasurementEvents(instrument);
        };
        _listener.SetMeasurementEventCallback<double>(
            (_, value, tags, _) =>
            {
                var unitKind = "";
                string? executionMode = null;
                foreach (var tag in tags)
                    if (tag.Key == "unit_kind")
                        unitKind = tag.Value?.ToString() ?? "";
                    else if (tag.Key == "execution_mode")
                        executionMode = tag.Value?.ToString();
                _items.Enqueue(new RelocationInterruptionEvidence(
                    DateTimeOffset.UtcNow.ToUnixTimeMilliseconds(),
                    value,
                    unitKind,
                    executionMode));
            });
        _listener.Start();
    }

    internal async ValueTask<RelocationInterruptionEvidence[]> WaitUntilAsync(
        string unitKind,
        string? executionMode,
        int minimumCount,
        TimeSpan timeout,
        CancellationToken cancellationToken)
    {
        var deadline = DateTimeOffset.UtcNow + timeout;
        while (DateTimeOffset.UtcNow < deadline)
        {
            var snapshot = Snapshot(unitKind, executionMode);
            if (snapshot.Length >= minimumCount) return snapshot;
            await Task.Delay(50, cancellationToken).ConfigureAwait(false);
        }

        return Snapshot(unitKind, executionMode);
    }

    private RelocationInterruptionEvidence[] Snapshot(
        string unitKind,
        string? executionMode) =>
        _items.Where(item => StringComparer.Ordinal.Equals(
                item.UnitKind,
                unitKind)
            && (executionMode is null
                || StringComparer.Ordinal.Equals(
                    item.ExecutionMode,
                    executionMode)))
            .ToArray();

    public void Dispose() => _listener.Dispose();
}

internal sealed class RelocationMessageFlowEvidenceStore : IDisposable
{
    private const string FrameworkActivitySourceName = "Zlink.Framework";
    private readonly string _nodeRid;
    private readonly ConcurrentQueue<RelocationMessageFlowEvidence>
        _items = new();
    private readonly ActivityListener _listener;

    internal RelocationMessageFlowEvidenceStore(string nodeRid)
    {
        _nodeRid = nodeRid;
        _listener = new ActivityListener
        {
            ShouldListenTo = static source =>
                source.Name == FrameworkActivitySourceName,
            Sample = static (
                    ref ActivityCreationOptions<ActivityContext> _) =>
                ActivitySamplingResult.AllData,
            ActivityStopped = Capture
        };
        ActivitySource.AddActivityListener(_listener);
    }

    public RelocationMessageFlowEvidence[] Snapshot() =>
        _items.ToArray();

    public void Dispose() => _listener.Dispose();

    private void Capture(Activity activity)
    {
        if (!StringComparer.Ordinal.Equals(
                activity.OperationName,
                "zlink.message_flow"))
            return;

        var phase = Tag(activity, "phase");
        var surface = NormalizeSurface(Tag(activity, "surface"));
        var messageKind = NormalizeMessageKind(
            Tag(activity, "message_kind"));
        var packetName = Tag(activity, "packet_name");
        if (surface != "spot"
            || messageKind != "request"
            || packetName != nameof(RelocationWorkloadRequest)
            || phase is not ("received" or "replied"))
            return;

        _items.Enqueue(new RelocationMessageFlowEvidence(
            _nodeRid,
            new DateTimeOffset(activity.StartTimeUtc)
                .ToUnixTimeMilliseconds(),
            phase,
            surface,
            messageKind,
            packetName,
            Tag(activity, "spot_id"),
            Tag(activity, "actor_id"),
            Tag(activity, "correlation_id"),
            Tag(activity, "flow_id")));
    }

    private static string? Tag(Activity activity, string name) =>
        activity.GetTagItem(name)?.ToString();

    private static string NormalizeSurface(string? surface) =>
        surface switch
        {
            "SpotRoute" or "SpotSubscription" => "spot",
            "SpotActor" => "actor",
            _ => surface?.ToLowerInvariant() ?? ""
        };

    private static string NormalizeMessageKind(string? messageKind) =>
        messageKind switch
        {
            "Request" or "ActorRequest" => "request",
            "Send" or "ActorSend" => "send",
            _ => messageKind?.ToLowerInvariant() ?? ""
        };
}

internal sealed class ActorHandoffEvidenceLoggerProvider(
    RuntimeEvidenceStore runtimeEvidence,
    EvidenceStore scenarioEvidence) : ILoggerProvider
{
    public ILogger CreateLogger(string categoryName) =>
        string.Equals(
            categoryName,
            "Zlink.Framework.ActorHandoff",
            StringComparison.Ordinal)
        || string.Equals(
            categoryName,
            "Zlink.Framework.Relocation",
            StringComparison.Ordinal)
            ? new ActorHandoffEvidenceLogger(runtimeEvidence, scenarioEvidence)
            : Microsoft.Extensions.Logging.Abstractions.NullLogger.Instance;

    public void Dispose()
    {
    }

    private sealed class ActorHandoffEvidenceLogger(
        RuntimeEvidenceStore runtimeEvidence,
        EvidenceStore scenarioEvidence) : ILogger
    {
        public IDisposable? BeginScope<TState>(TState state) where TState : notnull => null;

        public bool IsEnabled(LogLevel logLevel) => logLevel >= LogLevel.Information;

        public void Log<TState>(LogLevel logLevel, EventId eventId, TState state,
            Exception? exception, Func<TState, Exception?, string> formatter)
        {
            if (!IsEnabled(logLevel))
                return;

            var marker = formatter(state, exception);
            runtimeEvidence.Add(marker);
            const string prefix = "location_committed actor=";
            if (!marker.StartsWith(prefix, StringComparison.Ordinal))
                return;

            var spotSeparator = marker.IndexOf(" spot=", StringComparison.Ordinal);
            if (spotSeparator <= prefix.Length)
                return;

            var actorId = marker[prefix.Length..spotSeparator];
            var spotId = marker[(spotSeparator + " spot=".Length)..];
            scenarioEvidence.Add(
                "runtime",
                actorId,
                "authority_committed",
                spotId);
        }
    }
}

internal sealed class DomainStateStore(string directory)
{
    public void Save(string actorId, int stateVersion)
    {
        Directory.CreateDirectory(directory);
        File.WriteAllText(Path.Combine(directory, $"domain-{actorId}.state"), stateVersion.ToString());
    }

    public int Load(string actorId)
    {
        var path = Path.Combine(directory, $"domain-{actorId}.state");
        return int.Parse(File.ReadAllText(path));
    }
}

internal sealed class JoinedGateStore
{
    private readonly ConcurrentDictionary<string, TaskCompletionSource> _gates = new(StringComparer.Ordinal);

    public Task WaitAsync(string spotId, CancellationToken cancellationToken)
    {
        var gate = _gates.GetOrAdd(
            spotId,
            static _ => new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously));
        return gate.Task.WaitAsync(cancellationToken);
    }

    public bool Release(string spotId)
    {
        var gate = _gates.GetOrAdd(
            spotId,
            static _ => new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously));
        return gate.TrySetResult();
    }
}

internal sealed class TransferGateStore
{
    private readonly ConcurrentDictionary<string, TaskCompletionSource> _gates = new(StringComparer.Ordinal);

    public Task WaitAsync(string actorId, CancellationToken cancellationToken)
    {
        var gate = _gates.GetOrAdd(
            actorId,
            static _ => new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously));
        return gate.Task.WaitAsync(cancellationToken);
    }

    public bool Release(string actorId)
    {
        var gate = _gates.GetOrAdd(
            actorId,
            static _ => new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously));
        return gate.TrySetResult();
    }
}
