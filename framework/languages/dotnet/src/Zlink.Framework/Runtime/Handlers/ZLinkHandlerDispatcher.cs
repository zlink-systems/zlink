using Microsoft.Extensions.DependencyInjection;

namespace Zlink.Framework.Runtime.Handlers;

internal sealed class ZLinkHandlerDispatcher(
    IServiceScopeFactory scopeFactory,
    ZLinkFrameworkRegistration registration,
    ZLinkDispatchErrorReporter? channelDispatchErrors = null
)
{
    // Every scope of this factory shares one DI service catalog, so the
    // dispatcher looks the catalog up once and each dispatch reads the handler
    // factory from it (spec 08 §4.3, construction decided before dispatch).
    private static readonly object HandlerNotInvoked = new();
    private ZLinkScopedHandlerInstanceOwner.DispatchCatalog? _catalog;
    private readonly ZLinkDispatchErrorReporter _channelDispatchErrors =
        channelDispatchErrors ?? new ZLinkDispatchErrorReporter(registration.DispatchOptions);

    public ValueTask<ZLinkHandlerDispatchResult> DispatchAsync(
        ZLinkHandlerEndpointDescriptor endpoint,
        object? message,
        IZLinkMessageContext context,
        ZLinkHandlerDispatchKind dispatchKind,
        CancellationToken cancellationToken
    )
    {
        return DispatchCoreAsync(
            endpoint.DeclaringType,
            endpoint.Invoker,
            endpoint.ArgumentPlan,
            message,
            context,
            dispatchKind,
            cancellationToken
        );
    }

    public ValueTask<ZLinkHandlerDispatchResult> DispatchRouteAsync(
        ZLinkRouteHandlerDescriptor descriptor,
        object? message,
        ZLinkRouteMessageContext context,
        ZLinkHandlerDispatchKind dispatchKind,
        CancellationToken cancellationToken
    )
    {
        return DispatchCoreAsync(
            descriptor.HandlerType,
            descriptor.Invoker,
            ZLinkHandlerContractDescriptorSupport.HandleAsyncArgumentPlan,
            message,
            context,
            dispatchKind,
            cancellationToken
        );
    }

    private ValueTask<ZLinkHandlerDispatchResult> DispatchCoreAsync(
        Type handlerType,
        ZLinkHandlerMethodInvoker invoker,
        IReadOnlyList<ZLinkHandlerArgumentKind> argumentPlan,
        object? message,
        IZLinkMessageContext context,
        ZLinkHandlerDispatchKind dispatchKind,
        CancellationToken cancellationToken
    ) =>
        registration.Filters.Count == 0
            ? InvokeUnfilteredAsync(
                handlerType,
                invoker,
                argumentPlan,
                message,
                context,
                dispatchKind,
                cancellationToken
            )
            : InvokeFilteredAsync(
                handlerType,
                invoker,
                argumentPlan,
                message,
                context,
                dispatchKind,
                cancellationToken
            );

    private async ValueTask<ZLinkHandlerDispatchResult> InvokeUnfilteredAsync(
        Type handlerType,
        ZLinkHandlerMethodInvoker invoker,
        IReadOnlyList<ZLinkHandlerArgumentKind> argumentPlan,
        object? message,
        IZLinkMessageContext context,
        ZLinkHandlerDispatchKind dispatchKind,
        CancellationToken cancellationToken
    )
    {
        await using var scope = scopeFactory.CreateAsyncScope();
        var services = scope.ServiceProvider;
        var catalog =
            Volatile.Read(ref _catalog)
            ?? Interlocked.CompareExchange(ref _catalog, new(services), null)
            ?? _catalog!;
        var handler = catalog.Create(services, handlerType);
        try
        {
            if (
                dispatchKind
                    is ZLinkHandlerDispatchKind.ChannelRequest
                        or ZLinkHandlerDispatchKind.ChannelSend
                && _channelDispatchErrors.Flow.Enabled(ZLinkMessageFlowOutcome.Dispatched)
            )
            {
                new ZLinkDispatchFlowScope(
                    context is ZLinkRouteMessageContext
                        ? ZLinkDispatchErrorSurface.RouteMeshChannel
                        : ZLinkDispatchErrorSurface.Channel,
                    _channelDispatchErrors.Flow.CaptureEnabled,
                    dispatchKind == ZLinkHandlerDispatchKind.ChannelRequest
                        ? ZLinkDispatchMessageKind.Request
                        : ZLinkDispatchMessageKind.Send,
                    context.PacketName,
                    context.ChannelName,
                    context.ContentType,
                    context.CorrelationId
                ).Trace(_channelDispatchErrors, ZLinkMessageFlowOutcome.Dispatched);
            }
            var result = await ZLinkHandlerInvocationEngine
                .InvokeAsync(handler, invoker, argumentPlan, message, context, cancellationToken)
                .ConfigureAwait(false);
            return new ZLinkHandlerDispatchResult(true, result);
        }
        finally
        {
            if (handler is IAsyncDisposable asyncDisposable)
                await asyncDisposable.DisposeAsync().ConfigureAwait(false);
            else if (handler is IDisposable disposable)
                disposable.Dispose();
        }
    }

    private async ValueTask<ZLinkHandlerDispatchResult> InvokeFilteredAsync(
        Type handlerType,
        ZLinkHandlerMethodInvoker invoker,
        IReadOnlyList<ZLinkHandlerArgumentKind> argumentPlan,
        object? message,
        IZLinkMessageContext context,
        ZLinkHandlerDispatchKind dispatchKind,
        CancellationToken cancellationToken
    )
    {
        await using var scope = scopeFactory.CreateAsyncScope();
        await using var instances = new ZLinkScopedHandlerInstanceOwner(scope.ServiceProvider);

        object? result = HandlerNotInvoked;
        var filterContext = new ZLinkHandlerFilterContext(context, dispatchKind);
        var pipeline = BuildPipeline(
            filterContext,
            instances,
            async ct =>
            {
                result = null;
                var handler = instances.Resolve(handlerType);
                if (
                    filterContext.DispatchKind
                        is ZLinkHandlerDispatchKind.ChannelRequest
                            or ZLinkHandlerDispatchKind.ChannelSend
                    && _channelDispatchErrors.Flow.Enabled(ZLinkMessageFlowOutcome.Dispatched)
                )
                {
                    new ZLinkDispatchFlowScope(
                        filterContext.Message is ZLinkRouteMessageContext
                            ? ZLinkDispatchErrorSurface.RouteMeshChannel
                            : ZLinkDispatchErrorSurface.Channel,
                        _channelDispatchErrors.Flow.CaptureEnabled,
                        filterContext.DispatchKind == ZLinkHandlerDispatchKind.ChannelRequest
                            ? ZLinkDispatchMessageKind.Request
                            : ZLinkDispatchMessageKind.Send,
                        filterContext.PacketName,
                        filterContext.ChannelName,
                        filterContext.ContentType,
                        filterContext.CorrelationId
                    ).Trace(_channelDispatchErrors, ZLinkMessageFlowOutcome.Dispatched);
                }
                result = await ZLinkHandlerInvocationEngine
                    .InvokeAsync(handler, invoker, argumentPlan, message, filterContext.Message, ct)
                    .ConfigureAwait(false);
            },
            cancellationToken
        );
        await pipeline().ConfigureAwait(false);
        return ReferenceEquals(result, HandlerNotInvoked)
            ? new ZLinkHandlerDispatchResult(false, null)
            : new ZLinkHandlerDispatchResult(true, result);
    }

    private ZLinkHandlerFilterNext BuildPipeline(
        IZLinkHandlerFilterContext context,
        ZLinkScopedHandlerInstanceOwner instances,
        Func<CancellationToken, ValueTask> invokeHandler,
        CancellationToken cancellationToken
    )
    {
        ZLinkHandlerFilterNext pipeline = () => invokeHandler(cancellationToken);

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

    private static ZLinkHandlerFilterNext InvokeAtMostOnce(ZLinkHandlerFilterNext next)
    {
        var invoked = 0;
        return () =>
        {
            if (Interlocked.Exchange(ref invoked, 1) == 0)
                return next();

            return ValueTask.FromException(
                new ZLinkFrameworkException(
                    ZLinkFrameworkErrorKind.InvalidOperation,
                    "A handler filter cannot invoke next more than once."
                )
            );
        };
    }
}

internal readonly record struct ZLinkHandlerDispatchResult(bool HandlerInvoked, object? Value);
