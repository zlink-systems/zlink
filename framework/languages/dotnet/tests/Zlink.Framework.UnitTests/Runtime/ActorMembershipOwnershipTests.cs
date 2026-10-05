using Microsoft.Extensions.DependencyInjection;
using Zlink.Framework.Runtime.Actors;
using Zlink.Framework.Runtime.Backend.Contracts;
using Zlink.Framework.Runtime.Execution;
using Zlink.Framework.Runtime.Spots;

namespace Zlink.Framework.UnitTests.Runtime;

public sealed partial class EntrySpotActorDispatchTests
{
    [Fact]
    public async Task ActorMembershipCommit_UsesStateLaneAfterExternalAuthorityCompletion()
    {
        using var services = new ServiceCollection()
            .AddScoped<ProbeActorFactory>()
            .BuildServiceProvider();
        var node = new CapturingSpotNode();
        node.SetRoutingId(RoutingId.From("membership-node"));
        var registration = new ZLinkFrameworkRegistration();
        registration.SpotNodes["membership-node"] = new ZLinkSpotNodeRegistration
        {
            SpotNodeName = "membership-node",
            ActorFactories = { ["probe"] = typeof(ProbeActorFactory) },
        };
        registration.ActorCatalog.Build(registration.SpotNodes.Values);
        var runtime = new ZLinkFrameworkRuntime(
            services,
            new CapturingBackendAdapterFactory(node),
            registration,
            new ZLinkHandlerRegistry([]),
            new ZLinkHandlerDispatcher(
                services.GetRequiredService<IServiceScopeFactory>(),
                registration
            )
        );
        var sessions = new ZLinkActorSessionManager(
            runtime,
            services,
            () => node,
            null,
            new ZLinkBoundSessionService(runtime)
        );
        var actor = await sessions.CreateAndBindActorAsync("membership-actor", "probe");
        await using var activation = new ZLinkUserSpotActivation(
            runtime,
            services.CreateAsyncScope(),
            new CapturingSpot(),
            "membership-target",
            RoutingId.From("membership-node"),
            "membership-node",
            "membership-channel",
            TimeSpan.FromSeconds(1),
            ZLinkUserSpotExecutionMode.SpotWide,
            ZLinkSpotRelocationCoordinationMode.ApplicationSignaled
        );
        activation.AttachSpot(new RelocationReadyProbeSpot(activation));
        var authorityCompleted = false;
        ZLinkStateLane? publicationLane = null;
        await sessions.CommitActorToSpotAsync(
            activation,
            actor.Actor,
            async _ =>
            {
                Assert.Null(ZLinkStateLane.Current);
                await Task.Yield();
                authorityCompleted = true;
            },
            () =>
            {
                Assert.True(authorityCompleted);
                publicationLane = ZLinkStateLane.Current;
            }
        );
        Assert.NotNull(publicationLane);
        Assert.Same(activation, sessions.GetOrCreateState("membership-actor").Activation);
    }
}
