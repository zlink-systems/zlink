using Microsoft.Extensions.DependencyInjection;

namespace Zlink.Framework.Runtime.Handlers;

internal sealed class ZLinkScopedHandlerInstanceOwner(IServiceProvider services) : IAsyncDisposable
{
    private readonly Dictionary<Type, object> _fallbackInstances = new();
    private readonly object _gate = new();
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

        lock (_gate)
        {
            ObjectDisposedException.ThrowIf(_disposed, this);

            if (_fallbackInstances.TryGetValue(handlerType, out var existing)) return existing;

            var created = ActivatorUtilities.CreateInstance(Services, handlerType);
            _fallbackInstances.Add(handlerType, created);
            return created;
        }
    }

    public ValueTask DisposeAsync()
    {
        Task task;
        TaskCompletionSource? start = null;
        lock (_gate)
        {
            if (_disposeTask is null)
            {
                _disposed = true;
                var instances = _fallbackInstances.Values.Reverse().ToArray();
                _fallbackInstances.Clear();
                start = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
                _disposeTask = DisposeCoreAsync(start.Task, instances);
            }
            task = _disposeTask;
        }
        start?.TrySetResult();
        return new ValueTask(task);
    }

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
