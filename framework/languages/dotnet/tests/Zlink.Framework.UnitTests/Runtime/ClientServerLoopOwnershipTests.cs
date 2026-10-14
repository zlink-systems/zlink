using Microsoft.Extensions.DependencyInjection;
using Microsoft.Extensions.Hosting;
using Zlink.Framework.AspNetCore;
using Zlink.Framework.LocationProvider;
using Zlink.Framework.Runtime.Locations;

namespace Zlink.Framework.UnitTests;

// A blocking ClientServer loop owns a dedicated thread and the socket it receives on
// (#1769, #1745). The default ThreadPool settings stay unchanged here.
public sealed class ClientServerLoopOwnershipTests
{
    private static readonly string[] Channels =
    [
        "matching",
        "money",
        "results",
        "fairness",
        "table-cleanup",
        "consent",
        "profile",
    ];

    // Every host serves each channel and connects to every host's server, so each process
    // holds hosts x channels x hosts client connections. A connection that keeps a ThreadPool
    // worker blocked in its native wait starves startup and the first requests.
    [Fact]
    public async Task Five_hosts_with_client_server_channels_start_and_answer_in_one_process()
    {
        const int hostCount = 5;
        var store = new ZLinkInMemoryProviderLocationStore();
        var hosts = Enumerable.Range(0, hostCount).Select(index => CreateHost(store)).ToArray();
        try
        {
            await Task.WhenAll(hosts.Select(host => host.StartAsync()))
                .WaitAsync(TimeSpan.FromSeconds(60));
            await Task.WhenAll(
                    hosts.SelectMany(
                        (host, index) =>
                            Channels.Select(async channel =>
                            {
                                var reply = await host
                                    .Services.GetRequiredService<IZLinkRouteClient>()
                                    .RequestToChannel(channel, new Ping(index, channel))
                                    .Timeout(TimeSpan.FromSeconds(10))
                                    .Async<Pong>();
                                Assert.Equal(new Pong(index, channel), reply);
                            })
                    )
                )
                .WaitAsync(TimeSpan.FromSeconds(60));
        }
        finally
        {
            await Task.WhenAll(hosts.Select(host => host.StopAsync()))
                .WaitAsync(TimeSpan.FromSeconds(60));
            foreach (var host in hosts)
                host.Dispose();
        }
    }

    // ClientServer channel section 6, steps 3 and 4: a server that stops still runs the
    // request handler it has accepted and delivers that request's reply before it closes the
    // listener. A listener that closes while the handler runs is the EBUSY window of #1745.
    [Fact]
    public async Task Stopping_server_delivers_the_reply_of_its_accepted_request()
    {
        var gate = new HandlerGate();
        var serverServices = new ServiceCollection();
        serverServices.AddSingleton(gate);
        serverServices.AddZLinkFramework(options =>
        {
            options.DisableImplicitHandlerAutoRegistration();
            options
                .AddClientServerChannel("work")
                .Server()
                .Listen(0)
                .AddRequestHandler<GatedHandler, GatedRequest, GatedReply>();
        });
        await using var server = serverServices.BuildServiceProvider();
        var serverRuntime = server.GetRequiredService<ZLinkFrameworkRuntime>();
        await serverRuntime.StartAsync(CancellationToken.None);
        var endpoint = serverRuntime
            .GetListenerStatus(ZLinkListenerKind.ClientServer, "work")
            .Endpoint;
        Assert.NotNull(endpoint);

        var clientServices = new ServiceCollection();
        clientServices.AddZLinkFramework(options =>
            options.AddClientServerChannel("work").Client().Connect(endpoint!)
        );
        await using var client = clientServices.BuildServiceProvider();
        var clientRuntime = client.GetRequiredService<ZLinkFrameworkRuntime>();
        await clientRuntime.StartAsync(CancellationToken.None);
        Task? stop = null;
        try
        {
            var request = client
                .GetRequiredService<IZLinkRouteClient>()
                .RequestToChannel("work", new GatedRequest("held"))
                .Timeout(TimeSpan.FromSeconds(30))
                .Async<GatedReply>()
                .AsTask();
            await gate.Entered.Task.WaitAsync(TimeSpan.FromSeconds(10));

            stop = serverRuntime.StopAsync(CancellationToken.None).AsTask();
            gate.Release();

            Assert.Equal(new GatedReply("held"), await request.WaitAsync(TimeSpan.FromSeconds(30)));
            await stop.WaitAsync(TimeSpan.FromSeconds(30));
        }
        finally
        {
            gate.Release();
            await clientRuntime.StopAsync(CancellationToken.None);
            if (stop is null)
                await serverRuntime.StopAsync(CancellationToken.None);
        }
    }

    private static IHost CreateHost(ZLinkInMemoryProviderLocationStore store)
    {
        var builder = Host.CreateApplicationBuilder();
        builder.Services.AddZLinkFramework(options =>
        {
            options.DisableImplicitHandlerAutoRegistration();
            options.AddLocationStore(store);
            options.AddRouteMesh("loop-ownership").Listen("tcp://127.0.0.1:*");
            foreach (var channel in Channels)
            {
                options
                    .AddClientServerChannel(channel)
                    .Server()
                    .Listen(0)
                    .SetBindHost("127.0.0.1")
                    .SetAdvertiseHost("127.0.0.1")
                    .AddRequestHandler<PingHandler, Ping, Pong>();
                options.AddClientServerChannel(channel).Client();
            }
        });
        return builder.Build();
    }

    private sealed record Ping(int Host, string Channel);

    private sealed record Pong(int Host, string Channel);

    private sealed class PingHandler : IZLinkRequestHandler<Ping, Pong>
    {
        public ValueTask<Pong> HandleAsync(
            Ping request,
            IZLinkMessageContext context,
            CancellationToken cancellationToken
        ) => ValueTask.FromResult(new Pong(request.Host, request.Channel));
    }

    private sealed record GatedRequest(string Value);

    private sealed record GatedReply(string Value);

    private sealed class HandlerGate
    {
        private readonly TaskCompletionSource _released = new(
            TaskCreationOptions.RunContinuationsAsynchronously
        );

        public TaskCompletionSource Entered { get; } =
            new(TaskCreationOptions.RunContinuationsAsynchronously);

        public void Release() => _released.TrySetResult();

        // The handler ignores cancellation so that it stays accepted until the test
        // releases it.
        public async ValueTask HoldAsync()
        {
            Entered.TrySetResult();
            await _released.Task.ConfigureAwait(false);
        }
    }

    private sealed class GatedHandler(HandlerGate gate)
        : IZLinkRequestHandler<GatedRequest, GatedReply>
    {
        public async ValueTask<GatedReply> HandleAsync(
            GatedRequest request,
            IZLinkMessageContext context,
            CancellationToken cancellationToken
        )
        {
            await gate.HoldAsync().ConfigureAwait(false);
            return new GatedReply(request.Value);
        }
    }
}
