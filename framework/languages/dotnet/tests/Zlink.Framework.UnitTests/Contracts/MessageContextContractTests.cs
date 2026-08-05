using System.Reflection;
using Microsoft.Extensions.DependencyInjection;
using Microsoft.Extensions.Logging.Abstractions;
using Zlink.Framework.Runtime.Configuration;
using Zlink.Framework.Runtime.Dispatch;

namespace Zlink.Framework.UnitTests;

public sealed class MessageContextContractTests
{
    private static readonly HashSet<string> RemovedContextTypeNames =
    [
        "ZLinkHandlerContext",
        "ZLinkRequestContext",
        "ZLinkSendContext",
        "ZLinkPublishContext",
        "ZLinkSpotActorRequestContext",
        "ZLinkSpotActorSendContext"
    ];

    [Fact]
    public void PublicSurface_ExposesOnlyUnifiedMessageContexts()
    {
        var properties = typeof(IZLinkMessageContext)
            .GetProperties(BindingFlags.Public | BindingFlags.Instance)
            .ToDictionary(static property => property.Name, StringComparer.Ordinal);

        Assert.Equal(6, properties.Count);
        Assert.Equal(typeof(string), properties["MeshName"].PropertyType);
        Assert.Equal(typeof(string), properties["ChannelName"].PropertyType);
        Assert.Equal(typeof(string), properties["PacketName"].PropertyType);
        Assert.Equal(typeof(string), properties["ContentType"].PropertyType);
        Assert.Equal(typeof(ZLinkMessageMetadata), properties["Metadata"].PropertyType);
        Assert.Equal(typeof(string), properties["CorrelationId"].PropertyType);
        Assert.All(properties.Values, static property => Assert.Null(property.SetMethod));

        Assert.DoesNotContain(
            typeof(IZLinkMessageContext).Assembly.GetTypes(),
            static type => RemovedContextTypeNames.Contains(type.Name));
    }

    [Fact]
    public void HandlerFilter_ReceivesFilterSpecificContext()
    {
        Assert.Equal(
            typeof(IZLinkHandlerFilterContext),
            typeof(IZLinkHandlerFilter)
                .GetMethod(nameof(IZLinkHandlerFilter.InvokeAsync))!
                .GetParameters()[0]
                .ParameterType);
        Assert.DoesNotContain(
            typeof(IZLinkHandlerFilter).Assembly.GetTypes(),
            static type => type.Name == "ZLinkHandlerInvocation");
    }

    [Fact]
    public void HandlerSignatures_UseExactMessageContexts()
    {
        Assert.Equal(
            typeof(IZLinkMessageContext),
            Parameters(typeof(IZLinkSendHandler<>))[1]);
        Assert.Equal(
            typeof(IZLinkMessageContext),
            Parameters(typeof(IZLinkRequestHandler<,>))[1]);
        Assert.Equal(
            typeof(ZLinkRouteMessageContext),
            Parameters(typeof(IZLinkRouteSendHandler<>))[1]);
        Assert.Equal(
            typeof(ZLinkRouteMessageContext),
            Parameters(typeof(IZLinkRouteRequestHandler<,>))[1]);
        Assert.Equal(
            typeof(ZLinkPublishMessageContext),
            Parameters(typeof(IZLinkSpotSubscriptionHandler<,>))[2]);

        AssertActorHandlerSignature(typeof(IZLinkSpotActorSendHandler<,,>), messageIndex: 3);
        AssertActorHandlerSignature(typeof(IZLinkSpotActorRequestHandler<,,,>), messageIndex: 3);
        AssertActorHandlerSignature(typeof(IZLinkEntrySpotActorSendHandler<,,>), messageIndex: 3);
        AssertActorHandlerSignature(typeof(IZLinkEntrySpotActorRequestHandler<,,,>), messageIndex: 3);
    }

