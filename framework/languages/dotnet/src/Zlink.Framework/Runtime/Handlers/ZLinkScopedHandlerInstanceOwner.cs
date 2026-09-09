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
                var completion = new TaskCompletionSource(
                    TaskCreationOptions.RunContinuationsAsynchronously);
                _disposeTask = completion.Task;
                return (Task: _disposeTask, Completion: completion, Instances: instances);
            }
            return (Task: _disposeTask, Completion: (TaskCompletionSource?)null, Instances: (object[]?)null);
        }));
        if (result.Completion is not null)
            StartDisposeCore(result.Completion, result.Instances!);
        return new ValueTask(result.Task);
    }

    private static T AwaitStateLane<T>(ValueTask<T> operation) =>
        operation.GetAwaiter().GetResult();

    private static void StartDisposeCore(TaskCompletionSource completion, object[] instances)
    {
        if (ExecutionContext.IsFlowSuppressed())
        {
            _ = DisposeCoreAsync(completion, instances);
            return;
        }

        using (ExecutionContext.SuppressFlow())
            _ = DisposeCoreAsync(completion, instances);
    }

    private static async Task DisposeCoreAsync(TaskCompletionSource completion, object[] instances)
    {
        try
        {
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
            completion.TrySetResult();
        }
        catch (OperationCanceledException exception)
        {
            completion.TrySetCanceled(exception.CancellationToken);
        }
        catch (Exception exception)
        {
            completion.TrySetException(exception);
        }
    }
}
