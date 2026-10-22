using System.Collections.Concurrent;
using System.Diagnostics;
using Microsoft.Extensions.DependencyInjection;
using Microsoft.Extensions.Hosting;
using Zlink.Framework.AspNetCore;
using Zlink.Framework.Runtime.Dispatch;
using Zlink.Framework.Runtime.Locations;

namespace Zlink.Framework.UnitTests;

public sealed class ChannelReceiveMessageFlowTests(Xunit.Abstractions.ITestOutputHelper output)
{
    [Theory]
    [InlineData(false, false)]
    [InlineData(false, true)]
    [InlineData(true, false)]
    [InlineData(true, true)]
    public async Task ChannelRecordsReceiveAdmissionDispatchAndTerminal(
        bool routeMesh,
        bool request
    )
    {
        var records = new ConcurrentQueue<Activity>();
        using var listener = new ActivityListener
        {
            ShouldListenTo = source => source.Name == ZLinkTelemetry.ActivitySourceName,
            Sample = (ref ActivityCreationOptions<ActivityContext> _) =>
                ActivitySamplingResult.AllDataAndRecorded,
            ActivityStopped = records.Enqueue,
        };
        ActivitySource.AddActivityListener(listener);
        var services = new ServiceCollection();
        services.AddZLinkFramework(options =>
        {
            options.DisableImplicitHandlerAutoRegistration();
            if (routeMesh)
            {
                options.UseTestLocationStore();
                var mesh = options
                    .AddRouteMesh("flow-mesh")
                    .Listen("tcp://127.0.0.1:0")
                    .SetRoutingId(RoutingId.From("flow-server"));
                mesh.Objects().Server();
                var channel = mesh.Channel("flow-channel");
                channel
                    .Server()
                    .AddRequestHandler<RequestHandler, FlowRequest, FlowReply>()
                    .AddSendHandler<SendHandler, FlowSend>();
            }
            else
            {
                var channel = options.AddClientServerChannel("flow-channel");
                channel
                    .Server()
                    .Listen(0)
                    .AddRequestHandler<RequestHandler, FlowRequest, FlowReply>()
                    .AddSendHandler<SendHandler, FlowSend>();
                channel.Client();
            }
        });
        services.AddSingleton<Probe>();
        await using var provider = services.BuildServiceProvider();
        provider
            .GetRequiredService<ZLinkFrameworkRegistration>()
            .DispatchOptions.Diagnostics.SetLevel(ZLinkDiagnosticsLevel.Normal);
        var runtime = provider.GetRequiredService<ZLinkFrameworkRuntime>();
        await provider
            .GetServices<IHostedService>()
            .Single(service => service is ZLinkFrameworkHostedService)
            .StartAsync(CancellationToken.None);
        var probe = provider.GetRequiredService<Probe>();
        var sourceServices = new ServiceCollection();
        sourceServices.AddZLinkFramework(options =>
        {
            options.DisableImplicitHandlerAutoRegistration();
            if (routeMesh)
            {
                options.UseTestLocationStore();
                var target = runtime.GetMeshNodeRuntime("flow-mesh").Node;
                var mesh = options
                    .AddRouteMesh("flow-mesh")
                    .Listen("tcp://127.0.0.1:0")
                    .SetRoutingId(RoutingId.From("flow-source"));
                mesh.Objects().Client();
                mesh.PeerConnections.Connect(target.RoutingId, target.MeshStatus().LocalEndpoint!);
                mesh.Channel("flow-channel").Client();
            }
            else
                options
                    .AddClientServerChannel("flow-channel")
                    .Client()
                    .Connect(
                        runtime
                            .GetListenerStatus(ZLinkListenerKind.ClientServer, "flow-channel")
                            .Endpoint
                    );
        });
        await using var source = sourceServices.BuildServiceProvider();
        source
            .GetRequiredService<ZLinkFrameworkRegistration>()
            .DispatchOptions.Diagnostics.SetLevel(ZLinkDiagnosticsLevel.Normal);
        var sourceRuntime = source.GetRequiredService<ZLinkFrameworkRuntime>();
        await source
            .GetServices<IHostedService>()
            .Single(service => service is ZLinkFrameworkHostedService)
            .StartAsync(CancellationToken.None);
        if (routeMesh)
        {
            using var readyTimeout = new CancellationTokenSource(TimeSpan.FromSeconds(5));
            await foreach (
                var status in source
                    .GetRequiredService<IZLinkRouteMeshRuntime>()
                    .ObserveAsync("flow-mesh", readyTimeout.Token)
            )
                if (
                    status.Status.Channels.Any(channel =>
                        channel.ChannelName == "flow-channel" && channel.IsReady
                    )
                )
                    break;
        }
        var client = source.GetRequiredService<IZLinkRouteClient>();
        try
        {
            Task operation = request
                ? client
                    .RequestToChannel("flow-channel", new FlowRequest())
                    .Timeout(TimeSpan.FromSeconds(5))
                    .Async<FlowReply>()
                    .AsTask()
                : client.SendToChannel("flow-channel", new FlowSend()).Async().AsTask();
            await probe.Started.Task.WaitAsync(TimeSpan.FromSeconds(5));
            var packet = request ? nameof(FlowRequest) : nameof(FlowSend);
            Activity[] Incoming() =>
                records
                    .Where(activity =>
                        (string?)activity.GetTagItem("packet_name") == packet
                        && (string?)activity.GetTagItem("surface") == "channel"
                        && new[]
                        {
                            "received",
                            "admitted",
                            "dispatched",
                            "replied",
                            "completed",
                        }.Contains((string?)activity.GetTagItem("phase"))
                    )
                    .ToArray();
            Assert.Equal(
                new[] { "received", "admitted", "dispatched" },
                Incoming().Select(activity => activity.GetTagItem("phase"))
            );
            probe.Release.TrySetResult();
            await operation;
            await runtime.StopAsync(CancellationToken.None);
            var incoming = Incoming();
            Assert.Equal(
                new[] { "received", "admitted", "dispatched", request ? "replied" : "completed" },
                incoming.Select(activity => activity.GetTagItem("phase"))
            );
            Assert.All(
                incoming,
                activity =>
                {
                    Assert.Equal(incoming[0].GetTagItem("flow_id"), activity.GetTagItem("flow_id"));
                    Assert.False(string.IsNullOrEmpty((string?)activity.GetTagItem("flow_id")));
                    Assert.Equal(
                        incoming[0].GetTagItem("correlation_id"),
                        activity.GetTagItem("correlation_id")
                    );
                }
            );
        }
        finally
        {
            probe.Release.TrySetResult();
            await sourceRuntime.StopAsync(CancellationToken.None);
            await runtime.StopAsync(CancellationToken.None);
        }
    }

