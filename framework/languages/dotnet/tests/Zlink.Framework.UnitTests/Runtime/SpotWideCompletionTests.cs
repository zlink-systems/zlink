using Microsoft.Extensions.DependencyInjection;
using Zlink.Framework.LocationProvider;
using Zlink.Framework.Runtime.Actors;
using Zlink.Framework.Runtime.Locations;

namespace Zlink.Framework.UnitTests.Runtime;

public sealed partial class HostRelocationSealStageTests
{
    [Fact]
    public async Task SpotWide_publication_finishes_while_the_result_CAS_response_is_held()
    {
        var capture = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        var trace = new RelocationBehaviorTrace { SourceActorCaptureGate = capture.Task };
        CommitResponseGateStore? publication = null;
        ZLinkProviderLocationRepository? repository = null;
        Guid relocationId = Guid.Empty;
        var timeout = TimeSpan.FromSeconds(10);

        await RunBoundMemberRelocationAsync(
            trace,
            async context =>
            {
                var relocation = context.StartRelocation();
                Task<BehaviorAck>? reply = null;
                try
                {
                    await trace.WaitAsync("sourceStateCapturing");
                    reply = context
                        .SessionServices.GetRequiredService<IZLinkActorClient>()
                        .RequestToActor(context.ActorId, new BehaviorWork("saved"))
                        .Timeout(TimeSpan.FromSeconds(15))
                        .Async<BehaviorAck>()
                        .AsTask();
                    await context.Backlog.WaitForBacklogAsync(context.ActorId).WaitAsync(timeout);
                    capture.TrySetResult();

                    _ = await publication!.Committed.Task.WaitAsync(timeout);
                    Assert.False(reply.IsCompleted);
                    var row = Assert.Single(
                        await repository!.ReadRelocationRepliesAsync(relocationId, default)
                    );
                    Assert.Equal(ZLinkRelocationCompletionDelivery.Pending, row.Entry.Delivery);

                    // The row write is visible but its caller cannot relay the result yet.
                    // The target should still unpublish and report steady authority.
                    await WaitForSteadyAsync(repository, context.ActorId, timeout);
                    Assert.False(reply.IsCompleted);
                    Assert.Equal(
                        row.Entry.ObjectKey,
                        ZLinkActorAuthorityPayloadCodec.AuthorityKey(context.ActorId).Value
                    );
                }
                finally
                {
                    capture.TrySetResult();
                    publication!.Release.TrySetResult();
                }

                Assert.Equal("saved", (await reply!.WaitAsync(timeout)).Marker);
                Assert.Equal(
                    ZLinkFrameworkRelocationOutcome.Relocated,
                    (await relocation.WaitAsync(timeout)).Outcome
                );
                Assert.Equal(1, trace.DeliveredMarkers.Count(marker => marker == "saved"));
            },
            (inner, actorId, _) =>
            {
                repository = new ZLinkProviderLocationRepository(inner);
                return publication = new CommitResponseGateStore(
                    inner,
                    actorId,
                    _ => true,
                    batch =>
                    {
                        var put = batch
                            .Mutations.OfType<ZLinkStoreMutation.Put>()
                            .SingleOrDefault(write =>
                                write.Key.Value.StartsWith(
                                    "$zlink/relocation-reply/",
                                    StringComparison.Ordinal
                                )
                                && batch
                                    .Conditions.OfType<ZLinkStoreCondition.Version>()
                                    .Any(c => c.Key == write.Key)
                            );
                        if (put is null)
                            return false;
                        var row = ZLinkRelocationReplyRow.Decode(put.Key, put.Bytes);
                        relocationId = row.RelocationId;
                        return row.Entry.Delivery == ZLinkRelocationCompletionDelivery.Pending;
                    }
                );
            }
        );
    }

    private static async Task WaitForSteadyAsync(
        ZLinkProviderLocationRepository repository,
        string actorId,
        TimeSpan timeout
    )
    {
        using var deadline = new CancellationTokenSource(timeout);
        var key = ZLinkActorAuthorityPayloadCodec.AuthorityKey(actorId);
        while (true)
        {
            if (
                await repository.ReadAuthorityAsync(key, deadline.Token)
                is ZLinkAuthorityReadResult.Found { Snapshot.Relocation: null }
            )
                return;
            await Task.Delay(TimeSpan.FromMilliseconds(5), deadline.Token);
        }
    }
}
