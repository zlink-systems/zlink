using Microsoft.Extensions.DependencyInjection;
using Microsoft.Extensions.Hosting;
using Zlink.Framework.AspNetCore;
using Zlink.Framework.LocationProvider;
using Zlink.Framework.Runtime.Backend.Contracts;
using Zlink.Framework.Runtime.Backend.DotNet;
using Zlink.Framework.Runtime.Host;
using Zlink.Framework.Runtime.Locations;

namespace Zlink.Framework.UnitTests.Runtime;

public sealed class PrestartCallTests : RegistrationValidationSupport
{
    [Theory]
    [InlineData("spot-send")]
    [InlineData("spot-request")]
    [InlineData("instance-send")]
    [InlineData("instance-request")]
    [InlineData("actor-send")]
    [InlineData("actor-request")]
    [InlineData("channel-send")]
    [InlineData("channel-request")]
    [InlineData("node-send")]
    [InlineData("node-request")]
    [InlineData("fanout-publish")]
    [InlineData("spot-publish")]
    [InlineData("spot-find")]
    [InlineData("actor-find")]
    [InlineData("actor-find-spot")]
    [InlineData("spot-create")]
    [InlineData("actor-create")]
    public async Task Public_terminal_before_start_is_invalid_without_store_or_transport(
        string kind
    )
    {
        var store = new CountingStore();
        var backend = new CountingBackend();
        var registrations = new ServiceCollection();
        registrations.AddZLinkFramework(options =>
        {
            options.AddLocationStore(store);
            options.AddRouteMesh("mesh").Listen("tcp://127.0.0.1:0").Objects().Client();
        });
        registrations.AddSingleton<IZLinkBackendAdapterFactory>(backend);
        await using var services = registrations.BuildServiceProvider();
        var runtime = services.GetRequiredService<ZLinkFrameworkRuntime>();
        var route = services.GetRequiredService<IZLinkRouteClient>();
        var spot = services.GetRequiredService<IZLinkSpotClient>();
        var actor = services.GetRequiredService<IZLinkActorClient>();
        var spots = services.GetRequiredService<IZLinkSpotManager>();
        var actors = services.GetRequiredService<IZLinkActorManager>();
        var message = new Probe("before-start");
        var storeBaseline = store.Calls;
        var transportBaseline = backend.Contexts;
        var error = await Assert.ThrowsAsync<ZLinkFrameworkException>(async () =>
        {
            switch (kind)
            {
                case "spot-send":
                    await spot.SendToSpot("spot", message).Async();
                    break;
                case "spot-request":
                    await spot.RequestToSpot("spot", message).Async<Probe>();
                    break;
                case "instance-send":
                    await spot.SendToSpot("spot", message).InstanceSpot().Async();
                    break;
                case "instance-request":
                    await spot.RequestToSpot("spot", message).InstanceSpot().Async<Probe>();
                    break;
                case "actor-send":
                    await actor.SendToActor("actor", message).Async();
                    break;
                case "actor-request":
                    await actor.RequestToActor("actor", message).Async<Probe>();
                    break;
                case "channel-send":
                    await route.SendToChannel("work", message).Async();
                    break;
                case "channel-request":
                    await route.RequestToChannel("work", message).Async<Probe>();
                    break;
                case "node-send":
                    await route.SendToNode("mesh", RoutingId.From("node"), message).Async();
                    break;
                case "node-request":
                    await route
                        .RequestToNode("mesh", RoutingId.From("node"), message)
                        .Async<Probe>();
                    break;
                case "fanout-publish":
                    await services
                        .GetRequiredService<IZLinkFanoutClient>()
                        .Publish("events", message)
                        .Async();
                    break;
                case "spot-publish":
                    await services
                        .GetRequiredService<IZLinkSpotPublisherClient>()
                        .Publish("events", "topic", message)
                        .Async();
                    break;
                case "spot-find":
                    await spots.FindAsync("spot");
                    break;
                case "actor-find":
                    await actors.FindAsync("actor");
                    break;
                case "actor-find-spot":
                    await actors.FindSpotAsync("actor");
                    break;
                case "spot-create":
                    await spots.Create("spot-type").Async();
                    break;
                case "actor-create":
                    await actors.Create("actor", "actor-type").Async();
                    break;
            }
        });
        Assert.Equal(ZLinkFrameworkErrorKind.InvalidOperation, error.Kind);
        Assert.Equal(storeBaseline, store.Calls);
        Assert.Equal(transportBaseline, backend.Contexts);
        Assert.False(runtime.IsStarted);
    }

