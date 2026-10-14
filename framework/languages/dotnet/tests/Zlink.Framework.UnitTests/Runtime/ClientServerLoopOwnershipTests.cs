using System.Reflection;
using Microsoft.Extensions.DependencyInjection;
using Microsoft.Extensions.Hosting;
using Zlink.Framework.AspNetCore;
using Zlink.Framework.LocationProvider;
using Zlink.Framework.Runtime.Identifiers;
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

    // ClientServer channel section 6, step 4: the listener closes after the accepted handlers
    // have finished. A close that runs while the server receive loop can still use the
    // router is the EBUSY window of #1745.
    [Fact]
    public async Task Server_listener_closes_only_after_its_accepted_handler_finishes()
    {
        var gate = new HandlerGate();
        var services = new ServiceCollection();
        services.AddSingleton(gate);
        services.AddZLinkFramework(options =>
        {
            options.DisableImplicitHandlerAutoRegistration();
            options.AddClientServerChannel("work").Client();
            options
                .AddClientServerChannel("work")
                .Server()
                .Listen(0)
                .AddRequestHandler<GatedHandler, GatedRequest, GatedReply>();
        });
        await using var provider = services.BuildServiceProvider();
        var runtime = provider.GetRequiredService<ZLinkFrameworkRuntime>();
        await runtime.StartAsync(CancellationToken.None);
        var state = await runtime.EnsureStartedStateAsync(CancellationToken.None);
        var bundle = await state.RunStateAsync(() =>
            state.ClientServerServerBundles[ZLinkChannelName.FromBoundary("work", "channelName")]
        );
        var recorder = DispatchProxy.Create<IAsyncDisposable, CloseRecorder>();
        var close = (CloseRecorder)(object)recorder;
        close.Inner = bundle.Socket;
        close.Gate = gate;
        typeof(ZLinkChannelRuntimeBundle)
            .GetField("<Socket>k__BackingField", BindingFlags.Instance | BindingFlags.NonPublic)!
            .SetValue(bundle, recorder);

        Task stop = Task.CompletedTask;
        try
        {
            var transport = runtime.GetClientServerClientRuntime("work");
            var ready = DateTime.UtcNow + TimeSpan.FromSeconds(10);
            while (transport.ReadyCount != 1 && DateTime.UtcNow < ready)
                await Task.Delay(10);
            Assert.Equal(1, transport.ReadyCount);

            var request = provider
                .GetRequiredService<IZLinkRouteClient>()
                .RequestToChannel("work", new GatedRequest("held"))
                .Timeout(TimeSpan.FromSeconds(30))
                .Async<GatedReply>()
                .AsTask();
            await gate.Entered.Task.WaitAsync(TimeSpan.FromSeconds(10));

            stop = runtime.StopAsync(CancellationToken.None).AsTask();
            // A close that does not wait for the handler happens while it is held here.
            await Task.WhenAny(close.Closed.Task, Task.Delay(TimeSpan.FromSeconds(1)));
            gate.Release();
            await stop.WaitAsync(TimeSpan.FromSeconds(30));
            await request.ContinueWith(static _ => { }, TaskScheduler.Default);

            Assert.True(close.Closed.Task.IsCompleted);
            Assert.True(await close.Closed.Task);
        }
        finally
        {
            gate.Release();
            await stop.ContinueWith(static _ => { }, TaskScheduler.Default);
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

    internal sealed class HandlerGate
    {
        private readonly TaskCompletionSource _released = new(
            TaskCreationOptions.RunContinuationsAsynchronously
        );
        private int _finished;

        public TaskCompletionSource Entered { get; } =
            new(TaskCreationOptions.RunContinuationsAsynchronously);

        public bool Finished => Volatile.Read(ref _finished) != 0;

        public void Release() => _released.TrySetResult();

        // The handler ignores cancellation so that it stays accepted until the test
        // releases it.
        public async ValueTask HoldAsync()
        {
            Entered.TrySetResult();
            await _released.Task.ConfigureAwait(false);
            Volatile.Write(ref _finished, 1);
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

    // Records whether the accepted handler had finished when the router was closed, then
    // closes the router.
    public class CloseRecorder : DispatchProxy
    {
        internal IAsyncDisposable Inner { get; set; } = null!;

        internal HandlerGate Gate { get; set; } = null!;

        internal TaskCompletionSource<bool> Closed { get; } =
            new(TaskCreationOptions.RunContinuationsAsynchronously);

        protected override object? Invoke(MethodInfo? targetMethod, object?[]? args)
        {
            if (targetMethod?.Name == nameof(IAsyncDisposable.DisposeAsync))
                Closed.TrySetResult(Gate.Finished);
            return targetMethod!.Invoke(Inner, args);
        }
    }
}