    [Fact]
    public async Task Dispatcher_FilterAndHandlerShareExactMessageContext()
    {
        var probe = new FilterProbe();
        var registeredDependency = new DispatchDependency();
        var registeredFilter = new CapturingFilter(probe, registeredDependency);
        var registeredHandler = new FilteredRequestHandler(probe, registeredDependency);
        var registration = new ZLinkFrameworkRegistration();
        registration.Filters.Add(typeof(CapturingFilter));
        await using var services = new ServiceCollection()
            .AddSingleton(probe)
            .AddScoped<DispatchDependency>()
            .AddSingleton(registeredFilter)
            .AddSingleton(registeredHandler)
            .BuildServiceProvider();
        var dispatcher = new ZLinkHandlerDispatcher(
            services.GetRequiredService<IServiceScopeFactory>(),
            registration);
        var endpoint = ZLinkHandlerEndpointDescriptorFactory.CreateInterface(
            typeof(FilteredRequestHandler),
            typeof(IZLinkRequestHandler<FilterRequest, FilterReply>),
            ZLinkMessageKind.Request,
            new HashSet<string>(StringComparer.Ordinal),
            "channel-a",
            "filter.request");
        var metadata = new ZLinkMessageMetadata(
            new Dictionary<string, string>(StringComparer.Ordinal)
            {
                ["tenant"] = "alpha"
            });
        var context = new ZLinkMessageContext(
            "mesh-a",
            "channel-a",
            "filter.request",
            "application/json",
            metadata,
            "correlation-a");

        var dispatch = await dispatcher.DispatchAsync(
            endpoint,
            new FilterRequest("value"),
            context,
            ZLinkHandlerDispatchKind.ChannelRequest,
            CancellationToken.None);
        var result = Assert.IsType<FilterReply>(dispatch.Value);

        Assert.Equal("VALUE", result.Value);
        var filterContext =
            Assert.IsAssignableFrom<IZLinkHandlerFilterContext>(
                probe.FilterContext);
        Assert.Equal(
            ZLinkHandlerDispatchKind.ChannelRequest,
            filterContext.DispatchKind);
        Assert.Equal(context.MeshName, filterContext.MeshName);
        Assert.Equal(context.ChannelName, filterContext.ChannelName);
        var handlerContext = Assert.IsAssignableFrom<IZLinkMessageContext>(probe.HandlerContext);
        Assert.Same(context, handlerContext);
        Assert.Equal("alpha", handlerContext.Metadata.Find("tenant"));
        Assert.Equal("correlation-a", handlerContext.CorrelationId);
        Assert.NotSame(registeredFilter, probe.Filter);
        Assert.NotSame(registeredHandler, probe.Handler);
        Assert.Same(probe.FilterDependency, probe.HandlerDependency);
        Assert.Equal(1, probe.Filter!.DisposeCount);
        Assert.Equal(1, probe.Handler!.DisposeCount);
        Assert.Equal(1, probe.FilterDependency!.DisposeCount);
    }

    [Fact]
    public async Task Dispatcher_FilterCanStopARequestWithoutInvokingTheHandler()
    {
        var probe = new FilterControlProbe();
        var registration = new ZLinkFrameworkRegistration();
        registration.Filters.Add(typeof(ShortCircuitFilter));
        await using var services = new ServiceCollection()
            .AddSingleton(probe)
            .BuildServiceProvider();
        var dispatcher = new ZLinkHandlerDispatcher(
            services.GetRequiredService<IServiceScopeFactory>(),
            registration);

        var result = await dispatcher.DispatchAsync(
            CreateControlEndpoint(),
            new FilterRequest("value"),
            CreateControlContext(),
            ZLinkHandlerDispatchKind.ChannelRequest,
            CancellationToken.None);

        Assert.False(result.HandlerInvoked);
        Assert.Null(result.Value);
        Assert.Equal(0, probe.HandlerCalls);
    }

