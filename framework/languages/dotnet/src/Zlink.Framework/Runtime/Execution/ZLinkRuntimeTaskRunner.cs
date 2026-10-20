namespace Zlink.Framework.Runtime.Execution;

internal sealed class ZLinkRuntimeTaskRunner
{
    internal static T WithoutExecutionContextFlow<T>(Func<T> start) =>
        WithoutExecutionContextFlow(static callback => callback(), start);

    internal static T WithoutExecutionContextFlow<TState, T>(Func<TState, T> start, TState state)
    {
        if (ExecutionContext.IsFlowSuppressed())
            return start(state);
        using (ExecutionContext.SuppressFlow())
            return start(state);
    }

    internal static Task RunDisposal(ref Task? task, Func<Task> close)
    {
        while (true)
        {
            var previous = Volatile.Read(ref task);
            if (previous is not null && !previous.IsFaulted)
                return previous;
            var completion = new TaskCompletionSource(
                TaskCreationOptions.RunContinuationsAsynchronously
            );
            if (Interlocked.CompareExchange(ref task, completion.Task, previous) != previous)
                continue;
            _ = WithoutExecutionContextFlow(() => CompleteDisposalAsync(close, completion));
            return completion.Task;
        }
    }

    private static async Task CompleteDisposalAsync(
        Func<Task> close,
        TaskCompletionSource completion
    )
    {
        // Publish the elected task before closing, and leave any caller's state lane.
        await Task.Yield();
        try
        {
            await close().ConfigureAwait(false);
            completion.TrySetResult();
        }
        catch (Exception error)
        {
            completion.TrySetException(error);
        }
    }

    private static readonly AsyncLocal<ExecutionLease?> AmbientExecution = new();
    private readonly HashSet<Task> _active = [];
    private readonly IZLinkRuntimeFailureReporter _errorSink;
    private readonly object _executionOwner;
    private readonly ZLinkRuntimeTaskSupervisor _supervisor;
    private readonly bool _ownsSupervisor;
    private readonly CancellationToken _shutdownToken;
    private bool _accepting = true;

    public bool IsCurrentExecution =>
        AmbientExecution.Value is { IsActive: true } lease
        && ReferenceEquals(lease.Owner, _executionOwner);

    private bool IsCurrentRunnerExecution =>
        AmbientExecution.Value is { IsActive: true } lease && ReferenceEquals(lease.Runner, this);

    internal object ExecutionOwner => _executionOwner;

    internal CancellationToken ShutdownToken => _shutdownToken;

    // Runner admission state is owned by the fixed supervisor lane. These
    // accessors must only be used from a supervisor-lane turn.
    internal bool AcceptingOnSupervisorLane
    {
        get => _accepting;
        set => _accepting = value;
    }

    internal HashSet<Task> ActiveOnSupervisorLane => _active;

    internal static bool IsCurrentExecutionFor(object executionOwner) =>
        AmbientExecution.Value is { IsActive: true } lease
        && ReferenceEquals(lease.Owner, executionOwner);

    public ZLinkRuntimeTaskRunner(
        IZLinkRuntimeFailureReporter errorSink,
        CancellationToken shutdownToken,
        object? executionOwner = null,
        bool ownsSupervisor = false
    )
    {
        _errorSink = errorSink;
        _shutdownToken = shutdownToken;
        _executionOwner = executionOwner ?? this;
        _supervisor = executionOwner is ZLinkRuntimeExecutionScope scope
            ? scope.Supervisor
            : new ZLinkRuntimeTaskSupervisor();
        _ownsSupervisor = ownsSupervisor;
    }

    public void RunDetached(string name, Func<CancellationToken, ValueTask> callback)
    {
        TryRunDetached(name, callback);
    }

    public bool TryRunDetached(string name, Func<CancellationToken, ValueTask> callback)
    {
        return TryStart(name, callback, out _);
    }

