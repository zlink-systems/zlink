using System.Collections.Concurrent;
using Microsoft.Extensions.DependencyInjection;
using Microsoft.Extensions.Logging;
using Zlink.Framework.Runtime.Backend.Contracts;
using Zlink.Framework.Runtime.Dispatch;
using Zlink.Framework.Runtime.Execution;

namespace Zlink.Framework.UnitTests.Runtime;

public sealed partial class EntrySpotActorDispatchTests
{
    [Fact]
    public async Task MeshNode_Channel_Request_Emits_Received_Then_Replied_With_Wire_Identity()
    {
        var membership = new ZLinkMeshChannelMembership { ChannelName = "play" };
        membership.RequestHandlers.Add(new ZLinkChannelHandlerRegistration(
            typeof(MeshChannelRequestHandler),
            typeof(MeshRequest),
            typeof(MeshReply),
            "ExactRequest"));

        var result = await DispatchMeshRequestAsync(
            membership,
            routeHandler: null,
            channelName: "play",
            expectedSurface: ZLinkDispatchErrorSurface.Channel);

        Assert.Equal("CHANNEL", result);
    }

    [Fact]
    public async Task MeshNode_Channel_Request_Dispatches_Scanned_Handler_Group()
    {
        var membership = new ZLinkMeshChannelMembership { ChannelName = "play" };
        membership.HandlerGroups.Add("mesh-node-auto-test");

        var result = await DispatchMeshRequestAsync(
            membership,
            routeHandler: null,
            channelName: "play",
            expectedSurface: ZLinkDispatchErrorSurface.Channel,
            packetName: nameof(MeshAutoRequest));

        Assert.Equal("AUTO", result);
    }

    [Fact]
    public async Task MeshNode_Channel_Request_Context_Includes_Source_Node_Rid()
    {
        var membership = new ZLinkMeshChannelMembership { ChannelName = "play" };
        membership.RequestHandlers.Add(new ZLinkChannelHandlerRegistration(
            typeof(MeshChannelRequestHandler),
            typeof(MeshRequest),
            typeof(MeshReply),
            "ExactRequest"));

        var result = await DispatchMeshRequestAsync(
            membership,
            routeHandler: null,
            channelName: "play",
            expectedSurface: ZLinkDispatchErrorSurface.Channel,
            expectedChannelSourceNodeRid: RoutingId.From("source-node"));

        Assert.Equal("CHANNEL", result);
    }

    [Fact]
    public async Task MeshNode_Rid_Request_Emits_Received_Then_Replied_With_Wire_Identity()
    {
        var routeHandler = new ZLinkRouteHandlerRegistration(
            typeof(MeshRouteRequestHandler),
            typeof(MeshRequest),
            typeof(MeshReply),
            "ExactRequest");

        var result = await DispatchMeshRequestAsync(
            membership: null,
            routeHandler,
            channelName: null,
            expectedSurface: ZLinkDispatchErrorSurface.RouteMeshChannel);

        Assert.Equal("ROUTE", result);
    }

    [Fact]
    public async Task MeshNode_Rid_Request_Context_Uses_Registered_Mesh_Name()
    {
        var routeHandler = new ZLinkRouteHandlerRegistration(
            typeof(MeshRouteRequestHandler),
            typeof(MeshRequest),
            typeof(MeshReply),
            "ExactRequest");

        var result = await DispatchMeshRequestAsync(
            membership: null,
            routeHandler,
            channelName: null,
            expectedSurface: ZLinkDispatchErrorSurface.RouteMeshChannel,
            expectedRouteMeshName: "mesh");

        Assert.Equal("ROUTE", result);
    }

    [Fact]
    public async Task MeshNode_Channel_Request_Handler_Failure_Logs_Wire_Flow_And_Correlation()
    {
        var membership = new ZLinkMeshChannelMembership { ChannelName = "play" };
        membership.RequestHandlers.Add(new ZLinkChannelHandlerRegistration(
            typeof(FailingMeshChannelRequestHandler),
            typeof(MeshRequest),
            typeof(MeshReply),
            "FailingExactRequest"));

        var result = await DispatchMeshRequestAsync(
            membership,
            routeHandler: null,
            channelName: "play",
            expectedSurface: ZLinkDispatchErrorSurface.Channel,
            packetName: "FailingExactRequest",
            expectHandlerError: true);

        Assert.Equal(nameof(ZLinkMessageKind.Error), result);
    }

    //  Spec 06 §13.1 splits the two refusals: `Rejected` is an application
    //  policy decision, while new admission closed by host shutdown is
    //  `ShuttingDown`. The drain seal is the second, and a select-one caller
    //  relies on that classification to choose another eligible member.
    [Fact]
    public async Task MeshNode_Channel_Request_Reports_ShuttingDown_After_Drain_Admission_Is_Sealed()
    {
        var membership = new ZLinkMeshChannelMembership { ChannelName = "play" };
        membership.RequestHandlers.Add(new ZLinkChannelHandlerRegistration(
            typeof(MeshChannelRequestHandler),
            typeof(MeshRequest),
            typeof(MeshReply),
            "ExactRequest"));

        var result = await DispatchMeshRequestAsync(
            membership,
            routeHandler: null,
            channelName: "play",
            expectedSurface: ZLinkDispatchErrorSurface.Channel,
            sealAdmission: true);

        Assert.Equal(nameof(ZLinkFrameworkErrorKind.ShuttingDown), result);
    }

