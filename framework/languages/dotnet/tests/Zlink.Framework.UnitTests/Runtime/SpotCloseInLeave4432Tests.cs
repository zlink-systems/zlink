using System.Collections.Concurrent;
using Zlink.Framework.Contracts.Messaging;
using Zlink.Framework.Runtime.Actors;
using Zlink.Framework.Runtime.Diagnostics;
using Zlink.Framework.Runtime.Dispatch;

namespace Zlink.Framework.UnitTests.Runtime;

public sealed partial class EntrySpotActorDispatchTests
{
    [Theory]
    [InlineData("join", false)]
    [InlineData("join", true)]
    [InlineData("joined", false)]
    [InlineData("joined", true)]
    [InlineData("leave", false)]
    [InlineData("leave", true)]
    public async Task SpotLifecycleSelfCloseIsRejectedAtInvocation(string phase, bool awaitClose)
    {
        var (runtime, actorRef) = await CreateStartedRuntimeAsync(
            new CapturingSpotNode(),
            messageFlowMode: ZLinkDiagnosticsLevel.Detailed,
            userSpotType: typeof(CloseInLeave4432Spot)
        );
        try
        {
            var actor = RegisterProbeActor(runtime, actorRef);
            var source = await runtime.CreateAsync<CloseInLeave4432Spot>();
            var target = await runtime.CreateAsync<CloseInLeave4432Spot>();
            var other = await runtime.CreateAsync<CloseInLeave4432Spot>();
            var spot = CloseInLeave4432Spot.Instances[source.Spot.SpotId];
            spot.Phase = phase;
            spot.AwaitClose = awaitClose;
            spot.Other = CloseInLeave4432Spot.Instances[other.Spot.SpotId].Context;
            Assert.IsType<ZLinkActorJoinResult.Accepted>(
                await runtime.JoinActorAsync(source.Spot.SpotId, actor, ZLinkMessage.Empty, default)
            );
            var pipeline = new ZLinkActorInboundPipeline(
                runtime,
                new ZLinkEntrySpotActorInboundEndpoint(runtime),
                "entry"
            );
            using var incoming = ZLinkActorHandoffIngress.CaptureMovingFrames(
                ZLinkApplicationJobOrigin.Remote,
                runtime,
                CreateActorRequestParts(actorRef, "move4432", target.Spot.SpotId, 4432, 1)
            );
            await pipeline.DispatchOwnedFrameAsync(incoming, null, default);
            await spot.Checked.Task;
            await spot.Left.Task;
            Assert.True(await runtime.CloseAsync(source.Spot));
        }
        finally
        {
            await runtime.StopAsync(default);
            CloseInLeave4432Spot.Instances.Clear();
        }
    }

    private sealed class CloseInLeave4432Spot : IZLinkSpot<ProbeActor>
    {
        internal static readonly ConcurrentDictionary<string, CloseInLeave4432Spot> Instances =
            new();

        public CloseInLeave4432Spot(IZLinkSpotContext context)
        {
            Context = context;
            Instances[context.SpotId] = this;
        }

        public IZLinkSpotContext Context { get; }
        public string? Phase { get; set; }
        public bool AwaitClose { get; set; }
        public IZLinkSpotContext? Other { get; set; }
        public TaskCompletionSource Left { get; } =
            new(TaskCreationOptions.RunContinuationsAsynchronously);
        public TaskCompletionSource Checked { get; } =
            new(TaskCreationOptions.RunContinuationsAsynchronously);

        public void Configure() => Context.Handlers.AddHandler<Move4432Handler>("move4432");

        public ValueTask OnInitializeAsync(CancellationToken cancellationToken)
        {
            var error = Assert.Throws<ZLinkFrameworkException>(() =>
            {
                _ = Context.CloseAsync();
            });
            Assert.Equal(ZLinkFrameworkErrorKind.InvalidOperation, error.Kind);
            return ValueTask.CompletedTask;
        }

        public async ValueTask<ZLinkSpotActorJoinResult> OnActorJoinAsync(
            string actorId,
            ZLinkMessage request,
            CancellationToken cancellationToken
        )
        {
            await CheckClose("join");
            return ZLinkSpotActorJoinResult.Accept();
        }

        public ValueTask OnJoinedActorAsync(
            ProbeActor actor,
            CancellationToken cancellationToken
        ) => CheckClose("joined");

        public async ValueTask OnLeaveActorAsync(
            ProbeActor actor,
            CancellationToken cancellationToken
        )
        {
            await CheckClose("leave");
            Left.SetResult();
        }

        private async ValueTask CheckClose(string phase)
        {
            if (Phase != phase)
                return;
            var error = Assert.Throws<ZLinkFrameworkException>(() =>
            {
                _ = Context.CloseAsync();
            });
            Assert.Equal(ZLinkFrameworkErrorKind.InvalidOperation, error.Kind);
            if (AwaitClose)
            {
                async Task AwaitOwnClose() => await Context.CloseAsync();
                var pending = AwaitOwnClose();
                Assert.True(pending.IsCompleted);
                Assert.Equal(
                    ZLinkFrameworkErrorKind.InvalidOperation,
                    (await Assert.ThrowsAsync<ZLinkFrameworkException>(() => pending)).Kind
                );
            }
            Assert.True(await Other!.CloseAsync());
            Checked.SetResult();
        }
    }

    private sealed class Move4432Handler
        : IZLinkSpotActorRequestHandler<CloseInLeave4432Spot, ProbeActor, string, ProbeReply>
    {
        public ValueTask<ProbeReply> HandleAsync(
            CloseInLeave4432Spot spot,
            ProbeActor actor,
            IZLinkMessageContext context,
            string request,
            CancellationToken cancellationToken
        )
        {
            actor.Context.JoinSpot(request, ZLinkMessage.Empty).Defer();
            return ValueTask.FromResult(new ProbeReply(request));
        }
    }
}
