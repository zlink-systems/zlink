using Zlink.Framework.Runtime.Backend.Contracts;
using Zlink.Framework.Runtime.Diagnostics;

namespace Zlink.Framework.Runtime.Backend.DotNet;

// RouteMesh 10.0.0 request/reply bridge.
//
// 9.x request operations took a callback and the binding drove it when the reply
// arrived. 10.0.0 requests are pull dispatch: RequestTo*/Join*/Destroy* return
// a SubmitResult plus an out MeshOperationId, and the reply/terminal outcome is
// delivered later as a Completion receive record. This table holds a pending
// record-aware completion handler keyed by MeshOperationId; the node dispatch
// pump resolves the entry when the matching Completion record drains.
internal sealed class ZLinkMeshCompletionTable
{
    internal delegate void CompletionHandler(
        MeshReceiveRecord record, IReadOnlyList<Message> parts);

    private const int DefaultEarlyCompletionCount = 4096;
    private const long DefaultEarlyCompletionBytes = 16L * 1024 * 1024;
    private const int DefaultTombstoneCount = 4096;
    private const long DefaultTombstoneBytes = 256L * 1024;
    private const long TombstoneRecordBytes = 32;
    private const int CompletionRecordHeaderBytes = sizeof(int) + sizeof(long);

    private readonly object _gate = new();
    private readonly Dictionary<MeshOperationId, CompletionHandler> _pending = new();
    private readonly Dictionary<MeshOperationId, EarlyCompletion> _early = new();
    private readonly Dictionary<MeshOperationId, Tombstone> _tombstones = new();
    private readonly int _earlyCompletionCount;
    private readonly long _earlyCompletionBytes;
    private readonly int _tombstoneCount;
    private readonly long _tombstoneBytesLimit;
    private readonly string _meshName;
    private long _earlyBytes;
    private long _tombstoneBytes;
    private long _overflowCount;

    internal ZLinkMeshCompletionTable(
        int earlyCompletionCount = DefaultEarlyCompletionCount,
        long earlyCompletionBytes = DefaultEarlyCompletionBytes,
        int tombstoneCount = DefaultTombstoneCount,
        long tombstoneBytes = DefaultTombstoneBytes,
        string? meshName = null)
    {
        if (earlyCompletionCount <= 0)
            throw new ArgumentOutOfRangeException(nameof(earlyCompletionCount));
        if (earlyCompletionBytes <= 0)
            throw new ArgumentOutOfRangeException(nameof(earlyCompletionBytes));
        if (tombstoneCount <= 0)
            throw new ArgumentOutOfRangeException(nameof(tombstoneCount));
        if (tombstoneBytes < TombstoneRecordBytes)
            throw new ArgumentOutOfRangeException(nameof(tombstoneBytes));
        _earlyCompletionCount = earlyCompletionCount;
        _earlyCompletionBytes = earlyCompletionBytes;
        _tombstoneCount = tombstoneCount;
        _tombstoneBytesLimit = tombstoneBytes;
        _meshName = string.IsNullOrWhiteSpace(meshName)
            ? "unknown"
            : meshName;
    }

    // A caller that registers after both bounded retention stores are full
    // cannot be given the original reply. Keep this condition observable so
    // the runtime does not turn a bounded-resource failure into a silent
    // timeout-only outcome.
    internal long OverflowCount => Volatile.Read(ref _overflowCount);

    // A same-process completion can drain on the pump thread between the
    // operation submit and the caller's Register. Retain it until registration,
    // but keep both count and byte bounds so an operation nobody awaits cannot
    // grow this table without limit.
    public bool Register(MeshOperationId operationId, CompletionHandler handler)
    {
        if (operationId == default) return false;
        ArgumentNullException.ThrowIfNull(handler);

        CompletionDispatch? dispatch = null;
        lock (_gate)
        {
            if (_early.Remove(operationId, out var early))
            {
                _earlyBytes -= early.Bytes;
                dispatch = new CompletionDispatch(
                    handler,
                    early.Record,
                    early.Parts);
            }
            else if (_tombstones.Remove(operationId, out _))
            {
                _tombstoneBytes -= TombstoneRecordBytes;
                dispatch = new CompletionDispatch(
                    handler,
                    MeshReceiveRecord.CompletionFailure(
                        operationId,
                        RequestResult.Backpressured),
                    Array.Empty<Message>());
            }
            else
            {
                _pending[operationId] = handler;
            }
        }

        dispatch?.Invoke();
        return true;
    }