    [Fact]
    public async Task Channel_call_succeeds_during_hosted_service_start_and_after_start()
    {
        var builder = Host.CreateApplicationBuilder();
        builder.Services.AddZLinkFramework(options =>
        {
            options.AddClientServerChannel("work").Client();
            options
                .AddClientServerChannel("work")
                .Server()
                .Listen(0)
                .AddRequestHandler<
                    TestChannelRequestHandler,
                    TestChannelRequest,
                    TestChannelReply
                >();
            options.AddFanoutChannel("events").EnablePublisher();
        });
        builder.Services.AddHostedService<StartupCaller>();
        using var host = builder.Build();
        var routes = host.Services.GetRequiredService<IZLinkRouteClient>();
        var call = routes.RequestToChannel("work", new TestChannelRequest("same-call"));
        var beforeStart = call.Async<TestChannelReply>();
        var sendCall = routes.SendToChannel("work", new TestChannelRequest("same-send-call"));
        var beforeStartSend = sendCall.Async();
        var error = await Assert.ThrowsAsync<ZLinkFrameworkException>(() => beforeStart.AsTask());
        Assert.Equal(ZLinkFrameworkErrorKind.InvalidOperation, error.Kind);
        var sendError = await Assert.ThrowsAsync<ZLinkFrameworkException>(async () =>
            await beforeStartSend
        );
        Assert.Equal(ZLinkFrameworkErrorKind.InvalidOperation, sendError.Kind);
        await host.StartAsync();
        try
        {
            Assert.Equal("same-call", (await call.Async<TestChannelReply>()).Value);
            await sendCall.Async();
            var repeatedSend = await Assert.ThrowsAsync<ZLinkFrameworkException>(async () =>
                await sendCall.Async()
            );
            Assert.Equal(ZLinkFrameworkErrorKind.InvalidOperation, repeatedSend.Kind);
            Assert.Contains("already submitted", repeatedSend.Message, StringComparison.Ordinal);
            await host
                .Services.GetRequiredService<IZLinkFanoutClient>()
                .Publish("events", new TestPublishedEvent("started"))
                .Async();
            var admissionError = await Assert.ThrowsAsync<ZLinkFrameworkException>(async () =>
                await beforeStart.Admitted
            );
            Assert.Equal(ZLinkFrameworkErrorKind.InvalidOperation, admissionError.Kind);
            var replyError = await Assert.ThrowsAsync<ZLinkFrameworkException>(() =>
                beforeStart.AsTask()
            );
            Assert.Equal(ZLinkFrameworkErrorKind.InvalidOperation, replyError.Kind);
        }
        finally
        {
            await host.StopAsync();
        }
    }

    [Fact]
    public async Task Call_admitted_while_runtime_is_starting_keeps_its_rejection_after_start()
    {
        var backend = new CountingBackend();
        var registrations = new ServiceCollection();
        registrations.AddZLinkFramework(_ => { });
        registrations.AddSingleton<IZLinkBackendAdapterFactory>(backend);
        await using var services = registrations.BuildServiceProvider();
        var runtime = services.GetRequiredService<ZLinkFrameworkRuntime>();
        var routes = services.GetRequiredService<IZLinkRouteClient>();
        Task? rejectedCall = null;
        backend.CreatingContext = () =>
        {
            Assert.False(runtime.IsStarted);
            rejectedCall = routes
                .RequestToChannel("work", new Probe("starting"))
                .Async<Probe>()
                .AsTask();
        };
        await runtime.StartAsync(CancellationToken.None);
        try
        {
            Assert.True(runtime.IsStarted);
            Assert.NotNull(rejectedCall);
            var error = await Assert.ThrowsAsync<ZLinkFrameworkException>(() => rejectedCall!);
            Assert.Equal(ZLinkFrameworkErrorKind.InvalidOperation, error.Kind);
        }
        finally
        {
            await runtime.StopAsync(CancellationToken.None);
        }
    }