    [Theory]
    [InlineData(ZLinkDiagnosticsLevel.Off, false)]
    [InlineData(ZLinkDiagnosticsLevel.Normal, true)]
    public void ChannelStageAllocationGate(ZLinkDiagnosticsLevel level, bool allocates)
    {
        var options = new ZLinkDispatchOptionsModel();
        options.Diagnostics.SetLevel(level);
        var reporter = new ZLinkDispatchErrorReporter(options);
        var scope = new ZLinkDispatchFlowScope(
            ZLinkDispatchErrorSurface.Channel,
            false,
            ZLinkDispatchMessageKind.Request,
            "FlowRequest",
            "flow-channel"
        );
        for (var index = 0; index < 1000; index++)
            TraceStages();
        var before = GC.GetAllocatedBytesForCurrentThread();
        for (var index = 0; index < 10000; index++)
            TraceStages();
        var allocated = GC.GetAllocatedBytesForCurrentThread() - before;
        Assert.Equal(allocates, allocated > 0);
        if (!allocates)
            Assert.Equal(0, allocated);
        void TraceStages()
        {
            scope.Trace(reporter, ZLinkMessageFlowOutcome.Received);
            scope.Trace(reporter, ZLinkMessageFlowOutcome.Admitted);
            scope.Trace(reporter, ZLinkMessageFlowOutcome.Dispatched);
            scope.Trace(reporter, ZLinkMessageFlowOutcome.Completed);
            scope.Trace(reporter, ZLinkMessageFlowOutcome.Replied);
        }
    }