    [Fact]
    public async Task Dispatcher_RejectsASecondNextWithoutRunningTheHandlerTwice()
    {
        var probe = new FilterControlProbe();
        var registration = new ZLinkFrameworkRegistration();
        registration.Filters.Add(typeof(DoubleNextFilter));
        await using var services = new ServiceCollection()
            .AddSingleton(probe)
            .BuildServiceProvider();
        var dispatcher = new ZLinkHandlerDispatcher(
            services.GetRequiredService<IServiceScopeFactory>(),
            registration);

        var error = await Assert.ThrowsAsync<ZLinkFrameworkException>(
            () => dispatcher.DispatchAsync(
                    CreateControlEndpoint(),
                    new FilterRequest("value"),
                    CreateControlContext(),
                    ZLinkHandlerDispatchKind.ChannelRequest,
                    CancellationToken.None)
                .AsTask());

        Assert.Equal(ZLinkFrameworkErrorKind.InvalidOperation, error.Kind);
        Assert.Equal(1, probe.HandlerCalls);
    }

    [Fact]
    public async Task Dispatcher_AtomicallyRejectsConcurrentNextCalls()
    {
        var probe = new FilterControlProbe();
        var registration = new ZLinkFrameworkRegistration();
        registration.Filters.Add(typeof(ConcurrentNextFilter));
        await using var services = new ServiceCollection()
            .AddSingleton(probe)
            .BuildServiceProvider();
        var dispatcher = new ZLinkHandlerDispatcher(
            services.GetRequiredService<IServiceScopeFactory>(),
            registration);

        var error = await Assert.ThrowsAsync<ZLinkFrameworkException>(
            () => dispatcher.DispatchAsync(
                    CreateControlEndpoint(),
                    new FilterRequest("value"),
                    CreateControlContext(),
                    ZLinkHandlerDispatchKind.ChannelRequest,
                    CancellationToken.None)
                .AsTask());

        Assert.Equal(ZLinkFrameworkErrorKind.InvalidOperation, error.Kind);
        Assert.Equal(1, probe.HandlerCalls);
    }

    [Fact]
    public async Task Dispatcher_ShortCircuitAndCancellationDisposeTheDispatchScope()
    {
        var probe = new FilterLifetimeProbe();
        var registration = new ZLinkFrameworkRegistration();
        registration.Filters.Add(typeof(LifetimeFilter));
        await using var services = new ServiceCollection()
            .AddSingleton(probe)
            .AddScoped<DispatchDependency>()
            .BuildServiceProvider();
        var dispatcher = new ZLinkHandlerDispatcher(
            services.GetRequiredService<IServiceScopeFactory>(),
            registration);

        await dispatcher.DispatchAsync(
            CreateControlEndpoint(),
            new FilterRequest("value"),
            CreateControlContext(),
            ZLinkHandlerDispatchKind.ChannelRequest,
            CancellationToken.None);

        Assert.Equal(1, probe.FilterDisposeCount);
        Assert.Equal(1, probe.Dependency!.DisposeCount);

        using var cancellation = new CancellationTokenSource();
        cancellation.Cancel();
        await Assert.ThrowsAnyAsync<OperationCanceledException>(
            () => dispatcher.DispatchAsync(
                    CreateControlEndpoint(),
                    new FilterRequest("value"),
                    CreateControlContext(),
                    ZLinkHandlerDispatchKind.ChannelRequest,
                    cancellation.Token)
                .AsTask());

        Assert.Equal(2, probe.FilterDisposeCount);
        Assert.Equal(1, probe.Dependency.DisposeCount);
        Assert.Equal(1, probe.LastDependency!.DisposeCount);
    }