    [Theory]
    [InlineData("spot-send")]
    [InlineData("channel-request")]
    [InlineData("spot-find")]
    [InlineData("actor-create")]
    public async Task Public_terminal_after_stop_is_shutting_down(string kind)
    {
        var registrations = new ServiceCollection();
        registrations.AddZLinkFramework(options =>
        {
            options.AddLocationStore(new CountingStore());
            options.AddRouteMesh("mesh").Listen("tcp://127.0.0.1:0").Objects().Client();
        });
        await using var services = registrations.BuildServiceProvider();
        var runtime = services.GetRequiredService<ZLinkFrameworkRuntime>();
        await runtime.StartAsync(CancellationToken.None);
        await runtime.StopAsync(CancellationToken.None);
        var message = new Probe("after-stop");
        var error = await Assert.ThrowsAsync<ZLinkFrameworkException>(async () =>
        {
            switch (kind)
            {
                case "spot-send":
                    await services
                        .GetRequiredService<IZLinkSpotClient>()
                        .SendToSpot("spot", message)
                        .Async();
                    break;
                case "channel-request":
                    await services
                        .GetRequiredService<IZLinkRouteClient>()
                        .RequestToChannel("work", message)
                        .Async<Probe>();
                    break;
                case "spot-find":
                    await services.GetRequiredService<IZLinkSpotManager>().FindAsync("spot");
                    break;
                case "actor-create":
                    await services
                        .GetRequiredService<IZLinkActorManager>()
                        .Create("actor", "actor-type")
                        .Async();
                    break;
            }
        });
        Assert.Equal(ZLinkFrameworkErrorKind.ShuttingDown, error.Kind);
    }

    [Theory]
    [InlineData("spot-send")]
    [InlineData("instance-send")]
    [InlineData("actor-send")]
    [InlineData("channel-send")]
    [InlineData("node-send")]
    [InlineData("fanout-publish")]
    [InlineData("spot-publish")]
    public async Task Rejected_before_start_send_or_publish_can_be_submitted_again_after_start(
        string kind
    )
    {
        var registrations = new ServiceCollection();
        registrations.AddZLinkFramework(options =>
        {
            options.AddLocationStore(new CountingStore());
            options.AddRouteMesh("mesh").Listen("tcp://127.0.0.1:0").Objects().Client();
        });
        await using var services = registrations.BuildServiceProvider();
        var runtime = services.GetRequiredService<ZLinkFrameworkRuntime>();
        var message = new Probe("same-call");
        Func<ValueTask> submit;
        switch (kind)
        {
            case "spot-send":
                var spotCall = services
                    .GetRequiredService<IZLinkSpotClient>()
                    .SendToSpot("spot", message);
                submit = () => spotCall.Async();
                break;
            case "instance-send":
                var instanceCall = services
                    .GetRequiredService<IZLinkSpotClient>()
                    .SendToSpot("spot", message)
                    .InstanceSpot();
                submit = () => instanceCall.Async();
                break;
            case "actor-send":
                var actorCall = services
                    .GetRequiredService<IZLinkActorClient>()
                    .SendToActor("actor", message);
                submit = () => actorCall.Async();
                break;
            case "channel-send":
                var channelCall = services
                    .GetRequiredService<IZLinkRouteClient>()
                    .SendToChannel("work", message);
                submit = () => channelCall.Async();
                break;
            case "node-send":
                var nodeCall = services
                    .GetRequiredService<IZLinkRouteClient>()
                    .SendToNode("mesh", RoutingId.From("node"), message);
                submit = () => nodeCall.Async();
                break;
            case "fanout-publish":
                var fanoutCall = services
                    .GetRequiredService<IZLinkFanoutClient>()
                    .Publish("events", message);
                submit = () => fanoutCall.Async();
                break;
            case "spot-publish":
                var publishCall = services
                    .GetRequiredService<IZLinkSpotPublisherClient>()
                    .Publish("events", "topic", message);
                submit = () => publishCall.Async();
                break;
            default:
                throw new ArgumentOutOfRangeException(nameof(kind));
        }

        var beforeStart = await Assert.ThrowsAsync<ZLinkFrameworkException>(async () =>
            await submit()
        );
        Assert.Equal(ZLinkFrameworkErrorKind.InvalidOperation, beforeStart.Kind);
        await runtime.StartAsync(CancellationToken.None);
        try
        {
            var afterStart = await Record.ExceptionAsync(async () => await submit());
            Assert.False(
                afterStart is ZLinkFrameworkException exception
                    && exception.Kind == ZLinkFrameworkErrorKind.InvalidOperation
                    && exception.Message.Contains("already submitted", StringComparison.Ordinal),
                afterStart?.ToString()
            );
        }
        finally
        {
            await runtime.StopAsync(CancellationToken.None);
        }
    }

