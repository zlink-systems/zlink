using Zlink.Framework.Runtime.Execution;

namespace Zlink.Framework.Runtime;

/// <summary>
/// Atomically admits records against both count and encoded-byte limits.
/// </summary>
internal sealed class ZLinkBoundedIngressAdmission
{
    //  The relocation hold has no record or byte bound; a negotiated
    //  admission still passes the peer's agreed values explicitly.
    internal const int SourceIngressHoldRecordCapacity = int.MaxValue;
    internal const long SourceIngressHoldByteCapacity = long.MaxValue;

    private readonly ZLinkStateLane _lane = new();
    private readonly int _recordCapacity;
    private readonly long _byteCapacity;
    private int _records;
    private long _bytes;
    private bool _closed;
    private TaskCompletionSource? _emptyWaiter;

    internal ZLinkBoundedIngressAdmission(
        int recordCapacity = SourceIngressHoldRecordCapacity,
        long byteCapacity = SourceIngressHoldByteCapacity)
    {
        if (recordCapacity <= 0)
            throw new ArgumentOutOfRangeException(nameof(recordCapacity));
        if (byteCapacity <= 0)
            throw new ArgumentOutOfRangeException(nameof(byteCapacity));
        _recordCapacity = recordCapacity;
        _byteCapacity = byteCapacity;
    }

    internal ValueTask<bool> TryAcquireAsync(long encodedBytes)
    {
        if (encodedBytes < 0)
            return ValueTask.FromResult(false);
        return _lane.RunAsync(() =>
        {
            if (_closed
                || _records >= _recordCapacity
                || encodedBytes > _byteCapacity - _bytes)
                return false;
            _records++;
            _bytes += encodedBytes;
            return true;
        });
    }

    internal ValueTask ReleaseAsync(long encodedBytes)
    {
        if (encodedBytes < 0)
            throw new ArgumentOutOfRangeException(nameof(encodedBytes));
        return ReleaseAsyncCore(encodedBytes);
    }

    private async ValueTask ReleaseAsyncCore(long encodedBytes)
    {
        var completed = await _lane.RunAsync(() =>
        {
            if (_records == 0 || encodedBytes > _bytes)
                throw new InvalidOperationException(
                    "Ingress admission release does not match an acquired record.");
            _records--;
            _bytes -= encodedBytes;
            TaskCompletionSource? completed = null;
            if (_records == 0)
            {
                completed = _emptyWaiter;
                _emptyWaiter = null;
            }
            return completed;
        }).ConfigureAwait(false);
        completed?.TrySetResult();
    }

    internal async ValueTask ReleaseAllAsync()
    {
        var completed = await _lane.RunAsync(() =>
        {
            _records = 0;
            _bytes = 0;
            var completed = _emptyWaiter;
            _emptyWaiter = null;
            return completed;
        }).ConfigureAwait(false);
        completed?.TrySetResult();
    }

    internal async ValueTask CloseAndWaitForEmptyAsync(
        CancellationToken cancellationToken)
    {
        var wait = await _lane.RunAsync(() =>
        {
            _closed = true;
            if (_records == 0)
                return (Task?)null;
            _emptyWaiter ??= new TaskCompletionSource(
                TaskCreationOptions.RunContinuationsAsynchronously);
            return _emptyWaiter.Task;
        }).ConfigureAwait(false);
        if (wait is not null)
            await wait.WaitAsync(cancellationToken).ConfigureAwait(false);
    }

    internal ValueTask<(int Records, long Bytes)> SnapshotAsync() =>
        _lane.RunAsync(() => (_records, _bytes));

    internal ValueTask<long> GetRemainingByteCapacityAsync() =>
        _lane.RunAsync(() => _byteCapacity - _bytes);

    internal ValueTask<int> GetRemainingRecordCapacityAsync() =>
        _lane.RunAsync(() => _recordCapacity - _records);

    internal bool TryAcquire(long encodedBytes) =>
        AwaitStateLane(TryAcquireAsync(encodedBytes));

    internal void Release(long encodedBytes) =>
        AwaitStateLane(ReleaseAsync(encodedBytes));

    internal void ReleaseAll() =>
        AwaitStateLane(ReleaseAllAsync());

    internal (int Records, long Bytes) Snapshot() =>
        AwaitStateLane(SnapshotAsync());

    internal long RemainingByteCapacity =>
        AwaitStateLane(GetRemainingByteCapacityAsync());

    internal int RemainingRecordCapacity =>
        AwaitStateLane(GetRemainingRecordCapacityAsync());

    private static void AwaitStateLane(ValueTask operation) =>
        operation.GetAwaiter().GetResult();

    private static T AwaitStateLane<T>(ValueTask<T> operation) =>
        operation.GetAwaiter().GetResult();
}
