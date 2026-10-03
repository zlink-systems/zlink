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
/// same lane would deadlock, so it throws <see cref="InvalidOperationException"/> at the call site
/// instead. Code reached from a turn must call the component's private state methods directly
/// rather than re-entering through its public surface.
/// </para>
/// <para>
/// Use this for state ownership. Spot and Actor <i>execution</i> keeps using
/// <see cref="ZLinkSerialExecutionQueue"/>, which additionally carries relocation sealing and
/// lifecycle admission that state owners do not need.
/// </para>
/// </remarks>
internal sealed class ZLinkStateLane : IAsyncDisposable
{
    private readonly ConcurrentQueue<Func<ValueTask>> _mailbox = new();
    private readonly TaskCompletionSource _completed = new(
        TaskCreationOptions.RunContinuationsAsynchronously
    );

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

        // A running thread holds the claim only while it executes turns; a
        // queued ThreadPool item takes it when it runs, not when it is queued.
        // With no predecessor the caller runs its own turn without a completion
        // allocation. With predecessors the caller queues behind them and
        // drains them itself, so a caller that then blocks on the result never
        // waits for a ThreadPool worker to start the drain.
        if (TryClaimDrain())
        {
            if (_mailbox.IsEmpty)
            {
                var previous = CurrentLane.Value;
                try
                {
                    ObjectDisposedException.ThrowIf(Volatile.Read(ref _closed) != 0, this);
                    CurrentLane.Value = this;
                    return ValueTask.FromResult(work());
                }
                catch (Exception error)
                {
                    return ValueTask.FromException<T>(error);
                }
                finally
                {
                    CurrentLane.Value = previous;
                    if (ReleaseDrain())
                        _ = DrainAsync();
                }
            }

            var queued = Enqueue(work);
            _ = DrainAsync();
            return queued;
        }

        var completion = Enqueue(work);
        // The holder may have released between the claim attempt and the
        // enqueue; its release re-check and this attempt cannot both miss.
        if (TryClaimDrain())
            _ = DrainAsync();
        return completion;
    }

    private ValueTask<T> Enqueue<T>(Func<T> work)
    {
        var completion = new TaskCompletionSource<T>(
            TaskCreationOptions.RunContinuationsAsynchronously
        );
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
        return new ValueTask<T>(completion.Task);
    }

    /// <summary>Runs <paramref name="work"/> on the lane.</summary>
    internal ValueTask RunAsync(Action work)
    {
        ArgumentNullException.ThrowIfNull(work);
        var operation = RunAsync(() =>
        {
            work();
            return true;
        });
        if (operation.IsCompletedSuccessfully)
        {
            operation.GetAwaiter().GetResult();
            return ValueTask.CompletedTask;
        }
        return new ValueTask(operation.AsTask());
    }

    /// <summary>
    /// Queues asynchronous work and returns its completion. Submission from this lane is deferred;
    /// callers on the lane must observe the completion without awaiting another turn.
    /// </summary>
    internal ValueTask RunAsync(Func<ValueTask> work)
    {
        ArgumentNullException.ThrowIfNull(work);
        if (Volatile.Read(ref _closed) != 0)
            return ValueTask.FromException(new ObjectDisposedException(nameof(ZLinkStateLane)));

        var completion = new TaskCompletionSource(
            TaskCreationOptions.RunContinuationsAsynchronously
        );
        _mailbox.Enqueue(async () =>
        {
            try
            {
                await work().ConfigureAwait(false);
                completion.TrySetResult();
            }
            catch (Exception failure)
            {
                completion.TrySetException(failure);
            }
        });
        ScheduleDrain();
        return new ValueTask(completion.Task);
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
                    + "surface."
            );
    }

    private bool TryClaimDrain() => Interlocked.CompareExchange(ref _scheduled, 1, 0) == 0;

    //  Deferred drain for submissions that must not run turns on the caller's
    //  thread. The worker claims the lane only when it runs; until then any
    //  caller can claim it and drain the same FIFO.
    private void ScheduleDrain() =>
        ThreadPool.UnsafeQueueUserWorkItem(
            static state =>
            {
                if (state.TryClaimDrain())
                    _ = state.DrainAsync();
            },
            this,
            preferLocal: false
        );

    //  Runs with the claim held and keeps it until the mailbox is empty. Turns
    //  queued while it runs are drained by this same owner, as the C++ lane's
    //  drain_loop does, instead of being handed to another ThreadPool worker.
    private async Task DrainAsync()
    {
        CurrentLane.Value = this;
        try
        {
            do
            {
                while (_mailbox.TryDequeue(out var work))
                    await work().ConfigureAwait(false);
            } while (ReleaseDrain());
        }
        finally
        {
            CurrentLane.Value = null;
        }
    }

    //  Returns true when the caller has reclaimed the lane for work queued
    //  after its last turn and must drain it.
    private bool ReleaseDrain()
    {
        // The full fence orders the ownership release before the queue check;
        // otherwise a racing producer and drainer can both miss the wakeup.
        Interlocked.Exchange(ref _scheduled, 0);
        if (!_mailbox.IsEmpty)
            return TryClaimDrain();
        if (Volatile.Read(ref _closed) != 0 && Volatile.Read(ref _scheduled) == 0)
            _completed.TrySetResult();
        return false;
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
