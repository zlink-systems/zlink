namespace Zlink.Framework.Runtime.Execution;

internal sealed class ZLinkRuntimeTaskRunner
{
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
        return TryStart(name, callback, TaskCreationOptions.None, out _);
    }

    public async ValueTask<bool> TryRunDetachedAsync(
        string name,
        Func<CancellationToken, ValueTask> callback
    )
    {
        var state = CreateTaskState(name, callback);
        var (acceptsRunnerExecution, acceptsOwnerExecution) = AdmissionPermissions();
        if (
            !await _supervisor
                .TryStartAsync(
                    this,
                    state.Completion.Task,
                    acceptsRunnerExecution,
                    acceptsOwnerExecution
                )
                .ConfigureAwait(false)
        )
            return false;
        ScheduleAccepted(state, TaskCreationOptions.None);
        return true;
    }

    public Task Run(string name, Func<CancellationToken, ValueTask> callback)
    {
        return TryStart(name, callback, TaskCreationOptions.None, out var task)
            ? task
            : Task.CompletedTask;
    }

    public Task RunLongRunning(string name, Func<CancellationToken, ValueTask> callback)
    {
        return TryStart(name, callback, TaskCreationOptions.LongRunning, out var task)
            ? task
            : Task.CompletedTask;
    }

    public async ValueTask StopAsync()
    {
        if (IsCurrentRunnerExecution)
            throw new InvalidOperationException(
                "A runtime task cannot synchronously stop the runner that owns it."
            );

        await _supervisor.StopRunnerAsync(this, _ownsSupervisor).ConfigureAwait(false);
    }

    private bool TryStart(
        string name,
        Func<CancellationToken, ValueTask> callback,
        TaskCreationOptions creationOptions,
        out Task task
    )
    {
        var state = CreateTaskState(name, callback);
        var startedTask = state.Completion.Task;
        var (acceptsRunnerExecution, acceptsOwnerExecution) = AdmissionPermissions();
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
        ScheduleAccepted(state, creationOptions);
        return true;
    }

    private TaskState CreateTaskState(string name, Func<CancellationToken, ValueTask> callback) =>
        new(
            this,
            name,
            callback,
            _errorSink,
            _shutdownToken,
            new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously)
        );

    private (bool AcceptsRunnerExecution, bool AcceptsOwnerExecution) AdmissionPermissions()
    {
        var acceptsRunnerExecution =
            AmbientExecution.Value is { IsActive: true } lease
            && (
                ReferenceEquals(lease.Runner, this)
                || _ownsSupervisor && ReferenceEquals(lease.Owner, _executionOwner)
            );
        var acceptsOwnerExecution =
            AmbientExecution.Value is { IsActive: true } ownerLease
            && ReferenceEquals(ownerLease.Owner, _executionOwner);
        return (acceptsRunnerExecution, acceptsOwnerExecution);
    }

    private static void ScheduleAccepted(TaskState state, TaskCreationOptions creationOptions)
    {
        // Scheduling happens after supervisor admission, outside its state lane.
        // The callback carries its own completion directly; no outer Task<Task>,
        // Unwrap task or completion-registration task is needed.
        if (creationOptions == TaskCreationOptions.LongRunning)
            _ = Task.Factory.StartNew(
                static value =>
                {
                    _ = RunDetachedCoreAsync((TaskState)value!);
                },
                state,
                CancellationToken.None,
                TaskCreationOptions.DenyChildAttach | TaskCreationOptions.LongRunning,
                TaskScheduler.Default
            );
        else
            ThreadPool.QueueUserWorkItem(
                static value =>
                {
                    _ = RunDetachedCoreAsync(value);
                },
                state,
                preferLocal: false
            );
    }

    private void RemoveCompletedTask(Task completed)
    {
        _supervisor.Remove(this, completed);
    }

    private static async Task RunDetachedCoreAsync(TaskState state)
    {
        var previous = AmbientExecution.Value;
        var lease = new ExecutionLease(state.Runner.ExecutionOwner, state.Runner);
        AmbientExecution.Value = lease;
        try
        {
            await state.Callback(state.ShutdownToken).ConfigureAwait(false);
        }
        catch (OperationCanceledException) when (state.ShutdownToken.IsCancellationRequested) { }
        catch (Exception ex)
        {
            try
            {
                state.ErrorSink.ReportRuntimeTaskException(state.Name, ex);
            }
            catch (Exception reportingFailure)
            {
                state.Runner.ReportErrorSinkFailure(state.Name, reportingFailure);
            }
        }
        finally
        {
            lease.Deactivate();
            AmbientExecution.Value = previous;
            state.Completion.TrySetResult();
            state.Runner.RemoveCompletedTask(state.Completion.Task);
        }
    }

    public void ReportErrorSinkFailure(string name, Exception exception)
    {
        ZLinkFrameworkDebugLog.TaskFailure(name, exception);
    }

    internal IZLinkRuntimeFailureReporter ErrorSink => _errorSink;

    private sealed record TaskState(
        ZLinkRuntimeTaskRunner Runner,
        string Name,
        Func<CancellationToken, ValueTask> Callback,
        IZLinkRuntimeFailureReporter ErrorSink,
        CancellationToken ShutdownToken,
        TaskCompletionSource Completion
    );

    private sealed class ExecutionLease(object owner, ZLinkRuntimeTaskRunner runner)
    {
        private int _active = 1;

        public object Owner { get; } = owner;

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
    private readonly ZLinkStateLane _lane = new();
    private bool _accepting = true;

    public bool TryStart(
        ZLinkRuntimeTaskRunner runner,
        Task task,
        bool acceptsRunnerExecution,
        bool acceptsOwnerExecution
    )
    {
        bool Admit() => AdmitOnLane(runner, task, acceptsRunnerExecution, acceptsOwnerExecution);
#if DEBUG
        // Reject before queueing: a post-admission guard failure would leave a task
        // in the active set without scheduling its worker.
        if (ZLinkInfrastructureWaitGuard.IsInfrastructureContext)
        {
            if (!_lane.TryRunInline(Admit, out var accepted))
            {
                ZLinkInfrastructureWaitGuard.ThrowIfBlocking(false, "task supervisor admission");
                throw new InvalidOperationException(
                    "Task supervisor admission requires a lane turn."
                );
            }
            return accepted;
        }
#endif
        return AwaitStateLane(_lane.RunAsync(Admit));
    }

    public ValueTask<bool> TryStartAsync(
        ZLinkRuntimeTaskRunner runner,
        Task task,
        bool acceptsRunnerExecution,
        bool acceptsOwnerExecution
    ) =>
        _lane.RunAsync(() =>
            AdmitOnLane(runner, task, acceptsRunnerExecution, acceptsOwnerExecution)
        );

    private bool AdmitOnLane(
        ZLinkRuntimeTaskRunner runner,
        Task task,
        bool acceptsRunnerExecution,
        bool acceptsOwnerExecution
    )
    {
        if (!runner.AcceptingOnSupervisorLane && !acceptsRunnerExecution)
            return false;
        if (!_accepting && !acceptsOwnerExecution)
            return false;
        runner.ActiveOnSupervisorLane.Add(task);
        _active.Add(task);
        return true;
    }

    public void Remove(ZLinkRuntimeTaskRunner runner, Task completed)
    {
        AwaitStateLane(
            _lane.RunAsync(() =>
            {
                runner.ActiveOnSupervisorLane.Remove(completed);
                _active.Remove(completed);
                return true;
            })
        );
    }

    public async ValueTask StopRunnerAsync(ZLinkRuntimeTaskRunner runner, bool ownsSupervisor)
    {
        while (true)
        {
            var active = await _lane
                .RunAsync(() =>
                {
                    runner.AcceptingOnSupervisorLane = false;
                    if (ownsSupervisor)
                        _accepting = false;

                    var activeSet = ownsSupervisor ? _active : runner.ActiveOnSupervisorLane;
                    activeSet.RemoveWhere(static candidate => candidate.IsCompleted);
                    return activeSet.ToArray();
                })
                .ConfigureAwait(false);
            if (active.Length == 0)
                return;

            await Task.WhenAll(active).ConfigureAwait(false);
        }
    }

    private static T AwaitStateLane<T>(ValueTask<T> operation)
    {
        ZLinkInfrastructureWaitGuard.ThrowIfBlocking(operation.IsCompleted, "task supervisor lane");
        return operation.GetAwaiter().GetResult();
    }
}
