using Microsoft.Extensions.DependencyInjection;

namespace Zlink.Framework.Runtime.Handlers;

internal sealed class ZLinkHandlerDispatcher(
    IServiceScopeFactory scopeFactory,
    ZLinkFrameworkRegistration registration)
{
    public ValueTask<ZLinkHandlerDispatchResult> DispatchAsync(
        ZLinkHandlerEndpointDescriptor endpoint,
        object? message,
        IZLinkMessageContext context,
        ZLinkHandlerDispatchKind dispatchKind,
        CancellationToken cancellationToken)
    {
        return DispatchCoreAsync(
            context,
            dispatchKind,
            (instances, ct) => InvokeHandlerAsync(
                endpoint,
                message,
                context,
                instances,
                ct),
            cancellationToken);
    }

    public ValueTask<ZLinkHandlerDispatchResult> DispatchRouteAsync(
        ZLinkRouteHandlerDescriptor descriptor,
        object? message,
        ZLinkRouteMessageContext context,
        ZLinkHandlerDispatchKind dispatchKind,
        CancellationToken cancellationToken)
    {
        return DispatchCoreAsync(
            context,
            dispatchKind,
            async (instances, ct) =>
            {
                var handler = instances.Resolve(descriptor.HandlerType);
                return await ZLinkHandlerInvocationEngine.InvokeAsync(
                        handler,
                        descriptor.Invoker,
                        3,
                        arguments =>
                        {
                            arguments[0] = message;
                            arguments[1] = context;
                            arguments[2] = ct;
                        })
                    .ConfigureAwait(false);
            },
            cancellationToken);
    }

    private async ValueTask<ZLinkHandlerDispatchResult> DispatchCoreAsync(
        IZLinkMessageContext context,
        ZLinkHandlerDispatchKind dispatchKind,
        Func<ZLinkScopedHandlerInstanceOwner, CancellationToken, ValueTask<object?>>
            invokeHandler,
        CancellationToken cancellationToken)
    {
        await using var scope = scopeFactory.CreateAsyncScope();
        await using var instances =
            new ZLinkScopedHandlerInstanceOwner(scope.ServiceProvider);
        object? result = null;
        var handlerInvoked = false;
        var filterContext = new ZLinkHandlerFilterContext(context, dispatchKind);
        var pipeline = BuildPipeline(
            filterContext,
            instances,
            async ct =>
            {
                handlerInvoked = true;
                result = await invokeHandler(instances, ct).ConfigureAwait(false);
            },
            cancellationToken);
        await pipeline().ConfigureAwait(false);
        return new ZLinkHandlerDispatchResult(handlerInvoked, result);
    }

    private ZLinkHandlerFilterNext BuildPipeline(
        IZLinkHandlerFilterContext context,
        ZLinkScopedHandlerInstanceOwner instances,
        Func<CancellationToken, ValueTask> invokeHandler,
        CancellationToken cancellationToken)
    {
        ZLinkHandlerFilterNext pipeline =
            () => invokeHandler(cancellationToken);

        for (var index = registration.Filters.Count - 1; index >= 0; index--)
        {
            var next = InvokeAtMostOnce(pipeline);
            var filterType = registration.Filters[index];
            pipeline = async () =>
            {
                var filter = (IZLinkHandlerFilter)instances.Resolve(filterType);
                await filter.InvokeAsync(context, next, cancellationToken).ConfigureAwait(false);
            };
        }

        return pipeline;
    }

    private static ZLinkHandlerFilterNext InvokeAtMostOnce(
        ZLinkHandlerFilterNext next)
    {
        var invoked = 0;
        return () =>
        {
            if (Interlocked.Exchange(ref invoked, 1) == 0)
                return next();

            return ValueTask.FromException(
                new ZLinkFrameworkException(
                    ZLinkFrameworkErrorKind.InvalidOperation,
                    "A handler filter cannot invoke next more than once."));
        };
    }

    private static async ValueTask<object?> InvokeHandlerAsync(
        ZLinkHandlerEndpointDescriptor endpoint,
        object? message,
        IZLinkMessageContext context,
        ZLinkScopedHandlerInstanceOwner instances,
        CancellationToken cancellationToken)
    {
        var handler = instances.Resolve(endpoint.DeclaringType);
        return await ZLinkHandlerInvocationEngine.InvokeAsync(
                handler,
                endpoint.Invoker,
                endpoint.ArgumentPlan,
                message,
                context,
                cancellationToken)
            .ConfigureAwait(false);
    }

}

internal readonly record struct ZLinkHandlerDispatchResult(
    bool HandlerInvoked,
    object? Value);
