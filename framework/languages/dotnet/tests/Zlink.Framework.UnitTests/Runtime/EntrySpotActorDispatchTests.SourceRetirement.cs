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
                2,
                TimeSpan.FromSeconds(1)
            );
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
            await state.ExecuteDispatchAsync(
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
        Assert.Null(await sessions.FindActorAsync(source.ActorId));
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

    [Theory]
    [InlineData(false, false, false, false)]
    [InlineData(true, false, false, false)]
    [InlineData(false, true, false, false)]
    [InlineData(true, false, true, false)]
    [InlineData(false, false, false, true)]
    public async Task ReturningTarget_WaitsForPreviousActivationDisposal(
        bool deferred,
        bool prepareBeforeRetirement,
        bool disposalFails,
        bool cancelPreparation
    )
    {
        await using var services = new ServiceCollection()
            .AddScoped<ProbeActorFactory>()
            .BuildServiceProvider();
        var node = new CapturingSpotNode();
        node.SetRoutingId(RoutingId.From("old-source"));
        var registration = new ZLinkFrameworkRegistration();
        registration.SpotNodes["source-node"] = new ZLinkSpotNodeRegistration
        {
            SpotNodeName = "source-node",
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
        var created = await sessions.CreateAndBindActorAsync("returning-target", "probe");
        Assert.True(sessions.TryGetCreatedActorState("returning-target", out var state));
        var source = state.NativeActorRef!.Value;
        var target = new ZLinkBackendActorRef(RoutingId.From("first-target"), state.ActorId, 1);
        state.BindNativeActorRef(source);
        void CommitSource()
        {
            state.Handoff.BeginCapture();
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
                2,
                TimeSpan.FromSeconds(1)
            );
        }
        var handler = state.HandlerInstances.Resolve<SourceRetirementBlockingHandler>();
        handler.FailDisposal = disposalFails;
        var diagnosed = new TaskCompletionSource<Exception>(
            TaskCreationOptions.RunContinuationsAsynchronously
        );
        runtime.ErrorSink.UnhandledCallbackException += error => diagnosed.TrySetResult(error);
        using var preparationCancellation = new CancellationTokenSource();
        Task retirement = Task.CompletedTask;
        Task preparation = Task.CompletedTask;
        try
        {
            if (deferred)
                retirement = state
                    .ExecuteDispatchAsync(
                        token =>
                        {
                            CommitSource();
                            return sessions.FinalizeMigratedSourceAsync(state, source, token);
                        },
                        CancellationToken.None
                    )
                    .AsTask();
            else
            {
                CommitSource();
                if (prepareBeforeRetirement)
                {
                    preparation = sessions
                        .PrepareForTransferredActivationAsync(state, preparationCancellation.Token)
                        .AsTask();
                    Assert.False(preparation.IsCompleted);
                    Assert.Same(created.Actor, state.Actor);
                }
                retirement = sessions.FinalizeMigratedSourceAsync(state, source).AsTask();
            }
            _ = await Task.WhenAny(handler.DisposalStarted.Task, retirement);
            if (retirement.IsFaulted || retirement.IsCanceled)
                await retirement;
            await handler.DisposalStarted.Task;
            await state.Handoff.WaitForSourceCompletionAsync(CancellationToken.None);
            Assert.Null(state.Actor);
            Assert.Throws<InvalidOperationException>(() => state.HandlerInstances);

            if (!prepareBeforeRetirement)
                preparation = sessions
                    .PrepareForTransferredActivationAsync(state, preparationCancellation.Token)
                    .AsTask();
            Assert.False(preparation.IsCompleted, preparation.Exception?.ToString());
            Assert.Equal(0, handler.DisposeCount);
            Assert.Throws<InvalidOperationException>(() => state.HandlerInstances);

            if (cancelPreparation)
            {
                preparationCancellation.Cancel();
                await Assert.ThrowsAnyAsync<OperationCanceledException>(() => preparation);
                Assert.Equal(0, handler.DisposeCount);
                Assert.Throws<InvalidOperationException>(() => state.HandlerInstances);
            }
            handler.ReleaseDisposal.SetResult();
            await retirement;
            if (cancelPreparation)
                await sessions.PrepareForTransferredActivationAsync(state, CancellationToken.None);
            else
                await preparation;
            if (disposalFails)
                Assert.Equal("source disposal failed", (await diagnosed.Task).Message);

            Assert.Equal(1, handler.DisposeCount);
            var replacement = state.HandlerInstances.Resolve<SourceRetirementBlockingHandler>();
            Assert.NotSame(handler, replacement);
            replacement.ReleaseDisposal.SetResult();
            var dispatched = false;
            await state.ExecuteDispatchAsync(
                _ =>
                {
                    dispatched = true;
                    return ValueTask.CompletedTask;
                },
                CancellationToken.None
            );
            Assert.True(dispatched);
        }
        finally
        {
            handler.ReleaseDisposal.TrySetResult();
            try
            {
                await retirement;
                // Observe a failed preparation as well as joining a suspended one.
                _ = await Record.ExceptionAsync(() => preparation);
            }
            finally
            {
                await state.InvalidateRuntimeGenerationAfterDispatchesAsync();
            }
        }
    }

    private sealed class SourceRetirementBlockingHandler : IAsyncDisposable
    {
        internal TaskCompletionSource DisposalStarted { get; } =
            new(TaskCreationOptions.RunContinuationsAsynchronously);
        internal TaskCompletionSource ReleaseDisposal { get; } =
            new(TaskCreationOptions.RunContinuationsAsynchronously);
        internal int DisposeCount { get; private set; }
        internal bool FailDisposal { get; set; }

        public async ValueTask DisposeAsync()
        {
            DisposalStarted.TrySetResult();
            await ReleaseDisposal.Task;
            DisposeCount++;
            if (FailDisposal)
                throw new InvalidOperationException("source disposal failed");
        }
    }

    [Fact]
    public async Task CommittedSource_UnfinishedLeave_StopsWaitingAtCleanupDeadline()
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
        _ = await sessions.CreateAndBindActorAsync("source-unfinished-leave", "probe");
        Assert.True(sessions.TryGetCreatedActorState("source-unfinished-leave", out var state));
        var source = state.NativeActorRef!.Value;
        var target = new ZLinkBackendActorRef(
            RoutingId.From("target-node"),
            source.ActorId,
            source.Generation
        );
        state.Handoff.BeginCapture();
        state.Handoff.SealCapture(new SourceRetirementReservation(), "source-unfinished-leave");
        _ = state.Handoff.FreezeCaptureCommitBoundary();
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
            2,
            TimeSpan.FromSeconds(1)
        );
        Assert.NotNull(state.Handoff.TryBeginSourceMembershipLeave("source-unfinished-leave"));
        var diagnosed = new TaskCompletionSource<Exception>(
            TaskCreationOptions.RunContinuationsAsynchronously
        );
        runtime.ErrorSink.UnhandledCallbackException += exception =>
            diagnosed.TrySetResult(exception);
        using var deadline = new CancellationTokenSource(TimeSpan.FromMilliseconds(50));

        await sessions
            .FinalizeMigratedSourceAsync(state, source, deadline.Token)
            .AsTask()
            .WaitAsync(TimeSpan.FromSeconds(5));

        Assert.IsAssignableFrom<OperationCanceledException>(
            await diagnosed.Task.WaitAsync(TimeSpan.FromSeconds(5))
        );
        Assert.Null(state.Actor);
        Assert.Null(await sessions.FindActorAsync(source.ActorId));
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
