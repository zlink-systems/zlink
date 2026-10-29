using System.Diagnostics;
using Microsoft.Extensions.Logging;

namespace Zlink.Framework.Runtime.Diagnostics;

// Host-owned facade; queued records retain the generation lease, never this facade or runtime.
internal sealed class ZLinkTelemetrySubmission : IDisposable
{
    private ZLinkTelemetryLease _current = new();
    internal ZLinkTelemetryLease Current => Volatile.Read(ref _current);

    internal void Renew()
    {
        if (Current.Retired)
            Volatile.Write(ref _current, new ZLinkTelemetryLease());
    }

    public void Dispose() => ZLinkMessageFlowDelivery.RetireSubmission(Current);
}

internal sealed class ZLinkTelemetryLease
{
    internal volatile bool Retired;
}

// One process owner. Provider callbacks never execute under the admission lock.
internal sealed class ZLinkMessageFlowDelivery
{
    internal const int Capacity = 8192;
    private static readonly Lazy<ZLinkMessageFlowDelivery> ProcessOwner = new(() => new());
    internal static ZLinkMessageFlowDelivery Shared => ProcessOwner.Value;
    internal static bool IsCreated => ProcessOwner.IsValueCreated;

    internal static void RetireSubmission(ZLinkTelemetryLease lease)
    {
        // Closing acceptance precedes checking the lazy owner: concurrent initialization
        // may finish, but its first submission must observe this retired lease.
        lease.Retired = true;
        if (ProcessOwner.IsValueCreated)
            ProcessOwner.Value.DiscardRetired(lease);
    }

    private readonly object _gate = new();
    private readonly Queue<ZLinkTelemetryRecord> _pending = new(Capacity);
    private readonly long[] _failures = new long[(int)ZLinkTelemetryProviderRole.Fallback + 1];
    private bool _executing;
    private long _overflow;
    private long _reportedOverflow;
    private long _wakeCount;

    private ZLinkMessageFlowDelivery()
    {
        var worker = new Thread(Run) { IsBackground = true, Name = "ZLink message-flow delivery" };
        if (ExecutionContext.IsFlowSuppressed())
            worker.Start();
        else
        {
            using (ExecutionContext.SuppressFlow())
                worker.Start();
        }
    }

    internal (int Pending, bool Executing, long Overflow, long Wakes) State
    {
        get
        {
            lock (_gate)
                return (_pending.Count, _executing, _overflow, _wakeCount);
        }
    }

    internal long FailureCount(ZLinkTelemetryProviderRole role) =>
        Volatile.Read(ref _failures[(int)role]);

    internal void Submit(ZLinkTelemetryRecord record)
    {
        lock (_gate)
        {
            if (record.Lease.Retired)
                return;
            if (_pending.Count == Capacity)
            {
                ++_overflow;
                return;
            }
            var wake = _pending.Count == 0 && !_executing;
            _pending.Enqueue(record);
            if (wake)
            {
                ++_wakeCount;
                Monitor.Pulse(_gate);
            }
        }
    }

    private void DiscardRetired(ZLinkTelemetryLease lease)
    {
        lock (_gate)
        {
            var count = _pending.Count;
            while (count-- != 0)
            {
                var record = _pending.Dequeue();
                if (!ReferenceEquals(record.Lease, lease))
                    _pending.Enqueue(record);
            }
        }
    }

    private void Run()
    {
        while (true)
        {
            ZLinkTelemetryRecord record;
            lock (_gate)
            {
                _executing = false;
                while (_pending.Count == 0)
                    Monitor.Wait(_gate);
                record = _pending.Dequeue();
                _executing = true;
            }
            Deliver(record);
            long overflow;
            lock (_gate)
            {
                overflow = _overflow - _reportedOverflow;
                _reportedOverflow = _overflow;
            }
            if (overflow != 0)
            {
                try
                {
                    ZLinkRuntimeMetrics.RecordMessageFlowOverflow(overflow);
                }
                catch (Exception error)
                {
                    Failed(ZLinkTelemetryProviderRole.Trace, error);
                }
            }
        }
    }

    private void Deliver(ZLinkTelemetryRecord record)
    {
        try
        {
            var level = record.Error is { } error
                ? error.Reason == ZLinkDispatchErrorReason.HandlerException
                    ? LogLevel.Error
                    : LogLevel.Warning
                : ZLinkTraceFormat.ResolveLogLevel(record.Flow!);
            if (record.Logger.IsEnabled(level))
            {
                var fields = record.Error is { } failure
                    ? ZLinkTraceFormat.StructuredFields(
                        failure,
                        failure.Details,
                        failure.FlowId,
                        failure.FlowOrigin
                    )
                    : ZLinkTraceFormat.StructuredFields(record.Flow!, record.Flow!.MessageSize);
                record.Logger.Log(
                    level,
                    default,
                    new ZLinkStructuredLogState(fields),
                    null,
                    static (state, _) => state.ToString()
                );
            }
        }
        catch (Exception error)
        {
            Failed(ZLinkTelemetryProviderRole.Logger, error);
        }
        if (record.Source is null)
            return;
        try
        {
            if (record.Error is { } failure)
                ZLinkTelemetry.TraceDispatchError(
                    failure,
                    failure.Details,
                    failure.FlowId,
                    failure.FlowOrigin,
                    record.Source,
                    record.Parent
                );
            else
                ZLinkTelemetry.TraceMessageFlow(record.Flow!, record.Source, record.Parent);
        }
        catch (Exception error)
        {
            Failed(ZLinkTelemetryProviderRole.Trace, error);
        }
    }

    private void Failed(ZLinkTelemetryProviderRole role, Exception error)
    {
        if (
            Interlocked.Increment(ref _failures[(int)role]) != 1
            || FailureCount(ZLinkTelemetryProviderRole.Fallback) != 0
        )
            return;
        try
        {
            Console.Error.WriteLine(
                "ZLink telemetry {0} provider failed: {1}: {2}",
                role,
                error.GetType().Name,
                error.Message
            );
        }
        catch
        {
            Interlocked.Increment(ref _failures[(int)ZLinkTelemetryProviderRole.Fallback]);
        }
    }
}

internal enum ZLinkTelemetryProviderRole
{
    Logger,
    Trace,
    Fallback,
}

internal sealed record ZLinkTelemetryRecord(
    ZLinkTelemetryLease Lease,
    ILogger Logger,
    ActivitySource? Source,
    ActivityContext Parent,
    ZLinkMessageFlowEvent? Flow = null,
    ZLinkDispatchErrorSnapshot? Error = null
);
