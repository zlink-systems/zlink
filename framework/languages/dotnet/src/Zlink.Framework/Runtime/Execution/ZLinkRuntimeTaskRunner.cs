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

    public bool IsCurrentExecution
        => AmbientExecution.Value is { IsActive: true } lease
           && ReferenceEquals(lease.Owner, _executionOwner);

    private bool IsCurrentRunnerExecution
        => AmbientExecution.Value is { IsActive: true } lease
           && ReferenceEquals(lease.Runner, this);

    internal object ExecutionOwner => _executionOwner;

    // Runner admission state is owned by the fixed supervisor lane. These
    // accessors must only be used from a supervisor-lane turn.
    internal bool AcceptingOnSupervisorLane
    {
        get => _accepting;
        set => _accepting = value;
    }

    internal HashSet<Task> ActiveOnSupervisorLane => _active;

    internal static bool IsCurrentExecutionFor(object executionOwner)
        => AmbientExecution.Value is { IsActive: true } lease
           && ReferenceEquals(lease.Owner, executionOwner);

    public ZLinkRuntimeTaskRunner(
        IZLinkRuntimeFailureReporter errorSink,
        CancellationToken shutdownToken,
        object? executionOwner = null,
        bool ownsSupervisor = false)
    {
        _errorSink = errorSink;
        _shutdownToken = shutdownToken;
        _executionOwner = executionOwner ?? this;
        _supervisor = executionOwner is ZLinkRuntimeExecutionScope scope
            ? scope.Supervisor
            : new ZLinkRuntimeTaskSupervisor();
        _ownsSupervisor = ownsSupervisor;
    }

    public void RunDetached(
        string name,
        Func<CancellationToken, ValueTask> callback)
    {
        TryRunDetached(name, callback);
    }

    public bool TryRunDetached(
        string name,
        Func<CancellationToken, ValueTask> callback)
    {
        return TryStart(
            name,
            callback,
            TaskCreationOptions.None,
            out _);
    }

    public Task Run(
        string name,
        Func<CancellationToken, ValueTask> callback)
    {
        return TryStart(
                   name,
                   callback,
                   TaskCreationOptions.None,
                   out var task)
            ? task
            : Task.CompletedTask;
    }

    public Task RunLongRunning(
        string name,
        Func<CancellationToken, ValueTask> callback)
    {
        return TryStart(
                   name,
                   callback,
                   TaskCreationOptions.LongRunning,
                   out var task)
            ? task
            : Task.CompletedTask;
    }

    public async ValueTask StopAsync()
    {
        if (IsCurrentRunnerExecution)
            throw new InvalidOperationException(
                "A runtime task cannot synchronously stop the runner that owns it.");

        await _supervisor.StopRunnerAsync(this, _ownsSupervisor).ConfigureAwait(false);
    }

    private bool TryStart(
        string name,
        Func<CancellationToken, ValueTask> callback,
        TaskCreationOptions creationOptions,
        out Task task)
    {
        // The outer task is created cold and started only after the supervisor
        // state lane releases it, so its synchronous callback prefix cannot
        // inherit that lane's AsyncLocal ownership.
        var outer = new Task<Task>(
            static state => RunDetachedCoreAsync((TaskState)state!),
            new TaskState(this, name, callback, _errorSink, _shutdownToken),
            CancellationToken.None,
            TaskCreationOptions.DenyChildAttach | creationOptions);
        var startedTask = outer.Unwrap();
        var acceptsRunnerExecution = AmbientExecution.Value is { IsActive: true } lease
                                     && (ReferenceEquals(lease.Runner, this)
                                         || _ownsSupervisor
                                         && ReferenceEquals(lease.Owner, _executionOwner));
        var acceptsOwnerExecution = AmbientExecution.Value is { IsActive: true } ownerLease
                                    && ReferenceEquals(ownerLease.Owner, _executionOwner);
        var accepted = _supervisor.TryStart(
            this,
            startedTask,
            acceptsRunnerExecution,
            acceptsOwnerExecution);

        if (!accepted)
        {
            task = Task.CompletedTask;
            return false;
        }
        task = startedTask;
        RegisterCompletion(startedTask);
        outer.Start(TaskScheduler.Default);
        return true;
    }

    private void RemoveCompletedTask(Task completed)
    {
        _supervisor.Remove(this, completed);
    }

    private void RegisterCompletion(Task task)
    {
        if (ExecutionContext.IsFlowSuppressed())
        {
            _ = task.ContinueWith(
                static (completed, state) => ((ZLinkRuntimeTaskRunner)state!).RemoveCompletedTask(completed),
                this,
                CancellationToken.None,
                TaskContinuationOptions.ExecuteSynchronously,
                TaskScheduler.Default);
            return;
        }

        using (ExecutionContext.SuppressFlow())
            _ = task.ContinueWith(
                static (completed, state) => ((ZLinkRuntimeTaskRunner)state!).RemoveCompletedTask(completed),
                this,
                CancellationToken.None,
                TaskContinuationOptions.ExecuteSynchronously,
                TaskScheduler.Default);
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
        catch (OperationCanceledException) when (state.ShutdownToken.IsCancellationRequested)
        {
        }
        catch (Exception ex)
        {
            try
            {
                state.ErrorSink.ReportRuntimeTaskException(state.Name, ex);
            }
            catch
            {
            }
        }
        finally
        {
            lease.Deactivate();
            AmbientExecution.Value = previous;
        }
    }

    public void ReportErrorSinkFailure(
        string name,
        Exception exception)
    {
        ZLinkFrameworkDebugLog.TaskFailure(name, exception);
    }

    internal IZLinkRuntimeFailureReporter ErrorSink => _errorSink;

    private sealed record TaskState(
        ZLinkRuntimeTaskRunner Runner,
        string Name,
        Func<CancellationToken, ValueTask> Callback,
        IZLinkRuntimeFailureReporter ErrorSink,
        CancellationToken ShutdownToken);

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
        bool acceptsOwnerExecution)
    {
        return AwaitStateLane(_lane.RunAsync(() =>
        {
            if (!runner.AcceptingOnSupervisorLane && !acceptsRunnerExecution)
                return false;

            if (!_accepting && !acceptsOwnerExecution)
                return false;

            runner.ActiveOnSupervisorLane.Add(task);
            _active.Add(task);
            return true;
        }));
    }

    public void Remove(ZLinkRuntimeTaskRunner runner, Task completed)
    {
        AwaitStateLane(_lane.RunAsync(() =>
        {
            runner.ActiveOnSupervisorLane.Remove(completed);
            _active.Remove(completed);
            return true;
        }));
    }

    public async ValueTask StopRunnerAsync(
        ZLinkRuntimeTaskRunner runner,
        bool ownsSupervisor)
    {
        while (true)
        {
            var active = await _lane.RunAsync(() =>
            {
                runner.AcceptingOnSupervisorLane = false;
                if (ownsSupervisor)
                    _accepting = false;

                var activeSet = ownsSupervisor
                    ? _active
                    : runner.ActiveOnSupervisorLane;
                activeSet.RemoveWhere(static candidate => candidate.IsCompleted);
                return activeSet.ToArray();
            }).ConfigureAwait(false);
            if (active.Length == 0) return;

            await Task.WhenAll(active).ConfigureAwait(false);
        }
    }

    private static T AwaitStateLane<T>(ValueTask<T> operation) =>
        operation.GetAwaiter().GetResult();
}