    // Registers a plain request callback (result + reply parts).
    public bool RegisterRequest(MeshOperationId operationId, RequestCallback callback)
    {
        ArgumentNullException.ThrowIfNull(callback);
        return Register(operationId, (record, parts) =>
            callback(MapResult(record.TerminalResult, record.FailureErrno), parts));
    }

    public void Complete(
        MeshReceiveRecord record, IReadOnlyList<Message> parts)
    {
        CompletionDispatch? dispatch = null;
        var dispose = false;
        var bytes = EstimateBytes(parts);
        lock (_gate)
        {
            if (_pending.Remove(record.OperationId, out var handler))
            {
                dispatch = new CompletionDispatch(handler, record, parts);
            }
            else if (_early.ContainsKey(record.OperationId)
                     || _tombstones.ContainsKey(record.OperationId))
            {
                // A second terminal for the same operation has no owner.
                dispose = true;
            }
            else if (_early.Count < _earlyCompletionCount
                     && CanRetain(_earlyBytes, bytes, _earlyCompletionBytes))
            {
                _early[record.OperationId] = new EarlyCompletion(
                    record,
                    parts,
                    bytes);
                _earlyBytes = checked(_earlyBytes + bytes);
            }
            else if (_tombstones.Count < _tombstoneCount
                     && CanRetain(
                         _tombstoneBytes,
                         TombstoneRecordBytes,
                         _tombstoneBytesLimit))
            {
                // The parts cannot be retained after the early-result budget is
                // full. Retain a bounded failure marker so a caller that
                // registers later receives CapacityExceeded instead of waiting
                // until its own timeout.
                _tombstones[record.OperationId] = new Tombstone();
                _tombstoneBytes = checked(
                    _tombstoneBytes + TombstoneRecordBytes);
                dispose = true;
            }
            else
            {
                // The pump still owns these parts at this point. Release them
                // when neither the result nor a failure marker can be retained.
                // The counter and metric are the observable CapacityExceeded
                // outcome for the bounded completion-retention resource.
                Interlocked.Increment(ref _overflowCount);
                ZLinkRuntimeMetrics.RecordMessageDropped(
                    _meshName,
                    "completion",
                    "reply",
                    "capacity_exceeded");
                dispose = true;
            }
        }

        if (dispose)
            ZLinkMessageParts.DisposeAll(parts);
        dispatch?.Invoke();
    }

    public void FailAll(RequestResult result)
    {
        List<CompletionDispatch> dispatches;
        List<IReadOnlyList<Message>> partsToDispose;
        lock (_gate)
        {
            dispatches = _pending
                .Select(entry => new CompletionDispatch(
                    entry.Value,
                    MeshReceiveRecord.CompletionFailure(entry.Key, result),
                    Array.Empty<Message>()))
                .ToList();
            _pending.Clear();
            partsToDispose = _early.Values
                .Select(static early => early.Parts)
                .ToList();
            _early.Clear();
            _tombstones.Clear();
            _earlyBytes = 0;
            _tombstoneBytes = 0;
        }

        foreach (var dispatch in dispatches)
            dispatch.Invoke();
        foreach (var parts in partsToDispose)
            ZLinkMessageParts.DisposeAll(parts);
    }

    private static bool CanRetain(long current, long bytes, long limit) =>
        bytes >= 0 && current <= limit - bytes;

    private static long EstimateBytes(IReadOnlyList<Message> parts)
    {
        try
        {
            var bytes = (long)CompletionRecordHeaderBytes;
            foreach (var part in parts)
                bytes = checked(bytes + Math.Max(part.Size, 1));
            return bytes;
        }
        catch (OverflowException)
        {
            return long.MaxValue;
        }
    }

    private sealed record EarlyCompletion(
        MeshReceiveRecord Record,
        IReadOnlyList<Message> Parts,
        long Bytes);

    private sealed record Tombstone;

    private sealed record CompletionDispatch(
        CompletionHandler Handler,
        MeshReceiveRecord Record,
        IReadOnlyList<Message> Parts)
    {
        internal void Invoke() => Handler(Record, Parts);
    }

    // Maps a Completion receive record's terminal result/errno onto the
    // framework callback surface. Completion terminal results are
    // zlink_request_result_t values (0 / 101+), which the binding's
    // RequestResult mirrors one-to-one.
    public static RequestResult MapResult(int terminalResult, int failureErrno)
    {
        if (terminalResult == 0) return RequestResult.Ok;
        var result = (RequestResult)terminalResult;
        return Enum.IsDefined(result) ? result : RequestResult.InternalError;
    }
}