    public Task Run(string name, Func<CancellationToken, ValueTask> callback)
    {
        return TryStart(name, callback, out var task) ? task : Task.CompletedTask;
    }

    /// <summary>
    /// Runs a loop that blocks in a native poll or wait. The loop is synchronous and owns one
    /// dedicated thread until it returns, so a blocked loop never holds a ThreadPool worker.
    /// </summary>
    public Task RunLongRunning(string name, Action<CancellationToken> loop)
    {
        return TryRunLongRunning(name, loop, out var task) ? task : Task.CompletedTask;
    }

    public bool TryRunLongRunning(string name, Action<CancellationToken> loop, out Task task)
    {
        ArgumentNullException.ThrowIfNull(loop);
        return TryStart(name, loop, out task);
    }

    /// <summary>
    /// Starts <paramref name="loop"/> on a thread of its own. Every blocking native loop of the
    /// framework starts here; none runs on a ThreadPool worker.
    /// </summary>
    internal static Task StartDedicatedThread(Action<object?> loop, object? state) =>
        Task.Factory.StartNew(
            loop,
            state,
            CancellationToken.None,
            TaskCreationOptions.DenyChildAttach | TaskCreationOptions.LongRunning,
            TaskScheduler.Default
        );

    public ValueTask StopAsync() => StopAsync(CancellationToken.None);

    public async ValueTask StopAsync(CancellationToken cancellationToken)
    {
        if (IsCurrentRunnerExecution)
            throw new InvalidOperationException(
                "A runtime task cannot synchronously stop the runner that owns it."
            );

        await _supervisor
            .StopRunnerAsync(this, _ownsSupervisor)
            .AsTask()
            .WaitAsync(cancellationToken)
            .ConfigureAwait(false);
    }

    // The work is either an asynchronous callback, which runs on the ThreadPool, or a
    // synchronous blocking loop, which runs on a dedicated thread.
    private bool TryStart(string name, Delegate work, out Task task)
    {
        var completion = new TaskCompletionSource(
            TaskCreationOptions.RunContinuationsAsynchronously
        );
        var state = new TaskState(this, name, work, completion);
        var startedTask = completion.Task;
        var acceptsRunnerExecution =
            AmbientExecution.Value is { IsActive: true } lease
            && (
                ReferenceEquals(lease.Runner, this)
                || _ownsSupervisor && ReferenceEquals(lease.Owner, _executionOwner)
            );
        var acceptsOwnerExecution =
            AmbientExecution.Value is { IsActive: true } ownerLease
            && ReferenceEquals(ownerLease.Owner, _executionOwner);
        var accepted = _supervisor.TryStart(
            this,
            startedTask,
            acceptsRunnerExecution,
            acceptsOwnerExecution
        );

        if (!accepted)
        {
            task = Task.CompletedTask;
            return false;
        }
        task = startedTask;
        // Scheduling happens after supervisor admission, outside its state lane.
        // The callback carries its own completion directly; no outer Task<Task>,
        // Unwrap task or completion-registration task is needed.
        if (work is Action<CancellationToken>)
            _ = StartDedicatedThread(static value => RunLoopCore((TaskState)value!), state);
        else
            ThreadPool.QueueUserWorkItem(
                static value =>
                {
                    _ = RunDetachedCoreAsync(value);
                },
                state,
                preferLocal: false
            );
        return true;
    }

    private void RemoveCompletedTask(Task completed)
    {
        _supervisor.Remove(this, completed);
    }

    private static async Task RunDetachedCoreAsync(TaskState state)
    {
        var previous = AmbientExecution.Value;
        var lease = new ExecutionLease(state.Runner);
        AmbientExecution.Value = lease;
        try
        {
            await ((Func<CancellationToken, ValueTask>)state.Work)(state.Runner.ShutdownToken)
                .ConfigureAwait(false);
        }
        catch (Exception exception)
        {
            Report(state, exception);
        }
        finally
        {
            Complete(state, lease, previous);
        }
    }