    private static async Task<string> DispatchMeshRequestAsync(
        ZLinkMeshChannelMembership? membership,
        ZLinkRouteHandlerRegistration? routeHandler,
        string? channelName,
        ZLinkDispatchErrorSurface expectedSurface,
        string packetName = "ExactRequest",
        bool expectHandlerError = false,
        bool sealAdmission = false,
        string? expectedRouteMeshName = null,
        RoutingId? expectedChannelSourceNodeRid = null)
    {
        var loggerFactory = new MeshFlowLoggerFactory();
        var routeContextCapture = new MeshRouteContextCapture();
        await using var services = new ServiceCollection()
            .AddSingleton<ILoggerFactory>(loggerFactory)
            .AddSingleton(routeContextCapture)
            .AddTransient<MeshChannelRequestHandler>()
            .AddTransient<MeshAutoRequestHandler>()
            .AddTransient<FailingMeshChannelRequestHandler>()
            .AddTransient<MeshRouteRequestHandler>()
            .BuildServiceProvider();
        var registration = new ZLinkFrameworkRegistration
        {
            ImplicitHandlerAutoRegistrationEnabled = false
        };
        registration.HandlerAssemblies.Add(typeof(MeshAutoRequestHandler).Assembly);
        registration.FreezeScannedHandlerCatalog();
        registration.DispatchOptions.Diagnostics.SetLevel(ZLinkDiagnosticsLevel.Normal);
        var spotNode = new ZLinkSpotNodeRegistration
        {
            SpotNodeName = "mesh",
            RoutingId = RoutingId.From("mesh-node")
        };
        if (membership is not null)
            spotNode.ChannelMemberships.Add(membership);
        if (routeHandler is not null)
            spotNode.RouteRequestHandlers.Add(routeHandler);

        var handlerRegistry = new ZLinkHandlerRegistry([]);
        var runtime = new ZLinkFrameworkRuntime(
            services,
            null!,
            registration,
            handlerRegistry,
            new ZLinkHandlerDispatcher(
                services.GetRequiredService<IServiceScopeFactory>(),
                registration));
        var taskRunner = new ZLinkRuntimeTaskRunner(
            new ThrowingRuntimeErrorSink(),
            CancellationToken.None);
        using var completionAdmission = new ZLinkCompletionAdmissionOwner(
            16,
            16,
            1024 * 1024);
        var dispatcher = Assert.IsType<ZLinkMeshNodeRouteDispatcher>(
            ZLinkMeshNodeRouteDispatcher.Create(
                services,
                registration,
                spotNode,
                runtime,
                taskRunner,
                completionAdmission));
        if (sealAdmission)
            runtime.SealApplicationAdmissionsForDrain();

        var header = new ZLinkEnvelopeHeader(
            ZLinkMessageKind.Request,
            channelName ?? "mesh",
            packetName,
            ZLinkEnvelopeCodec.DefaultContentType,
            MeshCorrelationId,
            null,
            null,
            null,
            null)
        {
            FlowId = MeshFlowId,
            FlowOrigin = ZLinkFlowOrigin.Application
        };
        var request = packetName == nameof(MeshAutoRequest)
            ? (object)new MeshAutoRequest("auto")
            : new MeshRequest(channelName is null ? "route" : "channel");
        var parts = ZLinkEnvelopeCodec.EncodeParts(
            header,
            request,
            request.GetType(),
            null);
        var reply = new TaskCompletionSource<string>(
            TaskCreationOptions.RunContinuationsAsynchronously);
        dispatcher.Dispatch(new ZLinkBackendRouteReceived(
            parts,
            sourceNodeRid: RoutingId.From("source-node"),
            spotId: null,
            requestSeq: 41,
            reply: (replyParts, _) =>
            {
                var replyHeader = ZLinkEnvelopeCodec.DecodeHeader(replyParts);
                if (replyHeader.Kind == ZLinkMessageKind.Error)
                    reply.TrySetResult(
                        sealAdmission
                            ? replyHeader.ErrorCode ?? nameof(ZLinkMessageKind.Error)
                            : nameof(ZLinkMessageKind.Error));
                else
                {
                    var decoded = ZLinkEnvelopeCodec.DecodeBody(replyParts, typeof(MeshReply));
                    reply.TrySetResult(Assert.IsType<MeshReply>(decoded).Value);
                }
                return SubmitResult.Ok;
            },
            channelName));

        var result = await reply.Task.WaitAsync(TimeSpan.FromSeconds(5));
        await taskRunner.StopAsync();

        Assert.Equal(0, completionAdmission.Snapshot().PendingCompletionSends);
        Assert.Equal(0UL, completionAdmission.Snapshot().ResponderReserveBytes);

        var matching = loggerFactory.Messages
            .Where(line => line.Contains($"surface={expectedSurface}", StringComparison.Ordinal))
            .ToArray();
        if (sealAdmission)
        {
            Assert.Empty(matching);
            return result;
        }
        Assert.Equal(2, matching.Length);
        Assert.Contains("phase=received", matching[0], StringComparison.Ordinal);
        Assert.Contains(
            expectHandlerError ? "phase=error" : "phase=replied",
            matching[1],
            StringComparison.Ordinal);
        Assert.All(matching, line =>
        {
            Assert.Contains($"flow={MeshFlowId}", line, StringComparison.Ordinal);
            Assert.Contains($"corr={MeshCorrelationId}", line, StringComparison.Ordinal);
        });
        if (expectedRouteMeshName is not null)
            Assert.Equal(expectedRouteMeshName, routeContextCapture.MeshName);
        if (expectedChannelSourceNodeRid is { } expectedSource)
            Assert.Equal(expectedSource, routeContextCapture.SourceNodeRid);
        return result;
    }

