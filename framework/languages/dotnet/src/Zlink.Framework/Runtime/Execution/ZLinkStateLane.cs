using System.Collections.Concurrent;

namespace Zlink.Framework.Runtime.Execution;

/// <summary>
/// Single-owner execution lane for a component's mutable state.
/// </summary>
/// <remarks>
/// <para>
/// A component that owns state runs every read and write of that state through one lane. The lane
/// executes at most one work item at a time, so the state needs no lock and its collections stay
/// plain <see cref="Dictionary{TKey,TValue}"/> — ownership is what makes them safe, not a gate.
/// </para>
/// <para>
/// This exists because <c>lock</c> cannot span <c>await</c>. Any component that guards state with a
/// gate and then does asynchronous work has to release the gate first, which turns every async
/// boundary into "snapshot, release, act on a value that may already be stale". That shape is
/// forced by the mechanism, not chosen, and it is the source of the intermittent route/session
/// failures this design replaces. Inside a lane turn there is no release point, so no snapshot.
/// </para>
/// <para>
/// A lane is <b>not</b> reentrant. Calling <see cref="RunAsync{T}"/> from inside a lane turn on the
/// same lane deadlocks. Code reached from a turn must call the component's private state methods
/// directly rather than re-entering through its public surface.
/// </para>
/// <para>
/// Use this for state ownership. Spot and Actor <i>execution</i> keeps using
/// <see cref="ZLinkSerialExecutionQueue"/>, which additionally carries relocation sealing and
/// lifecycle admission that state owners do not need.
/// </para>
/// </remarks>
internal sealed class ZLinkStateLane : IAsyncDisposable
{
    //  Draining a bounded batch keeps one saturated lane from occupying a thread-pool thread
    //  indefinitely: the drain yields after this many items and reschedules if work remains.
    private const int DrainBatchLimit = 100;

    private readonly ConcurrentQueue<Func<ValueTask>> _mailbox = new();
    private readonly TaskCompletionSource _completed =
        new(TaskCreationOptions.RunContinuationsAsynchronously);

    private int _scheduled;
    private int _closed;

    /// <summary>Tracks which lane the calling code is currently executing on.</summary>
    /// <remarks>
    /// Only used to turn reentrancy into a diagnosable exception instead of a hang. It is
    /// <see cref="AsyncLocal{T}"/> so it survives the awaits inside a single turn.
    /// </remarks>
    private static readonly AsyncLocal<ZLinkStateLane?> CurrentLane = new();

    /// <summary>The lane whose turn the calling code is running on, if any.</summary>
    internal static ZLinkStateLane? Current => CurrentLane.Value;

    /// <summary>Whether the calling code is already executing on this lane.</summary>
    internal bool IsOnLane => ReferenceEquals(CurrentLane.Value, this);

    /// <summary>Runs <paramref name="work"/> on the lane and returns its result.</summary>
    /// <exception cref="InvalidOperationException">
    /// The caller is already on this lane. A lane turn cannot wait for another turn of the same
    /// lane, so this would otherwise hang.
    /// </exception>
    /// <exception cref="ObjectDisposedException">The lane is closed.</exception>
    internal ValueTask<T> RunAsync<T>(Func<T> work)
    {
        ArgumentNullException.ThrowIfNull(work);
        ThrowIfReentrant();
        if (Volatile.Read(ref _closed) != 0)
            throw new ObjectDisposedException(nameof(ZLinkStateLane));

        var completion = new TaskCompletionSource<T>(
            TaskCreationOptions.RunContinuationsAsynchronously);
        _mailbox.Enqueue(() =>
        {
            try
            {
                completion.TrySetResult(work());
            }
            catch (Exception error)
            {
                completion.TrySetException(error);
            }

            return ValueTask.CompletedTask;
        });
        ScheduleDrain();
        return new ValueTask<T>(completion.Task);
    }

    /// <summary>Runs <paramref name="work"/> on the lane.</summary>
    internal ValueTask RunAsync(Action work)
    {
        ArgumentNullException.ThrowIfNull(work);
        return new ValueTask(
            RunAsync(() => { work(); return true; }).AsTask());
    }

    /// <summary>
    /// Queues <paramref name="work"/> without waiting for it. Ordering against other posts on this
    /// lane still holds.
    /// </summary>
    internal bool TryPost(Func<ValueTask> work)
    {
        ArgumentNullException.ThrowIfNull(work);
        if (Volatile.Read(ref _closed) != 0)
            return false;

        _mailbox.Enqueue(work);
        ScheduleDrain();
        return true;
    }

    /// <summary>
    /// Throws when the caller is already executing on this lane. Call this from any component
    /// method that will post to the lane, so a reentrant path fails at its source with a name
    /// attached instead of deadlocking somewhere later.
    /// </summary>
    internal void ThrowIfReentrant()
    {
        if (IsOnLane)
            throw new InvalidOperationException(
                "This code already runs on the state lane it is trying to enter. Call the "
                + "component's private state method directly instead of re-entering its public "
                + "surface.");
    }

    private void ScheduleDrain()
    {
        //  Exactly one drain runs at a time. The drain clears the flag and re-checks the mailbox
        //  before exiting, so an item enqueued during that window is never left unscheduled.
        if (Interlocked.CompareExchange(ref _scheduled, 1, 0) == 0)
            ThreadPool.UnsafeQueueUserWorkItem(
                static state => _ = state.DrainAsync(), this, preferLocal: true);
    }

    private async Task DrainAsync()
    {
        CurrentLane.Value = this;
        try
        {
            var processed = 0;
            while (processed < DrainBatchLimit && _mailbox.TryDequeue(out var work))
            {
                try
                {
                    await work().ConfigureAwait(false);
                }
                catch
                {
                    //  RunAsync already routed the failure to its caller's completion. A TryPost
                    //  callback owns its own errors; letting one escape here would tear down the
                    //  lane and strand every item behind it.
                }

                processed++;
            }
        }
        finally
        {
            CurrentLane.Value = null;
            Volatile.Write(ref _scheduled, 0);
            if (!_mailbox.IsEmpty)
                ScheduleDrain();
            else if (Volatile.Read(ref _closed) != 0)
                _completed.TrySetResult();
        }
    }

    public async ValueTask DisposeAsync()
    {
        if (Interlocked.Exchange(ref _closed, 1) != 0)
            return;

        //  A drain in flight completes the signal on its way out. With no drain scheduled there is
        //  nothing left to wait for.
        if (Volatile.Read(ref _scheduled) == 0 && _mailbox.IsEmpty)
            _completed.TrySetResult();
        else
            ScheduleDrain();

        await _completed.Task.ConfigureAwait(false);
    }
}
