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
#if DEBUG
    internal static readonly AsyncLocal<Action<string, ZLinkStateLane?>?> WorkObserver = new();
#endif

    [System.Diagnostics.Conditional("DEBUG")]
    private void ObserveEntry()
    {
#if DEBUG
        WorkObserver.Value?.Invoke("lane-entry", this);
#endif
    }

    [System.Diagnostics.Conditional("DEBUG")]
    internal static void ObserveWork(string stage)
    {
#if DEBUG
        WorkObserver.Value?.Invoke(stage, Current);
#endif
    }

    private ConcurrentQueue<LaneWork>? _mailbox;
    private TaskCompletionSource? _completed;

    private ConcurrentQueue<LaneWork> Mailbox =>
        LazyInitializer.EnsureInitialized(ref _mailbox, static () => new());

    internal int QueuedWorkCount => Volatile.Read(ref _mailbox)?.Count ?? 0;

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
        ObserveEntry();
        ThrowIfReentrant();
        if (Volatile.Read(ref _closed) != 0)
            throw new ObjectDisposedException(nameof(ZLinkStateLane));

        // An idle RunAsync turn rechecks close after its claim and fails instead of
        // running on a closed lane. TryPost has no such recheck: see its summary.
        var inline = TryRunInlineAsync(
                static entry =>
                {
                    ObjectDisposedException.ThrowIf(
                        Volatile.Read(ref entry.Lane._closed) != 0,
                        entry.Lane
                    );
                    return entry.Work(entry.State);
                },
                (Lane: this, Work: work, State: state)
            )
            .GetAwaiter()
            .GetResult();
        if (inline.Ran)
            return inline.Result;

        var completion = Enqueue(work, state);
        // The holder may have released between the claim attempt and the
        // enqueue; its release re-check and this attempt cannot both miss.
        if (TryClaimDrain())
            _ = DrainAsync();
        return completion;
    }

    // The async builder isolates AsyncLocal changes even with suppressed flow.
    // This method never suspends. The result stays a value so a thrown OCE is
    // faulted, like queued ResultWork, rather than converted to cancellation.
#pragma warning disable CS1998
    private async ValueTask<(bool Ran, ValueTask<T> Result)> TryRunInlineAsync<TState, T>(
        Func<TState, T> work,
        TState state
    )
    {
        if (!TryClaimDrain())
            return (false, default);
        if (HasQueuedWork)
        {
            _ = DrainAsync();
            return (false, default);
        }

        try
        {
            CurrentLane.Value = this;
            return (true, ValueTask.FromResult(work(state)));
        }
        catch (Exception error)
        {
            return (true, ValueTask.FromException<T>(error));
        }
        finally
        {
            if (ReleaseDrain())
                _ = DrainAsync();
        }
    }
#pragma warning restore CS1998

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
        ObserveEntry();
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
    /// Runs an idle turn directly, or queues <paramref name="work"/> with its
    /// <paramref name="state"/> without a closure when the lane has a predecessor.
    /// A post accepted by the entry check always runs, even if the lane closes
    /// before its turn, because callers hand release work to it.
    /// </summary>
    internal bool TryPost<TState>(Action<TState> work, TState state)
    {
        ArgumentNullException.ThrowIfNull(work);
        ObserveEntry();
        if (Volatile.Read(ref _closed) != 0)
            return false;
        if (
            TryRunInlineAsync(
                    static posted =>
                    {
                        posted.Work(posted.State);
                        return true;
                    },
                    (Work: work, State: state)
                )
                .GetAwaiter()
                .GetResult()
                .Ran
        )
            return true;
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
        // The async method builder restores the caller's ExecutionContext on
        // suspension and return; clearing the value would only rebuild the map.
        do
        {
            while (_mailbox is { } mailbox && mailbox.TryDequeue(out var work))
#if DEBUG
                await work.RunObservedAsync().ConfigureAwait(false);
#else
                await work.RunAsync().ConfigureAwait(false);
#endif
        } while (ReleaseDrain());
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
#if DEBUG
        private readonly Action<string, ZLinkStateLane?>? _observer = WorkObserver.Value;

        internal async ValueTask RunObservedAsync()
        {
            // Queued turns run in the drainer's context. Carry only the test
            // observer, without changing runtime ExecutionContext ownership.
            var previous = WorkObserver.Value;
            WorkObserver.Value = _observer;
            try
            {
                await RunAsync().ConfigureAwait(false);
            }
            finally
            {
                WorkObserver.Value = previous;
            }
        }
#endif

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
