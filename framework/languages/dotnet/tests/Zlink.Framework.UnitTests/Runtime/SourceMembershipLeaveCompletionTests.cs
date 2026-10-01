using Microsoft.Extensions.DependencyInjection;
using Zlink.Framework.Contracts.Messaging;
using Zlink.Framework.Runtime.Backend.Contracts;

namespace Zlink.Framework.UnitTests.Runtime;

public sealed partial class EntrySpotActorDispatchTests
{
    [Fact]
    public async Task CommittedSourceLeave_ClosedEntryQueueReportsOnce()
    {
        using var services = new ServiceCollection().BuildServiceProvider();
        var (entry, runtime) = CreateActivationWithRuntime(
            services,
            new CapturingSpot(),
            typeof(OneWayLeaveEntrySpot)
        );
        var actor = RegisterProbeActor(
            runtime,
            new ZLinkBackendActorRef(RoutingId.From("entry-node"), "actor-a", 1)
        );
        var source = Assert.IsType<OneWayLeaveEntrySpot>(entry.EntrySpot);
        entry.Configure();
        Assert.True(entry.TryResolveActorLeft(actor.GetType(), out _));
        await entry.DisposeAsync();
        var reportCount = 0;
        var failure = new TaskCompletionSource<Exception>(
            TaskCreationOptions.RunContinuationsAsynchronously
        );
        runtime.ErrorSink.UnhandledCallbackException += exception =>
        {
            Interlocked.Increment(ref reportCount);
            failure.TrySetResult(exception);
        };
        entry.SubmitActorLeft(actor);
        Assert.False(source.Probe.Started.Task.IsCompleted);
        var reported = Assert.IsType<ZLinkFrameworkException>(
            await failure.Task.WaitAsync(TimeSpan.FromSeconds(5))
        );
        Assert.Equal(ZLinkFrameworkErrorKind.ShuttingDown, reported.Kind);
        Assert.Equal(1, reportCount);
    }

    [Theory]
    [InlineData(false, false)]
    [InlineData(true, false)]
    [InlineData(false, true)]
    [InlineData(true, true)]
    public async Task CommittedSourceLeave_DoesNotBlockJoinAndReportsFailureOnce(
        bool entrySource,
        bool deferred
    )
    {
        var (runtime, actorRef) = await CreateStartedRuntimeAsync(
            new CapturingSpotNode(),
            entrySpotType: entrySource ? typeof(OneWayLeaveEntrySpot) : null,
            userSpotType: typeof(OneWayLeaveUserSpot)
        );
        OneWayLeaveProbe? probe = null;
        Zlink.Framework.Runtime.Spots.ZLinkSpotActivation? sourceActivation = null;
        Task<ZLinkActorJoinResult>? join = null;
        var reportCount = 0;
        try
        {
            var actor = RegisterProbeActor(runtime, actorRef);
            if (entrySource)
                probe = Assert
                    .IsType<OneWayLeaveEntrySpot>(
                        runtime.GetSpotNodeRuntime("entry").EntrySpotActivation!.EntrySpot
                    )
                    .Probe;
            else
            {
                var source = await runtime.CreateAsync<OneWayLeaveUserSpot>();
                Assert.IsType<ZLinkActorJoinResult.Accepted>(
                    await runtime.JoinActorAsync(source.Spot.SpotId, actor, ZLinkMessage.Empty)
                );
                sourceActivation = runtime
                    .GetSpotNodeRuntime("entry")
                    .Spots.Single(activation => activation.SpotId == source.Spot.SpotId);
                probe = Assert.IsType<OneWayLeaveUserSpot>(sourceActivation.Spot).Probe;
            }
            var failure = new TaskCompletionSource<Exception>(
                TaskCreationOptions.RunContinuationsAsynchronously
            );
            runtime.ErrorSink.UnhandledCallbackException += exception =>
            {
                Interlocked.Increment(ref reportCount);
                failure.TrySetResult(exception);
            };
            var target = await runtime.CreateAsync<OneWayLeaveUserSpot>();
            if (deferred)
            {
                using (var handler = ZLinkDeferredActorJoinHandlerScope.Open())
                {
                    actor.Context.JoinSpot(target.Spot.SpotId, ZLinkMessage.Empty).Defer();
                    handler.Complete();
                }
                Assert.IsType<ZLinkActorJoinCompletion.Accepted>(
                    await actor.JoinCompletion.Task.WaitAsync(TimeSpan.FromSeconds(5))
                );
            }
            else
            {
                join = runtime
                    .JoinActorAsync(target.Spot.SpotId, actor, ZLinkMessage.Empty)
                    .AsTask();
                Assert.IsType<ZLinkActorJoinResult.Accepted>(
                    await join.WaitAsync(TimeSpan.FromSeconds(5))
                );
            }
            if (sourceActivation is not null)
                Assert.Equal(0, sourceActivation.JoinedActorCount);
            Assert.Equal(target.Spot.SpotId, actor.Context.SpotId);
            await probe.Started.Task.WaitAsync(TimeSpan.FromSeconds(5));
            Assert.False(probe.Release.Task.IsCompleted);
            Assert.IsType<ZLinkActorJoinResult.Accepted>(
                await runtime
                    .JoinActorAsync(target.Spot.SpotId, actor, ZLinkMessage.Empty)
                    .AsTask()
                    .WaitAsync(TimeSpan.FromSeconds(5))
            );
            probe.Release.SetResult();
            Assert.Same(probe.Failure, await failure.Task.WaitAsync(TimeSpan.FromSeconds(5)));
        }
        finally
        {
            probe?.Release.TrySetResult();
            if (join is not null)
                await join.WaitAsync(TimeSpan.FromSeconds(5));
            await runtime.StopAsync(CancellationToken.None);
        }
        Assert.Equal(1, probe!.Calls);
        Assert.Equal(1, reportCount);
    }