    private static void RunLoopCore(TaskState state)
    {
        var previous = AmbientExecution.Value;
        var lease = new ExecutionLease(state.Runner);
        AmbientExecution.Value = lease;
        try
        {
            ((Action<CancellationToken>)state.Work)(state.Runner.ShutdownToken);
        }
        catch (Exception exception)
        {
            Report(state, exception);
        }
        finally
        {
            Complete(state, lease, previous);
        }
    }

    private static void Report(TaskState state, Exception exception)
    {
        if (
            exception is OperationCanceledException
            && state.Runner.ShutdownToken.IsCancellationRequested
        )
            return;
        try
        {
            state.Runner.ErrorSink.ReportRuntimeTaskException(state.Name, exception);
        }
        catch (Exception reportingFailure)
        {
            state.Runner.ReportErrorSinkFailure(state.Name, reportingFailure);
        }
    }

    private static void Complete(TaskState state, ExecutionLease lease, ExecutionLease? previous)
    {
        lease.Deactivate();
        AmbientExecution.Value = previous;
        state.Completion.TrySetResult();
        state.Runner.RemoveCompletedTask(state.Completion.Task);
    }

    public void ReportErrorSinkFailure(string name, Exception exception)
    {
        ZLinkFrameworkDebugLog.TaskFailure(name, exception);
    }

    internal IZLinkRuntimeFailureReporter ErrorSink => _errorSink;

    private sealed record TaskState(
        ZLinkRuntimeTaskRunner Runner,
        string Name,
        Delegate Work,
        TaskCompletionSource Completion
    );

    private sealed class ExecutionLease(ZLinkRuntimeTaskRunner runner)
    {
        private int _active = 1;

        public object Owner => Runner.ExecutionOwner;

        public ZLinkRuntimeTaskRunner Runner { get; } = runner;

        public bool IsActive => Volatile.Read(ref _active) != 0;

        public void Deactivate() => Interlocked.Exchange(ref _active, 0);
    }
}

internal sealed class ZLinkRuntimeExecutionScope
{
    internal ZLinkRuntimeTaskSupervisor Supervisor { get; }

    public ZLinkRuntimeExecutionScope()
    {
        Supervisor = new ZLinkRuntimeTaskSupervisor();
    }
}

internal sealed class ZLinkRuntimeTaskSupervisor
{
    private readonly HashSet<Task> _active = [];
    private readonly object _gate = new();
    private bool _accepting = true;

    public bool TryStart(
        ZLinkRuntimeTaskRunner runner,
        Task task,
        bool acceptsRunnerExecution,
        bool acceptsOwnerExecution
    )
    {
        lock (_gate)
        {
            if (!runner.AcceptingOnSupervisorLane && !acceptsRunnerExecution)
                return false;

            if (!_accepting && !acceptsOwnerExecution)
                return false;

            runner.ActiveOnSupervisorLane.Add(task);
            _active.Add(task);
            return true;
        }
    }

    public void Remove(ZLinkRuntimeTaskRunner runner, Task completed)
    {
        lock (_gate)
        {
            runner.ActiveOnSupervisorLane.Remove(completed);
            _active.Remove(completed);
        }
    }

    public async ValueTask StopRunnerAsync(ZLinkRuntimeTaskRunner runner, bool ownsSupervisor)
    {
        while (true)
        {
            Task[] active;
            lock (_gate)
            {
                runner.AcceptingOnSupervisorLane = false;
                if (ownsSupervisor)
                    _accepting = false;

                var activeSet = ownsSupervisor ? _active : runner.ActiveOnSupervisorLane;
                activeSet.RemoveWhere(static candidate => candidate.IsCompleted);
                active = activeSet.ToArray();
            }
            if (active.Length == 0)
                return;

            await Task.WhenAll(active).ConfigureAwait(false);
        }
    }
}