    [Fact]
    public async Task Accepted_fanout_call_is_single_use()
    {
        var registrations = new ServiceCollection();
        registrations.AddZLinkFramework(options =>
            options.AddFanoutChannel("events").EnablePublisher()
        );
        await using var services = registrations.BuildServiceProvider();
        var runtime = services.GetRequiredService<ZLinkFrameworkRuntime>();
        var call = services
            .GetRequiredService<IZLinkFanoutClient>()
            .Publish("events", new Probe("accepted"));
        var beforeStart = await Assert.ThrowsAsync<ZLinkFrameworkException>(async () =>
            await call.Async()
        );
        Assert.Equal(ZLinkFrameworkErrorKind.InvalidOperation, beforeStart.Kind);

        await runtime.StartAsync(CancellationToken.None);
        try
        {
            await call.Async();
            var repeated = await Assert.ThrowsAsync<ZLinkFrameworkException>(async () =>
                await call.Async()
            );
            Assert.Equal(ZLinkFrameworkErrorKind.InvalidOperation, repeated.Kind);
            Assert.Contains("already submitted", repeated.Message, StringComparison.Ordinal);
        }
        finally
        {
            await runtime.StopAsync(CancellationToken.None);
        }
    }

    [Fact]
    public async Task Send_rejected_after_stop_remains_shutting_down_when_resubmitted()
    {
        var registrations = new ServiceCollection();
        registrations.AddZLinkFramework(_ => { });
        await using var services = registrations.BuildServiceProvider();
        var runtime = services.GetRequiredService<ZLinkFrameworkRuntime>();
        var call = services
            .GetRequiredService<IZLinkRouteClient>()
            .SendToChannel("work", new Probe("after-stop"));
        await runtime.StartAsync(CancellationToken.None);
        await runtime.StopAsync(CancellationToken.None);

        for (var attempt = 0; attempt < 2; attempt++)
        {
            var error = await Assert.ThrowsAsync<ZLinkFrameworkException>(async () =>
                await call.Async()
            );
            Assert.Equal(ZLinkFrameworkErrorKind.ShuttingDown, error.Kind);
        }
    }

    private sealed class StartupCaller(IZLinkRouteClient routes, ZLinkFrameworkRuntime runtime)
        : IHostedService
    {
        public async Task StartAsync(CancellationToken cancellationToken)
        {
            Assert.True(runtime.IsStarted);
            var reply = await routes
                .RequestToChannel("work", new TestChannelRequest("hosted-start"))
                .Async<TestChannelReply>(cancellationToken);
            Assert.Equal("hosted-start", reply.Value);
        }

        public Task StopAsync(CancellationToken cancellationToken) => Task.CompletedTask;
    }

    private sealed record Probe(string Value);

    private sealed class CountingBackend : IZLinkBackendAdapterFactory
    {
        private readonly ZLinkDotNetBackendAdapterFactory _inner = new();
        public int Contexts { get; private set; }
        public Action? CreatingContext { get; set; }

        public IZLinkBackendRuntimeContext CreateRuntimeContext()
        {
            Contexts++;
            CreatingContext?.Invoke();
            return _inner.CreateRuntimeContext();
        }

        public IZLinkMonitoringBackendAdapter CreateMonitoringAdapter() =>
            _inner.CreateMonitoringAdapter();
    }

    private sealed class CountingStore : IZLinkLocationStore
    {
        private readonly ZLinkInMemoryProviderLocationStore _inner = new();
        public int Calls { get; private set; }

        public ValueTask<ZLinkStoreReadResult> ReadAsync(
            ZLinkStoreKey key,
            CancellationToken cancellationToken = default
        )
        {
            Calls++;
            return _inner.ReadAsync(key, cancellationToken);
        }

        public ValueTask<ZLinkStoreWriteResult> WriteAsync(
            ZLinkStoreWriteRequest request,
            CancellationToken cancellationToken = default
        )
        {
            Calls++;
            return _inner.WriteAsync(request, cancellationToken);
        }

        public ValueTask<ZLinkStoreScanResult> ScanAsync(
            ZLinkStoreScanRequest request,
            CancellationToken cancellationToken = default
        )
        {
            Calls++;
            return _inner.ScanAsync(request, cancellationToken);
        }
    }
}
