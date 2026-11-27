using Microsoft.Extensions.DependencyInjection;
using Zlink.Framework.Contracts.Actors;
using Zlink.Framework.Contracts.Errors;
using Zlink.Framework.Contracts.Handlers;
using Zlink.Framework.Contracts.Messaging;
using Zlink.Framework.Contracts.Spots;
using Zlink.Framework.Runtime.Actors;
using Zlink.Framework.Runtime.Host;
using Zlink.Framework.Runtime.Locations;

namespace Zlink.Framework.Locations.Redis.Tests;

public sealed partial class ActorManagerProductionTests
{
    [Fact]
    public async Task RequestDuringClosingCasIsUnavailableAndServedAfterCasFailure()
    {
        const string actorId = "destroy-closing-cas";
        var key = ZLinkActorAuthorityPayloadCodec.AuthorityKey(actorId);
        var store = new DestroyLocationStore(key, holdAuthorityRead: false);
        var repository = new ZLinkProviderLocationRepository(store);
        await using var owner = BuildServer(store, "tcp://127.0.0.1:0");
        var runtime = owner.GetRequiredService<ZLinkFrameworkRuntime>();
        await FrameworkHost(owner).StartAsync(CancellationToken.None);
        using var timeout = new CancellationTokenSource(TimeSpan.FromSeconds(10));
        var ct = timeout.Token;

        try
        {
            var actors = owner.GetRequiredService<IZLinkActorManager>();
            var actor = Assert
                .IsType<ZLinkActorCreateResult.Created>(
                    await actors.GetOrCreate(actorId, "player").Async(ct)
                )
                .Actor;
            var client = owner.GetRequiredService<IZLinkActorClient>();
            var probe = new ClosingProbe(27);
            Assert.Equal(
                probe,
                await client.RequestToActor(actorId, probe).Async<ClosingProbe>(ct)
            );

            store.FailClosing = true;
            var destroy = actors.DestroyAsync(actor, ct).AsTask();
            Exception? duringClosing;
            try
            {
                await store.ClosingStarted.Task.WaitAsync(ct);
                duringClosing = await Record.ExceptionAsync(async () =>
                    await client.RequestToActor(actorId, probe).Async<ClosingProbe>(ct)
                );
            }
            finally
            {
                store.ReleaseClosing.TrySetResult();
            }

            await Assert.ThrowsAsync<ZLinkFrameworkException>(() => destroy);
            var failure = Assert.IsType<ZLinkFrameworkException>(duringClosing);
            Assert.Equal(ZLinkFrameworkErrorKind.Unavailable, failure.Kind);
            var ready = Assert.IsType<ZLinkAuthorityReadResult.Found>(
                await repository.ReadAuthorityAsync(key, ct)
            );
            Assert.True(ZLinkActorAuthorityPayloadCodec.IsSteadyReady(ready.Snapshot.Payload.Span));
            Assert.Equal(actor, await actors.FindAsync(actorId, ct));
            Assert.Equal(
                probe,
                await client.RequestToActor(actorId, probe).Async<ClosingProbe>(ct)
            );

            store.FailClosing = false;
            var accepted = actors.DestroyAsync(actor, ct).AsTask();
            await store.DeleteStarted.Task.WaitAsync(ct);
            var afterClosing = await Record.ExceptionAsync(async () =>
                await client.RequestToActor(actorId, probe).Async<ClosingProbe>(ct)
            );
            store.ReleaseDelete.TrySetResult();
            Assert.Equal(
                ZLinkFrameworkErrorKind.NotFound,
                Assert.IsType<ZLinkFrameworkException>(afterClosing).Kind
            );
            Assert.True(await accepted);
        }
        finally
        {
            store.ReleaseClosing.TrySetResult();
            store.ReleaseDelete.TrySetResult();
            await runtime.StopAsync(CancellationToken.None);
        }
    }

    private sealed record ClosingProbe(int Value);

    private sealed class ClosingProbeHandler
        : IZLinkEntrySpotActorRequestHandler<TestEntrySpot, TestActor, ClosingProbe, ClosingProbe>
    {
        public ValueTask<ClosingProbe> HandleAsync(
            TestEntrySpot spot,
            TestActor actor,
            IZLinkMessageContext context,
            ClosingProbe request,
            CancellationToken cancellationToken
        ) => ValueTask.FromResult(request);
    }
}
