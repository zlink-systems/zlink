using Microsoft.Extensions.DependencyInjection;
using Zlink.Framework.Runtime.Execution;

namespace Zlink.Framework.Runtime.Handlers;

internal sealed class ZLinkScopedHandlerInstanceOwner(IServiceProvider services) : IAsyncDisposable
{
    private readonly Dictionary<Type, object> _fallbackInstances = new();
    private readonly ZLinkStateLane _lane = new();
    private bool _disposed;
    private Task? _disposeTask;

    internal IServiceProvider Services { get; } = services;

    public THandler Resolve<THandler>()
        where THandler : class
    {
        return (THandler)Resolve(typeof(THandler));
    }

    public object Resolve(Type handlerType)
    {
        ArgumentNullException.ThrowIfNull(handlerType);

        return AwaitStateLane(_lane.RunAsync(() =>
        {
            ObjectDisposedException.ThrowIf(_disposed, this);

            if (_fallbackInstances.TryGetValue(handlerType, out var existing)) return existing;

            var created = ActivatorUtilities.CreateInstance(Services, handlerType);
            _fallbackInstances.Add(handlerType, created);
            return created;
        }));
    }

    public ValueTask DisposeAsync()
    {
        var result = AwaitStateLane(_lane.RunAsync(() =>
        {
            if (_disposeTask is null)
            {
                _disposed = true;
                var instances = _fallbackInstances.Values.Reverse().ToArray();
                _fallbackInstances.Clear();
                var start = new TaskCompletionSource(
                    TaskCreationOptions.RunContinuationsAsynchronously);
                using (ExecutionContext.SuppressFlow())
                    _disposeTask = DisposeCoreAsync(start.Task, instances);
                return (Task: _disposeTask, Start: start);
            }
            return (Task: _disposeTask, Start: (TaskCompletionSource?)null);
        }));
        result.Start?.TrySetResult();
        return new ValueTask(result.Task);
    }

    private static T AwaitStateLane<T>(ValueTask<T> operation) =>
        operation.GetAwaiter().GetResult();

    private static async Task DisposeCoreAsync(Task started, object[] instances)
    {
        await started.ConfigureAwait(false);
        List<Exception>? failures = null;
        foreach (var instance in instances)
            try
            {
                if (instance is IAsyncDisposable asyncDisposable)
                    await asyncDisposable.DisposeAsync().ConfigureAwait(false);
                else if (instance is IDisposable disposable)
                    disposable.Dispose();
            }
            catch (Exception exception)
            {
                (failures ??= []).Add(exception);
            }

        if (failures is { Count: 1 })
            System.Runtime.ExceptionServices.ExceptionDispatchInfo.Capture(failures[0]).Throw();
        if (failures is { Count: > 1 }) throw new AggregateException(failures);
    }
}