    private const string MeshFlowId = "0196f7c2-4cb4-7cc8-89d4-2d6aee6fca2d";
    private const string MeshCorrelationId = "mesh-dispatch-correlation-41";

    private sealed record MeshRequest(string Value);

    private sealed record MeshReply(string Value);

    private sealed record MeshAutoRequest(string Value);

    [ZLinkHandlerGroup("mesh-node-auto-test")]
    private sealed class MeshAutoRequestHandler
        : IZLinkRequestHandler<MeshAutoRequest, MeshReply>
    {
        public ValueTask<MeshReply> HandleAsync(
            MeshAutoRequest request,
            IZLinkMessageContext context,
            CancellationToken cancellationToken)
        {
            _ = request;
            _ = context;
            cancellationToken.ThrowIfCancellationRequested();
            return ValueTask.FromResult(new MeshReply("AUTO"));
        }
    }

    private sealed class MeshChannelRequestHandler(MeshRouteContextCapture capture)
        : IZLinkRequestHandler<MeshRequest, MeshReply>
    {
        public ValueTask<MeshReply> HandleAsync(
            MeshRequest request,
            IZLinkMessageContext context,
            CancellationToken cancellationToken)
        {
            if (context is ZLinkRouteMessageContext route)
                capture.SourceNodeRid = route.SourceNodeRid;
            cancellationToken.ThrowIfCancellationRequested();
            return ValueTask.FromResult(new MeshReply(request.Value.ToUpperInvariant()));
        }
    }

    private sealed class MeshRouteRequestHandler
        : IZLinkRouteRequestHandler<MeshRequest, MeshReply>
    {
        private readonly MeshRouteContextCapture _capture;

        public MeshRouteRequestHandler(MeshRouteContextCapture capture)
        {
            _capture = capture;
        }

        public ValueTask<MeshReply> HandleAsync(
            MeshRequest request,
            ZLinkRouteMessageContext context,
            CancellationToken cancellationToken)
        {
            _capture.MeshName = context.MeshName;
            cancellationToken.ThrowIfCancellationRequested();
            return ValueTask.FromResult(new MeshReply(request.Value.ToUpperInvariant()));
        }
    }

    private sealed class MeshRouteContextCapture
    {
        public string? MeshName { get; set; }

        public RoutingId? SourceNodeRid { get; set; }
    }

    private sealed class FailingMeshChannelRequestHandler
        : IZLinkRequestHandler<MeshRequest, MeshReply>
    {
        public ValueTask<MeshReply> HandleAsync(
            MeshRequest request,
            IZLinkMessageContext context,
            CancellationToken cancellationToken)
        {
            _ = request;
            _ = context;
            cancellationToken.ThrowIfCancellationRequested();
            throw new InvalidOperationException("expected current MeshNode handler failure");
        }
    }

    private sealed class MeshFlowLoggerFactory : ILoggerFactory
    {
        private readonly MeshFlowLogger _logger = new();

        public IReadOnlyCollection<string> Messages => _logger.Messages;

        public void AddProvider(ILoggerProvider provider)
        {
            _ = provider;
        }

        public ILogger CreateLogger(string categoryName)
        {
            _ = categoryName;
            return _logger;
        }

        public void Dispose()
        {
        }
    }

    private sealed class MeshFlowLogger : ILogger
    {
        public ConcurrentQueue<string> Messages { get; } = new();

        public IDisposable? BeginScope<TState>(TState state) where TState : notnull => null;

        public bool IsEnabled(LogLevel logLevel) => true;

        public void Log<TState>(
            LogLevel logLevel,
            EventId eventId,
            TState state,
            Exception? exception,
            Func<TState, Exception?, string> formatter)
        {
            Messages.Enqueue(formatter(state, exception));
        }
    }
}
