using System.Diagnostics;
using Microsoft.Extensions.DependencyInjection;
using Zlink.Framework.AspNetCore;
using Zlink.Framework.LocationProvider;
using Zlink.Framework.Runtime.Backend.Contracts;
using Zlink.Framework.Runtime.Configuration;
using Zlink.Framework.Runtime.Diagnostics;
using Zlink.Framework.Runtime.Host;
using Zlink.Framework.Runtime.Identifiers;
using Zlink.Framework.Runtime.Locations;

namespace Zlink.Framework.UnitTests;

public sealed class SelfClientServerAckFailureRecoveryTests
{
    private const string Channel = "self-ack-recovery";

    [Theory]
    [InlineData(false)]
    [InlineData(true)]
    public Task Ack_failure_preserves_the_registered_connection_and_public_requests(
        bool expireServer
    ) => RunRecoveryAsync(expireServer, expireClient: false, injectAckFailure: true);

    [Theory]
    [InlineData(false)]
    [InlineData(true)]
    public Task Natural_liveness_expiry_recovers_public_requests(bool expireClient) =>
        RunRecoveryAsync(expireServer: true, expireClient, injectAckFailure: false);

    private static async Task RunRecoveryAsync(
        bool expireServer,
        bool expireClient,
        bool injectAckFailure
    )
    {
        var label = $"ack-{injectAckFailure}-server-{expireServer}-client-{expireClient}";
        var directory = Environment.GetEnvironmentVariable("ZLINK_CLIENT_SERVER_RECOVERY_OUTPUT");
        using var flow = directory is null
            ? null
            : new TestHostMessageFlowListener($"{directory}/{label}.flow");
        using var log = directory is null
            ? null
            : new StreamWriter($"{directory}/{label}.log") { AutoFlush = true };
        var time = new ControllableTimeProvider();
        var services = new ServiceCollection();
        services.AddZLinkFramework(options =>
        {
            options.AddLocationStore(new ZLinkInMemoryProviderLocationStore());
            options.AddClientServerChannel(Channel).Client();
            options
                .AddClientServerChannel(Channel)
                .Server()
                .Listen(0)
                .AddRequestHandler<EchoHandler, EchoRequest, EchoReply>();
        });
        await using var provider = services.BuildServiceProvider();
        var registration = provider.GetRequiredService<ZLinkFrameworkRegistration>();
        registration.TimeProvider = time;
        registration.DispatchOptions.Diagnostics.SetLevel(ZLinkDiagnosticsLevel.Detailed);
        var runtime = provider.GetRequiredService<ZLinkFrameworkRuntime>();
        var locations = provider.GetRequiredService<ZLinkLocationRuntime>();
        var discovery = provider.GetRequiredService<ZLinkLocationAutoConnectHost>();
        await locations.StartAsync(RoutingId.From("self-ack-recovery-node"));
        await runtime.StartAsync(CancellationToken.None);
        IRouterSocket? router = null;
        try
        {
            var state = await runtime.EnsureStartedStateAsync(CancellationToken.None);
            await discovery.StartAsync(state);
            var bundle = state.ClientServerServerBundles[
                ZLinkChannelName.FromBoundary(Channel, "channel")
            ];
            var identity = bundle.ClientServerServer!;
            router = (IRouterSocket)bundle.Socket;
            var client = runtime.GetClientServerClientRuntime(Channel);
            var route = provider.GetRequiredService<IZLinkRouteClient>();
            var before = await route
                .RequestToChannel(Channel, new EchoRequest("before"))
                .Timeout(TimeSpan.FromSeconds(3))
                .Async<EchoReply>();
            Assert.Equal("before", before.Value);
            // Let the initial control replies finish before advancing the monotonic clock.
            await Task.Delay(100);
            var generation = Assert.Single(router.RoutesSnapshot()).RouteGeneration;
            var connection = await client._lane.RunAsync(() =>
                Assert.Single(client._connections.Values)
            );
            var expected = connection.Expected!;
            log?.WriteLine(
                $"before ready={client.ReadyCount} generation={generation} {client.AdmissionDiagnostics}"
            );

            if (expireServer)
            {
                router.SetReceiveFlowState(ReceiveFlowState.Paused);
                time.AdvanceMonotonicWithoutTimers(TimeSpan.FromSeconds(16));
                if (expireClient)
                {
                    time.AdvanceMonotonic(TimeSpan.Zero);
                    await WaitUntilAsync(() =>
                        client.AdmissionDiagnostics.Contains(
                            "liveness:timeout",
                            StringComparison.Ordinal
                        )
                    );
                    Assert.Equal(0, client.ReadyCount);
                }
                // Run the existing liveness owner without introducing another native poller.
                await identity.TickLivenessAsync(
                    router,
                    new IdleReceiveTurn(),
                    CancellationToken.None
                );
                Assert.Equal(0, await identity.GetAdmittedPeerCountAsync());
                log?.WriteLine($"expired ready={client.ReadyCount} {client.AdmissionDiagnostics}");
            }

            if (injectAckFailure)
            {
                // Inject only an ACK completion result at the existing Framework observer boundary.
                await connection.ObserveControlAckAsync(
                    Task.FromException(
                        new ZlinkSubmitException(ZlinkSubmitException.ErrorCode.NotConnected)
                    ),
                    CancellationToken.None
                );
                log?.WriteLine($"ack-observed disposing={connection._disposeTask is not null}");
                Assert.Null(connection._disposeTask);
                await client.ReplaceAutomaticAsync([expected]);
                var retained = await client._lane.RunAsync(() =>
                    Assert.Single(client._connections.Values)
                );
                Assert.Same(connection, retained);
                Assert.Null(retained._disposeTask);
                log?.WriteLine("same-descriptor-reconcile retained=True disposing=False");
            }

            router.SetReceiveFlowState(ReceiveFlowState.Running);
            await WaitUntilAsync(() =>
                client.ReadyCount == 1
                && (
                    !expireServer
                    || router.RoutesSnapshot().Any(route => route.RouteGeneration != generation)
                )
            );
            // One public request must succeed after transport/admission recovery; no request retry.
            var after = await route
                .RequestToChannel(Channel, new EchoRequest("after"))
                .Timeout(TimeSpan.FromSeconds(3))
                .Async<EchoReply>()
                .AsTask()
                .WaitAsync(TimeSpan.FromSeconds(3));
            Assert.Equal("after", after.Value);
            Assert.Null(connection._disposeTask);
            Assert.Same(
                connection,
                await client._lane.RunAsync(() => Assert.Single(client._connections.Values))
            );
            if (expireServer)
                Assert.NotEqual(generation, Assert.Single(router.RoutesSnapshot()).RouteGeneration);
            else
                Assert.Equal(generation, Assert.Single(router.RoutesSnapshot()).RouteGeneration);
            log?.WriteLine(
                $"public-request=succeeded ready={client.ReadyCount} {client.AdmissionDiagnostics}"
            );
        }
        finally
        {
            router?.SetReceiveFlowState(ReceiveFlowState.Running);
            try
            {
                await discovery.StopAsync();
            }
            finally
            {
                try
                {
                    await runtime.StopAsync(CancellationToken.None);
                }
                finally
                {
                    await locations.StopAsync();
                }
            }
        }
    }

    private static async Task WaitUntilAsync(Func<bool> condition)
    {
        var elapsed = Stopwatch.StartNew();
        while (!condition() && elapsed.Elapsed < TimeSpan.FromSeconds(3))
            await Task.Delay(1);
        Assert.True(
            condition(),
            "The existing transport/liveness owner did not complete its transition."
        );
    }

    public sealed record EchoRequest(string Value);

    public sealed record EchoReply(string Value);

    public sealed class EchoHandler : IZLinkRequestHandler<EchoRequest, EchoReply>
    {
        public ValueTask<EchoReply> HandleAsync(
            EchoRequest request,
            IZLinkMessageContext context,
            CancellationToken cancellationToken
        ) => ValueTask.FromResult(new EchoReply(request.Value));
    }

    private sealed class IdleReceiveTurn : IZLinkBackendSocketPoller
    {
        public ValueTask ResumeAsync() => ValueTask.CompletedTask;

        public ValueTask<ZLinkBackendSocketReadiness> WaitAsync(
            TimeSpan timeout,
            CancellationToken cancellationToken
        ) => throw new NotSupportedException();

        public ValueTask DisposeAsync() => ValueTask.CompletedTask;
    }
}
