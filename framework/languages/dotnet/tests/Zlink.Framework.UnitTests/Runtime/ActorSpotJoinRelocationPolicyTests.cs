using Microsoft.Extensions.DependencyInjection;
using Zlink.Framework.Contracts.Configuration;
using Zlink.Framework.Contracts.Messaging;
using Zlink.Framework.Runtime.Backend.Contracts;
using Zlink.Framework.Runtime.Backend.DotNet;

namespace Zlink.Framework.UnitTests.Runtime;

public sealed class ActorSpotJoinRelocationPolicyTests(Xunit.Abstractions.ITestOutputHelper output)
{
    private const string EnabledActorType = "relocation-enabled-actor";
    private static readonly TimeSpan Wait = TimeSpan.FromSeconds(10);

    [Theory]
    [InlineData(false)]
    [InlineData(true)]
    public async Task DisabledPolicyRejectsOnlyCrossNodeJoin(bool mixedFactories)
    {
        using var flow = Environment.GetEnvironmentVariable("ZLINK_TEST_FLOW_DIRECTORY")
            is { } flowDirectory
            ? new TestHostMessageFlowListener(
                Path.Combine(flowDirectory, $"actor-join-policy-{Guid.NewGuid():N}.flow")
            )
            : null;
        var wireRequests = 0;
        var backendProbe = new CanonicalRelocationTransportProbe(new RelocationBehaviorTrace())
        {
            OnCanonicalActorJoinSubmitted = () => Interlocked.Increment(ref wireRequests),
        };
        await using var target = await SpotCloseHost.StartAsync(
            configureServer: ConfigureServer,
            actorLimit: 1
        );
        var spot = await target.CreateSpotAsync();
        var localActor = await target.CreateActorAsync();
        await AssertLocalActorAsync(target, localActor);

        var admissions = 0;
        string? enabledActor = null;
        target.State.OnJoin = actorId =>
        {
            Interlocked.Increment(ref admissions);
            return actorId == enabledActor
                ? ZLinkSpotActorJoinResult.Reject()
                : ZLinkSpotActorJoinResult.Accept();
        };
        await using var source = await SpotCloseHost.StartAsync(
            target.Store,
            registerUserSpotFactory: false,
            configureServer: ConfigureServer
        );
        using var peerDeadline = new CancellationTokenSource(Wait);
        await Task.WhenAll(
            source.WaitForPeerAsync(target, peerDeadline.Token),
            target.WaitForPeerAsync(source, peerDeadline.Token)
        );

        await target.JoinActorAsync(spot.SpotId, localActor);
        Assert.IsType<ZLinkActorJoinCompletion.Accepted>(
            await target.State.JoinCompletion.Task.WaitAsync(Wait)
        );
        output.WriteLine($"local: callbacks={admissions}, canonical requests={wireRequests}");
        Assert.Equal(1, Volatile.Read(ref admissions));
        Assert.Equal(0, Volatile.Read(ref wireRequests));
        Interlocked.Exchange(ref admissions, 0);

        var remoteActor = await source.CreateActorAsync();
        var remoteActorRef = await AssertLocalActorAsync(source, remoteActor);
        await source.JoinActorAsync(spot.SpotId, remoteActor);
        var deferred = Assert.IsType<ZLinkActorJoinCompletion.Failed>(
            await source.State.JoinCompletion.Task.WaitAsync(Wait)
        );
        output.WriteLine(
            $"remote deferred: kind={deferred.Kind}, callbacks={admissions}, canonical requests={wireRequests}"
        );
        Assert.Equal(ZLinkFrameworkErrorKind.Rejected, deferred.Kind);
        Assert.Equal((0, 0), (Volatile.Read(ref admissions), Volatile.Read(ref wireRequests)));

        var immediate = await Assert.ThrowsAsync<ZLinkFrameworkException>(() =>
            source.Runtime.JoinActorAsync(spot.SpotId, remoteActorRef, ZLinkMessage.Empty).AsTask()
        );
        output.WriteLine(
            $"remote immediate: kind={immediate.Kind}, callbacks={admissions}, canonical requests={wireRequests}"
        );
        Assert.Equal(ZLinkFrameworkErrorKind.Rejected, immediate.Kind);
        Assert.Equal((0, 0), (Volatile.Read(ref admissions), Volatile.Read(ref wireRequests)));

        if (mixedFactories)
        {
            enabledActor = await source.CreateActorAsync(EnabledActorType);
            await AssertLocalActorAsync(source, enabledActor);
            source.State.JoinCompletion = new(TaskCreationOptions.RunContinuationsAsynchronously);
            await source.JoinActorAsync(spot.SpotId, enabledActor);
            Assert.IsType<ZLinkActorJoinCompletion.Rejected>(
                await source.State.JoinCompletion.Task.WaitAsync(Wait)
            );
            output.WriteLine(
                $"remote enabled: callbacks={admissions}, canonical requests={wireRequests}"
            );
            Assert.Equal((1, 1), (Volatile.Read(ref admissions), Volatile.Read(ref wireRequests)));
        }

        void ConfigureServer(IServiceCollection services, IZLinkMeshObjectServerBuilder server)
        {
            services.AddSingleton<IZLinkBackendAdapterFactory>(
                new ProbedBackendAdapterFactory(
                    new ZLinkDotNetBackendAdapterFactory(),
                    backendProbe,
                    routeProxy: null
                )
            );
            if (mixedFactories)
                server.AddActorFactory<SpotCloseActor, SpotCloseActorFactory>(
                    EnabledActorType,
                    static factory => factory.RecreateOnRelocation()
                );
        }
    }

    private static async Task<ActorRef> AssertLocalActorAsync(SpotCloseHost host, string actorId)
    {
        var actor = Assert.NotNull(
            await host.Runtime.Services.GetRequiredService<IZLinkActorManager>().FindAsync(actorId)
        );
        Assert.Equal(
            host.Runtime.GetSpotNodeRuntime(SpotCloseHost.MeshName).Node.RoutingId,
            actor.NodeRid
        );
        return actor;
    }
}
