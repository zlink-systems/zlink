namespace Zlink.Framework.Runtime.Execution;

internal sealed class ZLinkRuntimeTaskRunner
{
    private static readonly AsyncLocal<ExecutionLease?> AmbientExecution = new();
    private readonly object _gate = new();
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
            : new ZLinkRuntimeTaskSupervisor(_executionOwner);
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

        lock (_gate)
        {
            _accepting = false;
        }

        if (_ownsSupervisor)
        {
            await _supervisor.StopAsync().ConfigureAwait(false);
            return;
        }

        while (true)
        {
            Task[] active;
            lock (_gate)
            {
                _active.RemoveWhere(static candidate => candidate.IsCompleted);
                if (_active.Count == 0) return;
                active = _active.ToArray();
            }

            await Task.WhenAll(active).ConfigureAwait(false);
        }
    }

    private bool TryStart(
        string name,
        Func<CancellationToken, ValueTask> callback,
        TaskCreationOptions creationOptions,
        out Task task)
    {
        lock (_gate)
        {
            if (!_accepting
                && !(AmbientExecution.Value is { IsActive: true } lease
                     && (ReferenceEquals(lease.Runner, this)
                         || _ownsSupervisor && ReferenceEquals(lease.Owner, _executionOwner))))
            {
                task = Task.CompletedTask;
                return false;
            }

            if (!_supervisor.TryStart(
                    () => Task.Factory.StartNew(
                        static state => RunDetachedCoreAsync((TaskState)state!),
                        new TaskState(this, name, callback, _errorSink, _shutdownToken),
                        CancellationToken.None,
                        TaskCreationOptions.DenyChildAttach | creationOptions,
                        TaskScheduler.Default).Unwrap(),
                    out task))
                return false;
            _active.Add(task);
            _ = task.ContinueWith(
                static (completed, state) => ((ZLinkRuntimeTaskRunner)state!).RemoveCompletedTask(completed),
                this,
                CancellationToken.None,
                TaskContinuationOptions.ExecuteSynchronously,
                TaskScheduler.Default);
            return true;
        }
    }

    private void RemoveCompletedTask(Task completed)
    {
        lock (_gate) _active.RemoveWhere(static candidate => candidate.IsCompleted);
        _supervisor.Remove(completed);
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
        Supervisor = new ZLinkRuntimeTaskSupervisor(this);
    }
}

internal sealed class ZLinkRuntimeTaskSupervisor(object executionOwner)
{
    private readonly HashSet<Task> _active = [];
    private readonly object _gate = new();
    private bool _accepting = true;

    public bool TryStart(Func<Task> start, out Task task)
    {
        lock (_gate)
        {
            if (!_accepting && !ZLinkRuntimeTaskRunner.IsCurrentExecutionFor(executionOwner))
            {
                task = Task.CompletedTask;
                return false;
            }

            task = start();
            _active.Add(task);
            return true;
        }
    }

    public void Remove(Task completed)
    {
        lock (_gate) _active.Remove(completed);
    }

    public async ValueTask StopAsync()
    {
        while (true)
        {
            Task[] active;
            lock (_gate)
            {
                _accepting = false;
                _active.RemoveWhere(static candidate => candidate.IsCompleted);
                if (_active.Count == 0) return;
                active = _active.ToArray();
            }

            await Task.WhenAll(active).ConfigureAwait(false);
        }
    }
}
