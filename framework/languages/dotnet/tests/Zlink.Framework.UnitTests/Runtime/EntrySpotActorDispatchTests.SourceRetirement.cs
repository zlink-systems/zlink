using Microsoft.Extensions.DependencyInjection;
using Systems.Zlink.Stream.Connector.Contracts;
using Zlink.Framework.Runtime;
using Zlink.Framework.Runtime.Actors;
using Zlink.Framework.Runtime.Backend.Contracts;
using Zlink.Framework.Runtime.Dispatch;
using Zlink.Framework.Runtime.Identifiers;
using Zlink.Framework.Runtime.Spots;

namespace Zlink.Framework.UnitTests.Runtime;

public sealed partial class EntrySpotActorDispatchTests
{
    [Theory]
    [InlineData(false, false, false, false)]
    [InlineData(true, false, false, false)]
    [InlineData(false, true, false, false)]
    [InlineData(true, true, false, false)]
    [InlineData(false, false, true, false)]
    [InlineData(true, false, true, false)]
    [InlineData(false, true, true, false)]
    [InlineData(true, true, true, false)]
    [InlineData(false, false, false, true)]
    [InlineData(true, false, false, true)]
    public async Task CommittedSource_DisposalFailure_RetiresOnceAndReportsOnce(
        bool deferred,
        bool preparationFails,
        bool leaveFails,
        bool standaloneCancelled
    )
    {
        using var services = new ServiceCollection()
            .AddScoped<ProbeActorFactory>()
            .BuildServiceProvider();
        var node = new CapturingSpotNode();
        node.SetRoutingId(RoutingId.From("source-node"));
        var registration = new ZLinkFrameworkRegistration();
        registration.SpotNodes["source-node"] = new ZLinkSpotNodeRegistration
        {
            SpotNodeName = "source-node",
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
        var created = await sessions.CreateAndBindActorAsync("source-retirement", "probe");
        Assert.True(sessions.TryGetCreatedActorState("source-retirement", out var state));
        var source = state.NativeActorRef!.Value;
        var target = new ZLinkBackendActorRef(
            RoutingId.From("target-node"),
            source.ActorId,
            source.Generation
        );
        void CommitSource()
        {
            state.Handoff.BeginCapture();
            if (leaveFails)
            {
                state.Handoff.SealCapture(new SourceRetirementReservation(), "source-retirement");
                _ = state.Handoff.FreezeCaptureCommitBoundary();
            }
            _ = state.Handoff.CutoverCaptureToMessageFollow(
                0,
                source,
                target,
                "mesh",
                1,
                1,
                1,
                2,
                1,
                2
            );
            state.Handoff.CommitMessageFollow(TimeSpan.FromSeconds(1));
            if (leaveFails)
                state
                    .Handoff.TryBeginSourceMembershipLeave("source-retirement")!
                    .TrySetException(new InvalidOperationException("source leave failed"));
        }
        var handler = state.HandlerInstances.Resolve<SourceRetirementFailingHandler>();
        var diagnosed = new TaskCompletionSource<Exception>(
            TaskCreationOptions.RunContinuationsAsynchronously
        );
        var diagnosticCount = 0;
        runtime.ErrorSink.UnhandledCallbackException += exception =>
        {
            Interlocked.Increment(ref diagnosticCount);
            diagnosed.TrySetResult(exception);
        };
        var preparationCount = 0;
        ValueTask PrepareSource()
        {
            preparationCount++;
            return preparationFails
                ? ValueTask.FromException(
                    new InvalidOperationException("source preparation failed")
                )
                : ValueTask.CompletedTask;
        }
        ValueTask FinalizeSource(CancellationToken token) =>
            standaloneCancelled
                ? runtime.CompleteStandaloneActorRelocationSourceAsync(
                    created.Actor,
                    state,
                    source,
                    target,
                    new CancellationToken(true)
                )
                : sessions.FinalizeMigratedSourceAsync(state, source, token, PrepareSource);

        if (deferred)
        {
            var header = new ZlinkStreamHeader(
                ZlinkStreamMessageKind.Send,
                ZlinkStreamCodec.Json,
                ZlinkStreamHeaderFlags.None,
                null,
                "source-retirement",
                ZlinkStreamMetadata.Empty
            );
            await state.ExecuteDispatchAsync(
                header,
                token =>
                {
                    CommitSource();
                    return FinalizeSource(token);
                },
                CancellationToken.None
            );
        }
        else
        {
            CommitSource();
            await FinalizeSource(CancellationToken.None);
        }

        var failure = await diagnosed.Task.WaitAsync(TimeSpan.FromSeconds(5));
        if (preparationFails || leaveFails)
            Assert.Equal(
                1 + (preparationFails ? 1 : 0) + (leaveFails ? 1 : 0),
                Assert.IsType<AggregateException>(failure).InnerExceptions.Count
            );
        else
            Assert.IsType<InvalidOperationException>(failure);
        Assert.Null(state.Actor);
        Assert.Null(state.Context);
        Assert.False(sessions.IsCurrentLocalActor(source));
        Assert.Equal(1, handler.DisposeCount);
        Assert.Equal(1, diagnosticCount);
        await FinalizeSource(CancellationToken.None);
        Assert.Equal(standaloneCancelled ? 0 : 1, preparationCount);
        Assert.Equal(1, handler.DisposeCount);
        Assert.Equal(1, diagnosticCount);
        await state.Handoff.WaitForSourceCompletionAsync(CancellationToken.None);
        var restored = await sessions.EnsureProvisionalActorAsync(
            source.ActorId,
            "probe",
            source.Generation,
            3,
            CancellationToken.None
        );
        Assert.NotSame(created.Actor, restored.Actor);
        Assert.Equal(source.Generation, restored.Actor.Context.ObjectGeneration);
        Assert.Same(restored.Actor, state.Actor);
    }

    private sealed class SourceRetirementFailingHandler : IAsyncDisposable
    {
        internal int DisposeCount { get; private set; }

        public ValueTask DisposeAsync()
        {
            DisposeCount++;
            return ValueTask.FromException(
                new InvalidOperationException("source handler disposal failed")
            );
        }
    }

    private sealed class SourceRetirementReservation : IDisposable
    {
        public void Dispose() { }
    }
}