    [Fact]
    public async Task ChannelRequest_FilterStopWritesRejectedErrorReply()
    {
        var probe = new FilterControlProbe();
        var registration = new ZLinkFrameworkRegistration();
        registration.Filters.Add(typeof(ShortCircuitFilter));
        await using var services = new ServiceCollection()
            .AddSingleton(probe)
            .BuildServiceProvider();
        var endpoint = CreateControlEndpoint();
        var dispatcher = new ZLinkHandlerDispatcher(
            services.GetRequiredService<IServiceScopeFactory>(),
            registration);
        var pipeline = new ZLinkChannelRequestDispatchPipeline(
            "mesh-a",
            new ZLinkHandlerRegistry([endpoint]),
            dispatcher,
            static _ => new HashSet<string>(StringComparer.Ordinal),
            registration.Codecs,
            new ZLinkDispatchErrorReporter(new ZLinkDispatchOptionsModel()),
            NullLogger.Instance);
        var header = new ZLinkEnvelopeHeader(
            ZLinkMessageKind.Request,
            "channel-a",
            "filter.request",
            ZLinkEnvelopeCodec.DefaultContentType,
            "correlation-a",
            null,
            null,
            null,
            null);
        var parts = ZLinkEnvelopeCodec.EncodeParts(
            header,
            new FilterRequest("value"),
            typeof(FilterRequest),
            registration.Codecs);
        ZLinkEnvelopeHeader? errorReply = null;
        var normalReply = false;
        try
        {
            await pipeline.DispatchAsync(
                "channel-a",
                parts,
                header,
                (_, _, _) =>
                {
                    normalReply = true;
                    return ValueTask.CompletedTask;
                },
                reply =>
                {
                    errorReply = reply;
                    return ValueTask.CompletedTask;
                },
                CancellationToken.None);
        }
        finally
        {
            ZLinkMessageParts.DisposeAll(parts);
        }

        Assert.False(normalReply);
        Assert.NotNull(errorReply);
        Assert.Equal(
            nameof(ZLinkFrameworkErrorKind.Rejected),
            errorReply!.ErrorCode);
        Assert.Equal(0, probe.HandlerCalls);
    }

    private static ZLinkHandlerEndpointDescriptor CreateControlEndpoint()
    {
        return ZLinkHandlerEndpointDescriptorFactory.CreateInterface(
            typeof(CountingRequestHandler),
            typeof(IZLinkRequestHandler<FilterRequest, FilterReply>),
            ZLinkMessageKind.Request,
            new HashSet<string>(StringComparer.Ordinal),
            "channel-a",
            "filter.request");
    }

    private static ZLinkMessageContext CreateControlContext()
    {
        return new ZLinkMessageContext(
            "mesh-a",
            "channel-a",
            "filter.request",
            "application/json",
            metadata: null,
            "correlation-a");
    }

    private static Type[] Parameters(Type handlerType)
    {
        return handlerType
            .GetMethod("HandleAsync")!
            .GetParameters()
            .Select(static parameter => parameter.ParameterType)
            .ToArray();
    }

    private static void AssertActorHandlerSignature(Type handlerType, int messageIndex)
    {
        var genericArguments = handlerType.GetGenericArguments();
        var parameters = Parameters(handlerType);

        Assert.Equal(genericArguments[0], parameters[0]);
        Assert.Equal(genericArguments[1], parameters[1]);
        Assert.Equal(typeof(IZLinkMessageContext), parameters[2]);
        Assert.Equal(genericArguments[2], parameters[messageIndex]);
        Assert.Equal(typeof(CancellationToken), parameters[^1]);
    }

    private sealed record FilterRequest(string Value);

    private sealed record FilterReply(string Value);

    private sealed class FilterProbe
    {
        public IZLinkMessageContext? FilterContext { get; set; }

        public IZLinkMessageContext? HandlerContext { get; set; }

        public CapturingFilter? Filter { get; set; }

        public FilteredRequestHandler? Handler { get; set; }

        public DispatchDependency? FilterDependency { get; set; }

        public DispatchDependency? HandlerDependency { get; set; }
    }

    private sealed class DispatchDependency : IDisposable
    {
        public int DisposeCount { get; private set; }

        public void Dispose() => DisposeCount++;
    }

    private sealed class FilterControlProbe
    {
        public int HandlerCalls;
    }

