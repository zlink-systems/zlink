namespace Zlink.Framework.Runtime.Execution;

internal sealed class ZLinkRuntimeTaskRunner
{
    private static readonly AsyncLocal<ExecutionLease?> AmbientExecution = new();
    private readonly ZLinkStateLane _lane = new();
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

        await _lane.RunAsync(() => _accepting = false).ConfigureAwait(false);

        if (_ownsSupervisor)
        {
            await _supervisor.StopAsync().ConfigureAwait(false);
            return;
        }

        while (true)
        {
            var active = await _lane.RunAsync(() =>
            {
                _active.RemoveWhere(static candidate => candidate.IsCompleted);
                return _active.ToArray();
            }).ConfigureAwait(false);
            if (active.Length == 0) return;

            await Task.WhenAll(active).ConfigureAwait(false);
        }
    }

    private bool TryStart(
        string name,
        Func<CancellationToken, ValueTask> callback,
        TaskCreationOptions creationOptions,
        out Task task)
    {
        // The outer task is created cold and started only after both state lanes
        // release it, so its synchronous callback prefix cannot inherit either
        // lane's AsyncLocal ownership.
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
        var accepted = AwaitStateLane(_lane.RunAsync(() =>
        {
            if (!_accepting
                && !acceptsRunnerExecution)
            {
                return false;
            }

            if (!_supervisor.TryStart(startedTask, acceptsOwnerExecution))
                return false;
            _active.Add(startedTask);
            RegisterCompletion(startedTask);
            return true;
        }));

        if (!accepted)
        {
            task = Task.CompletedTask;
            return false;
        }
        task = startedTask;
        outer.Start(TaskScheduler.Default);
        return true;
    }

    private void RemoveCompletedTask(Task completed)
    {
        AwaitStateLane(_lane.RunAsync(() => _active.Remove(completed)));
        _supervisor.Remove(completed);
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

    private static T AwaitStateLane<T>(ValueTask<T> operation) =>
        operation.GetAwaiter().GetResult();

    private static void AwaitStateLane(ValueTask operation) =>
        operation.GetAwaiter().GetResult();

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

    public bool TryStart(Task task, bool acceptsOwnerExecution)
    {
        return AwaitStateLane(_lane.RunAsync(() =>
        {
            if (!_accepting && !acceptsOwnerExecution)
                return false;

            _active.Add(task);
            return true;
        }));
    }

    public void Remove(Task completed)
    {
        AwaitStateLane(_lane.RunAsync(() => _active.Remove(completed)));
    }

    public async ValueTask StopAsync()
    {
        while (true)
        {
            var active = await _lane.RunAsync(() =>
            {
                _accepting = false;
                _active.RemoveWhere(static candidate => candidate.IsCompleted);
                return _active.ToArray();
            }).ConfigureAwait(false);
            if (active.Length == 0) return;

            await Task.WhenAll(active).ConfigureAwait(false);
        }
    }

    private static T AwaitStateLane<T>(ValueTask<T> operation) =>
        operation.GetAwaiter().GetResult();
}