    private sealed class OneWayLeaveProbe
    {
        public TaskCompletionSource Started { get; } =
            new(TaskCreationOptions.RunContinuationsAsynchronously);
        public TaskCompletionSource Release { get; } =
            new(TaskCreationOptions.RunContinuationsAsynchronously);
        public Exception Failure { get; } = new InvalidOperationException("Source leave failure.");
        public int Calls { get; private set; }

        public async ValueTask InvokeAsync(CancellationToken cancellationToken)
        {
            Calls++;
            Started.TrySetResult();
            await Release.Task.WaitAsync(cancellationToken);
            throw Failure;
        }
    }

    private sealed class OneWayLeaveUserSpot(IZLinkSpotContext context) : IZLinkSpot<ProbeActor>
    {
        public IZLinkSpotContext Context { get; } = context;
        public OneWayLeaveProbe Probe { get; } = new();

        public ValueTask<ZLinkSpotActorJoinResult> OnActorJoinAsync(
            string actorId,
            ZLinkMessage request,
            CancellationToken cancellationToken
        ) => ValueTask.FromResult(ZLinkSpotActorJoinResult.Accept());

        public ValueTask OnJoinedActorAsync(
            ProbeActor actor,
            CancellationToken cancellationToken
        ) => ValueTask.CompletedTask;

        public ValueTask OnLeaveActorAsync(ProbeActor actor, CancellationToken cancellationToken) =>
            Probe.InvokeAsync(cancellationToken);
    }

    private sealed class OneWayLeaveEntrySpot(IZLinkEntrySpotContext context)
        : IZLinkEntrySpot<ProbeActor>
    {
        public IZLinkEntrySpotContext Context { get; } = context;
        public OneWayLeaveProbe Probe { get; } = new();

        public ValueTask<ZLinkActorCreateResponse> OnCreateActorAsync(
            ProbeActor actor,
            ZLinkMessage request,
            CancellationToken cancellationToken
        ) => ValueTask.FromResult(ZLinkActorCreateResponse.Accept());

        public ValueTask OnJoinedActorAsync(
            ProbeActor actor,
            CancellationToken cancellationToken
        ) => ValueTask.CompletedTask;

        public ValueTask OnLeaveActorAsync(ProbeActor actor, CancellationToken cancellationToken) =>
            Probe.InvokeAsync(cancellationToken);
    }
}
