using Microsoft.Extensions.DependencyInjection;
using Zlink.Framework.Runtime.Handlers;

namespace Zlink.Framework.Runtime.Actors;

internal sealed class ZLinkActorHandlerActivation : IAsyncDisposable
{
    private readonly AsyncServiceScope _scope;
    private readonly ZLinkScopedHandlerInstanceOwner _instances;
    private readonly object _gate = new();
    private Task? _disposeTask;

    public ZLinkActorHandlerActivation(IServiceProvider services)
    {
        _scope = services.CreateAsyncScope();
        _instances = new ZLinkScopedHandlerInstanceOwner(_scope.ServiceProvider);
    }

    public ZLinkScopedHandlerInstanceOwner Instances => _instances;

    public ValueTask DisposeAsync()
    {
        Task task;
        lock (_gate)
            task = _disposeTask ??= DisposeCoreAsync();
        return new ValueTask(task);
    }

    private async Task DisposeCoreAsync()
    {
        var failures = new List<Exception>();
        try
        {
            await _instances.DisposeAsync().ConfigureAwait(false);
        }
        catch (Exception exception)
        {
            failures.Add(exception);
        }

        try
        {
            await _scope.DisposeAsync().ConfigureAwait(false);
        }
        catch (Exception exception)
        {
            failures.Add(exception);
        }

        if (failures.Count == 1)
            System.Runtime.ExceptionServices.ExceptionDispatchInfo.Capture(failures[0]).Throw();
        if (failures.Count > 1) throw new AggregateException(failures);
    }
}