    [Theory]
    [InlineData(false)]
    [InlineData(true)]
    public async Task DisabledChannelDispatchAddsNoAllocationToSuspendedHandlerInvocation(
        bool filtered
    )
    {
        var registration = new ZLinkFrameworkRegistration();
        registration.DispatchOptions.Diagnostics.SetLevel(ZLinkDiagnosticsLevel.Off);
        if (filtered)
            registration.Filters.Add(typeof(AllocationFilter));
        var probe = new AllocationProbe();
        using var services = new ServiceCollection().AddSingleton(probe).BuildServiceProvider();
        var dispatcher = new ZLinkHandlerDispatcher(
            services.GetRequiredService<IServiceScopeFactory>(),
            registration
        );
        var endpoint = ZLinkHandlerScanner.CreateExplicitInterfaceDescriptor(
            typeof(AllocationHandler),
            typeof(IZLinkRequestHandler<FlowRequest, FlowReply>),
            ZLinkMessageKind.Request,
            "flow-channel",
            nameof(FlowRequest)
        );
        var context = new ZLinkMessageContext(
            null,
            "flow-channel",
            nameof(FlowRequest),
            null,
            null,
            null
        );
        var message = new FlowRequest();
        async Task<long> Measure(ZLinkHandlerDispatchKind kind)
        {
            long total = 0;
            for (var index = 0; index < 256; index++)
            {
                probe.Pending = new TaskCompletionSource<FlowReply>(
                    TaskCreationOptions.RunContinuationsAsynchronously
                );
                var before = GC.GetAllocatedBytesForCurrentThread();
                var invocation = dispatcher.DispatchAsync(
                    endpoint,
                    message,
                    context,
                    kind,
                    CancellationToken.None
                );
                var bytes = GC.GetAllocatedBytesForCurrentThread() - before;
                Assert.False(invocation.IsCompleted);
                probe.Pending.SetResult(new FlowReply());
                await invocation;
                if (index >= 128)
                    total += bytes;
            }
            return total;
        }
        var direct = await Measure(ZLinkHandlerDispatchKind.NodeDirectRequest);
        var channel = await Measure(ZLinkHandlerDispatchKind.ChannelRequest);
        output.WriteLine(
            $"Off suspended invocation allocation: filtered={filtered}, direct={direct / 128} B, channel={channel / 128} B."
        );
        Assert.True(direct > 0);
        Assert.Equal(direct, channel);
    }

    private sealed class AllocationFilter : IZLinkHandlerFilter
    {
        public ValueTask InvokeAsync(
            IZLinkHandlerFilterContext context,
            ZLinkHandlerFilterNext next,
            CancellationToken cancellationToken
        ) => next();
    }

    private sealed class AllocationProbe
    {
        public TaskCompletionSource<FlowReply> Pending { get; set; } = null!;
    }

    private sealed class AllocationHandler(AllocationProbe probe)
        : IZLinkRequestHandler<FlowRequest, FlowReply>
    {
        public ValueTask<FlowReply> HandleAsync(
            FlowRequest message,
            IZLinkMessageContext context,
            CancellationToken cancellationToken
        ) => new(probe.Pending.Task);
    }

    private sealed record FlowRequest;

    private sealed record FlowReply;

    private sealed record FlowSend;

    private sealed class Probe
    {
        public TaskCompletionSource Started { get; } =
            new(TaskCreationOptions.RunContinuationsAsynchronously);
        public TaskCompletionSource Release { get; } =
            new(TaskCreationOptions.RunContinuationsAsynchronously);

        public async ValueTask HandleAsync()
        {
            Started.TrySetResult();
            await Release.Task;
        }
    }

    private sealed class RequestHandler(Probe probe) : IZLinkRequestHandler<FlowRequest, FlowReply>
    {
        public async ValueTask<FlowReply> HandleAsync(
            FlowRequest message,
            IZLinkMessageContext context,
            CancellationToken cancellationToken
        )
        {
            await probe.HandleAsync();
            return new FlowReply();
        }
    }

    private sealed class SendHandler(Probe probe) : IZLinkSendHandler<FlowSend>
    {
        public ValueTask HandleAsync(
            FlowSend message,
            IZLinkMessageContext context,
            CancellationToken cancellationToken
        ) => probe.HandleAsync();
    }
}