    private sealed class FilterLifetimeProbe
    {
        public int FilterDisposeCount;

        public DispatchDependency? Dependency { get; set; }

        public DispatchDependency? LastDependency { get; set; }
    }

    private sealed class ShortCircuitFilter : IZLinkHandlerFilter
    {
        public ValueTask InvokeAsync(
            IZLinkHandlerFilterContext context,
            ZLinkHandlerFilterNext next,
            CancellationToken cancellationToken)
        {
            _ = context;
            _ = next;
            cancellationToken.ThrowIfCancellationRequested();
            return ValueTask.CompletedTask;
        }
    }

    private sealed class DoubleNextFilter : IZLinkHandlerFilter
    {
        public async ValueTask InvokeAsync(
            IZLinkHandlerFilterContext context,
            ZLinkHandlerFilterNext next,
            CancellationToken cancellationToken)
        {
            _ = context;
            cancellationToken.ThrowIfCancellationRequested();
            await next();
            await next();
        }
    }

    private sealed class ConcurrentNextFilter : IZLinkHandlerFilter
    {
        public async ValueTask InvokeAsync(
            IZLinkHandlerFilterContext context,
            ZLinkHandlerFilterNext next,
            CancellationToken cancellationToken)
        {
            _ = context;
            cancellationToken.ThrowIfCancellationRequested();
            var first = next().AsTask();
            var second = next().AsTask();
            await Task.WhenAll(first, second);
        }
    }

    private sealed class LifetimeFilter(
        FilterLifetimeProbe probe,
        DispatchDependency dependency) : IZLinkHandlerFilter, IDisposable
    {
        public ValueTask InvokeAsync(
            IZLinkHandlerFilterContext context,
            ZLinkHandlerFilterNext next,
            CancellationToken cancellationToken)
        {
            _ = context;
            _ = next;
            probe.LastDependency = dependency;
            probe.Dependency ??= dependency;
            cancellationToken.ThrowIfCancellationRequested();
            return ValueTask.CompletedTask;
        }

        public void Dispose() => Interlocked.Increment(ref probe.FilterDisposeCount);
    }

    private sealed class CountingRequestHandler(FilterControlProbe probe)
        : IZLinkRequestHandler<FilterRequest, FilterReply>
    {
        public ValueTask<FilterReply> HandleAsync(
            FilterRequest request,
            IZLinkMessageContext context,
            CancellationToken cancellationToken)
        {
            _ = context;
            cancellationToken.ThrowIfCancellationRequested();
            Interlocked.Increment(ref probe.HandlerCalls);
            return ValueTask.FromResult(new FilterReply(request.Value));
        }
    }

    private sealed class CapturingFilter(
        FilterProbe probe,
        DispatchDependency dependency) : IZLinkHandlerFilter, IDisposable
    {
        public int DisposeCount { get; private set; }

        public async ValueTask InvokeAsync(
            IZLinkHandlerFilterContext context,
            ZLinkHandlerFilterNext next,
            CancellationToken cancellationToken)
        {
            cancellationToken.ThrowIfCancellationRequested();
            probe.FilterContext = context;
            probe.Filter = this;
            probe.FilterDependency = dependency;
            await next();
        }

        public void Dispose() => DisposeCount++;
    }

    private sealed class FilteredRequestHandler(
        FilterProbe probe,
        DispatchDependency dependency)
        : IZLinkRequestHandler<FilterRequest, FilterReply>, IDisposable
    {
        public int DisposeCount { get; private set; }

        public ValueTask<FilterReply> HandleAsync(
            FilterRequest request,
            IZLinkMessageContext context,
            CancellationToken cancellationToken)
        {
            cancellationToken.ThrowIfCancellationRequested();
            probe.HandlerContext = context;
            probe.Handler = this;
            probe.HandlerDependency = dependency;
            return ValueTask.FromResult(
                new FilterReply(request.Value.ToUpperInvariant()));
        }

        public void Dispose() => DisposeCount++;
    }
}
