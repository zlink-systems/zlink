using Microsoft.Extensions.DependencyInjection;
using Zlink.Framework.Contracts.Messaging;
using Zlink.Framework.Runtime.Actors;
using Zlink.Framework.Runtime.Backend.Contracts;
using Zlink.Framework.Runtime.Execution;
using Zlink.Framework.Runtime.Spots;

namespace Zlink.Framework.UnitTests.Runtime;

public sealed partial class EntrySpotActorDispatchTests
{
    [Fact]
    public async Task CommittedMembershipRetry_YieldsSpotGateAndReacquiresBeforeCallback()
    {
        var probe = new CommittedJoinRetryProbe { Failures = 2 };
        var (runtime, actorRef) = await CreateStartedRuntimeAsync(
            new CapturingSpotNode(),
            userSpotType: typeof(RetryingJoinTargetSpot),
            committedJoinRetryProbe: probe,
            messageFlowMode: ZLinkDiagnosticsLevel.Normal
        );
        using var cancellation = new CancellationTokenSource();
        var entered = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        var release = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        var handlers = new List<Task>();
        Task<ZLinkSpotActorJoinResult>? join = null;
        var handlerActive = false;
        var handled = 0;
        try
        {
            var actor = RegisterProbeActor(runtime, actorRef);
            var created = await runtime.CreateAsync<RetryingJoinTargetSpot>();
            var activation = Assert.Single(
                runtime.GetSpotNodeRuntime("entry").Catalog.Spots,
                candidate => candidate.SpotId == created.Spot.SpotId
            );
            probe.OnAttempt = attempt =>
            {
                Assert.NotNull(ZLinkSerialTurn.Current);
                Assert.True(activation._serial.IsCurrentLifecycleTurn);
                Assert.False(handlerActive);
                Assert.Equal(attempt - 1, handled);
                if (attempt > probe.Failures)
                    return;
                handlers.Add(
                    activation
                        ._serial.ExecuteAsync(
                            async (_, ct) =>
                            {
                                handlerActive = true;
                                entered.TrySetResult();
                                await release.Task.WaitAsync(ct);
                                handled++;
                                handlerActive = false;
                            },
                            cancellation.Token
                        )
                        .AsTask()
                );
            };
            join = activation
                .JoinActorAsync(actor, ZLinkMessage.Empty, cancellation.Token)
                .AsTask();
            await entered.Task.WaitAsync(TimeSpan.FromSeconds(5));
            Assert.Equal(1, probe.Attempts);
            Assert.False(join.IsCompleted);
            release.TrySetResult();
            Assert.True((await join.WaitAsync(TimeSpan.FromSeconds(5))).Accepted);
            await Task.WhenAll(handlers);
            Assert.Equal(3, probe.Attempts);
            Assert.Equal(2, handled);
            Assert.True(activation.ContainsActor(actor.Context.ActorId));
        }
        finally
        {
            release.TrySetResult();
            cancellation.Cancel();
            await runtime.StopAsync(CancellationToken.None);
        }
    }

    [Fact]
    public async Task CommittedMembershipRetry_DeadlinePreservesCallbackFailure()
    {
        var probe = new CommittedJoinRetryProbe { Failures = int.MaxValue };
        var (runtime, actorRef) = await CreateStartedRuntimeAsync(
            new CapturingSpotNode(),
            userSpotType: typeof(RetryingJoinTargetSpot),
            committedJoinRetryProbe: probe,
            messageFlowMode: ZLinkDiagnosticsLevel.Normal
        );
        try
        {
            var actor = RegisterProbeActor(runtime, actorRef);
            var created = await runtime.CreateAsync<RetryingJoinTargetSpot>();
            var activation = Assert.Single(
                runtime.GetSpotNodeRuntime("entry").Catalog.Spots,
                candidate => candidate.SpotId == created.Spot.SpotId
            );
            var failure = await Assert.ThrowsAsync<ZLinkFrameworkException>(async () =>
                await activation.JoinActorAsync(
                    actor,
                    ZLinkMessage.Empty,
                    CancellationToken.None,
                    DateTimeOffset.UtcNow.AddMilliseconds(100)
                )
            );
            Assert.True(probe.Attempts > 0);
            Assert.Equal(ZLinkFrameworkErrorKind.DeadlineExceeded, failure.Kind);
            Assert.Same(probe.Failure, failure.InnerException);
            Assert.True(activation.ContainsActor(actor.Context.ActorId));
        }
        finally
        {
            await runtime.StopAsync(CancellationToken.None);
        }
    }

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
            ActorFactories =
            {
                ["probe"] = new ZLinkActorFactoryRegistration(
                    typeof(ProbeActorFactory),
                    new ZLinkObjectRelocationRegistration(
                        typeof(ProbeActor),
                        new ZLinkObjectPlacementOptions(),
                        ZLinkObjectRelocationRegistration.DisabledPolicy,
                        null,
                        null
                    )
                ),
            },
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
