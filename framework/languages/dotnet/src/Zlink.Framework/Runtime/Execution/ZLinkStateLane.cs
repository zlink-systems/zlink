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
    private ConcurrentQueue<LaneWork>? _mailbox;
    private TaskCompletionSource? _completed;

    private ConcurrentQueue<LaneWork> Mailbox =>
        LazyInitializer.EnsureInitialized(ref _mailbox, static () => new());

    private bool HasQueuedWork => Volatile.Read(ref _mailbox) is { IsEmpty: false };

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
        return RunAsync(static work => work(), work);
    }

    /// <summary>
    /// Runs <paramref name="work"/> with its <paramref name="state"/> on the lane; the same turn
    /// as <see cref="RunAsync{T}(Func{T})"/> without a closure for the state.
    /// </summary>
    internal ValueTask<T> RunAsync<TState, T>(Func<TState, T> work, TState state)
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
            if (!HasQueuedWork)
            {
                var previous = CurrentLane.Value;
                try
                {
                    ObjectDisposedException.ThrowIf(Volatile.Read(ref _closed) != 0, this);
                    CurrentLane.Value = this;
                    return ValueTask.FromResult(work(state));
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

            var queued = Enqueue(work, state);
            _ = DrainAsync();
            return queued;
        }

        var completion = Enqueue(work, state);
        // The holder may have released between the claim attempt and the
        // enqueue; its release re-check and this attempt cannot both miss.
        if (TryClaimDrain())
            _ = DrainAsync();
        return completion;
    }

    private ValueTask<T> Enqueue<TState, T>(Func<TState, T> work, TState state)
    {
        var completion = new ResultWork<TState, T>(work, state);
        Mailbox.Enqueue(completion);
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

        var completion = new AsyncWork(work);
        Mailbox.Enqueue(completion);
        ScheduleDrain();
        return new ValueTask(completion.Task);
    }

    /// <summary>
    /// Queues <paramref name="work"/> without waiting for it (spec 06 §5 <c>tryPost</c>). Returns
    /// false when the lane is closed.
    /// </summary>
    /// <remarks>
    /// The work joins the same FIFO as <see cref="RunAsync{T}"/>, so any later turn observes it.
    /// Nobody awaits it, so the work reports its own failures; an escaped exception is published
    /// as an unobserved task fault. A post from a turn of this lane waits for nothing and runs
    /// after that turn.
    /// </remarks>
    internal bool TryPost(Action work)
    {
        ArgumentNullException.ThrowIfNull(work);
        return TryPost(static work => work(), work);
    }

    /// <summary>
    /// Queues <paramref name="work"/> with its <paramref name="state"/>, so a caller that would
    /// otherwise capture the state in a closure posts one queued item per turn.
    /// </summary>
    internal bool TryPost<TState>(Action<TState> work, TState state)
    {
        ArgumentNullException.ThrowIfNull(work);
        if (Volatile.Read(ref _closed) != 0)
            return false;
        Mailbox.Enqueue(new PostWork<TState>(work, state));
        // A current owner, including a turn that posts to its own lane, drains
        // this item before it releases the lane.
        if (TryClaimDrain())
            _ = DrainAsync();
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
                while (_mailbox is { } mailbox && mailbox.TryDequeue(out var work))
                    await work.RunAsync().ConfigureAwait(false);
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
        if (HasQueuedWork)
            return TryClaimDrain();
        if (Volatile.Read(ref _closed) != 0 && Volatile.Read(ref _scheduled) == 0)
            _completed?.TrySetResult();
        return false;
    }

    // One queued item per turn. The item owns the result signal of its turn, so a
    // queued turn allocates no wrapper closure around the caller's work.
    private abstract class LaneWork
    {
        internal abstract ValueTask RunAsync();
    }

    private sealed class ResultWork<TState, T>(Func<TState, T> work, TState state) : LaneWork
    {
        private readonly TaskCompletionSource<T> _completion = new(
            TaskCreationOptions.RunContinuationsAsynchronously
        );

        internal Task<T> Task => _completion.Task;

        internal override ValueTask RunAsync()
        {
            try
            {
                _completion.TrySetResult(work(state));
            }
            catch (Exception error)
            {
                _completion.TrySetException(error);
            }
            return ValueTask.CompletedTask;
        }
    }

    private sealed class AsyncWork(Func<ValueTask> work) : LaneWork
    {
        private readonly TaskCompletionSource _completion = new(
            TaskCreationOptions.RunContinuationsAsynchronously
        );

        internal Task Task => _completion.Task;

        internal override async ValueTask RunAsync()
        {
            try
            {
                await work().ConfigureAwait(false);
                _completion.TrySetResult();
            }
            catch (Exception failure)
            {
                _completion.TrySetException(failure);
            }
        }
    }

    private sealed class PostWork<TState>(Action<TState> work, TState state) : LaneWork
    {
        internal override ValueTask RunAsync()
        {
            try
            {
                work(state);
            }
            catch (Exception failure)
            {
                // A posted turn has no awaiting caller. Its failure becomes an
                // unobserved task fault (the channel of any fire-and-forget
                // task) instead of stopping the turns queued behind it.
                _ = System.Threading.Tasks.Task.FromException(failure);
            }
            return ValueTask.CompletedTask;
        }
    }

    public async ValueTask DisposeAsync()
    {
        if (Interlocked.Exchange(ref _closed, 1) != 0)
            return;

        //  A drain in flight completes the signal on its way out. With no drain scheduled there is
        //  nothing left to wait for.
        if (Volatile.Read(ref _scheduled) == 0 && !HasQueuedWork)
            return;
        var completed = LazyInitializer.EnsureInitialized(
            ref _completed,
            static () =>
                new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously)
        );
        if (Volatile.Read(ref _scheduled) == 0 && !HasQueuedWork)
            completed.TrySetResult();
        else
            ScheduleDrain();
        await completed.Task.ConfigureAwait(false);
    }
}

/// <summary>
/// The blocking compatibility boundary for state lanes (spec 06 §5 "완료 신호와 블로킹 호환
/// 경계").
/// </summary>
/// <remarks>
/// A synchronous surface that must return after its lane work has finished blocks here. Lane
/// completions run their continuations asynchronously, so the wait never runs the caller's
/// continuation inside the lane turn it waits for. Code that runs in a framework execution context
/// (handler, lane turn, dispatch worker, completion callback) uses the asynchronous surface instead.
/// </remarks>
internal static class ZLinkStateLaneWait
{
    internal static T AwaitStateLane<T>(ValueTask<T> operation) =>
        operation.GetAwaiter().GetResult();

    internal static void AwaitStateLane(ValueTask operation) => operation.GetAwaiter().GetResult();
}
