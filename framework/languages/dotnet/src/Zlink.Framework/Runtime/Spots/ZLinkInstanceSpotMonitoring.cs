using System.Collections.Concurrent;
using Zlink.Framework.Runtime.Diagnostics;

namespace Zlink.Framework.Runtime.Spots;

internal sealed class ZLinkInstanceSpotMonitoring
{
    private readonly ConcurrentDictionary<string, Operation> _operations =
        new(StringComparer.Ordinal);
    private readonly ConcurrentDictionary<string, Aggregate> _aggregates =
        new(StringComparer.Ordinal);

    internal async Task<InstanceSpotActivationTerminal> ObserveAsync(
        string operationKey,
        string meshName,
        string stableType,
        ulong pendingBytes,
        Func<Task<InstanceSpotActivationTerminal>> operation)
    {
        var selected = _operations.GetOrAdd(
            operationKey,
            _ => new Operation(
                stableType,
                ZLinkRuntimeMetrics.StartInstanceSpotActivation(
                    meshName,
                    stableType)));
        if (!StringComparer.Ordinal.Equals(selected.StableType, stableType))
            throw new InvalidOperationException(
                $"Instance operation '{operationKey}' changed stable type.");

        var aggregate = _aggregates.GetOrAdd(
            stableType,
            static _ => new Aggregate());
        if (selected.TryAccept())
            aggregate.Accept(pendingBytes);
        try
        {
            var terminal = await operation().ConfigureAwait(false);
            if (_operations.TryRemove(
                    new KeyValuePair<string, Operation>(
                        operationKey,
                        selected)))
            {
                var outcome = terminal.Result == RequestResult.Ok
                    ? "ready"
                    : "rejected";
                aggregate.Complete(
                    pendingBytes,
                    outcome);
                selected.Metrics.Complete(outcome);
            }
            return terminal;
        }
        catch (Exception exception)
        {
            if (_operations.TryRemove(
                    new KeyValuePair<string, Operation>(
                        operationKey,
                        selected)))
            {
                var outcome = Outcome(exception);
                aggregate.Complete(
                    pendingBytes,
                    outcome);
                selected.Metrics.Complete(outcome);
            }
            throw;
        }
    }

    internal ZLinkInstanceSpotOperationSnapshot Snapshot(string stableType) =>
        _aggregates.TryGetValue(stableType, out var aggregate)
            ? aggregate.Snapshot()
            : default;

    private static string Outcome(Exception exception) =>
        exception switch
        {
            TimeoutException => "timed_out",
            OperationCanceledException => "shutdown",
            ZLinkFrameworkException
            {
                Kind: ZLinkFrameworkErrorKind.DeadlineExceeded
            } => "timed_out",
            ZLinkFrameworkException
            {
                Kind: ZLinkFrameworkErrorKind.ShuttingDown
            } => "shutdown",
            ZLinkFrameworkException
            {
                Kind: ZLinkFrameworkErrorKind.InvalidOperation
            } => "fenced",
            ZLinkFrameworkException
            {
                Kind: ZLinkFrameworkErrorKind.AlreadyExists
            } => "conflict",
            ZLinkFrameworkException
            {
                Kind: ZLinkFrameworkErrorKind.InternalFailure
                or ZLinkFrameworkErrorKind.NotFound
            } => "store_failure",
            _ => "rejected"
        };

    private sealed class Operation(
        string stableType,
        ZLinkRuntimeMetrics.ZLinkInstanceSpotMetricOperation metrics)
    {
        private int _accepted;

        internal string StableType { get; } = stableType;
        internal ZLinkRuntimeMetrics.ZLinkInstanceSpotMetricOperation Metrics { get; } = metrics;

        internal bool TryAccept() =>
            Interlocked.Exchange(ref _accepted, 1) == 0;
    }

    private sealed class Aggregate
    {
        private readonly object _gate = new();
        private ulong _pendingMessages;
        private ulong _pendingBytes;
        private string? _lastOutcome;

        internal void Accept(ulong bytes)
        {
            lock (_gate)
            {
                _pendingMessages = checked(_pendingMessages + 1);
                _pendingBytes = checked(_pendingBytes + bytes);
            }
        }

        internal void Complete(ulong bytes, string outcome)
        {
            lock (_gate)
            {
                if (_pendingMessages == 0 || _pendingBytes < bytes)
                    throw new InvalidOperationException(
                        "Instance monitoring counters became inconsistent.");
                _pendingMessages--;
                _pendingBytes -= bytes;
                _lastOutcome = outcome;
            }
        }

        internal ZLinkInstanceSpotOperationSnapshot Snapshot()
        {
            lock (_gate)
                return new ZLinkInstanceSpotOperationSnapshot(
                    _pendingMessages,
                    _pendingBytes,
                    _lastOutcome);
        }
    }
}

internal readonly record struct ZLinkInstanceSpotOperationSnapshot(
    ulong PendingMessageCount,
    ulong PendingByteCount,
    string? LastActivationOutcome);

internal readonly record struct ZLinkInstanceSpotCatalogSnapshot(
    ulong ActiveCount,
    ulong ActivatingCount,
    ulong ClosingCount);
