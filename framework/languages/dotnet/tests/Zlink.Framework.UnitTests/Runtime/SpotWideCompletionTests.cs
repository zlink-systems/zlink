using System.Text.Json;
using Microsoft.Extensions.DependencyInjection;
using Zlink.Framework.Runtime.Locations;

namespace Zlink.Framework.UnitTests.Runtime;

public sealed partial class HostRelocationSealStageTests
{
    [Fact]
    public async Task SpotWide_member_reply_waits_for_published_completion()
    {
        var capture = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        var trace = new RelocationBehaviorTrace { SourceActorCaptureGate = capture.Task };
        CommitResponseGateStore? publication = null;
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
                    var committed = publication!.Committed.Task;
                    Assert.Same(committed, await Task.WhenAny(committed, reply).WaitAsync(timeout));
                    var published = await committed;
                    Assert.False(reply.IsCompleted);
                    var progress = CompletionJson(published);
                    Assert.Equal("1", progress.GetProperty("acceptedRequests").GetString());
                    Assert.Equal("1", progress.GetProperty("completedResults").GetString());
                    Assert.Equal("1", progress.GetProperty("pendingDeliveries").GetString());
                    Assert.False(
                        string.IsNullOrWhiteSpace(progress.GetProperty("reference").GetString())
                    );
                    var blocked = await context
                        .Target.Services.GetRequiredService<IZLinkFrameworkRuntime>()
                        .RelocateAsync(
                            new ZLinkFrameworkRelocationOptions
                            {
                                Mode = ZLinkFrameworkRelocationMode.PlannedMaintenance,
                                Deadline = timeout,
                            }
                        );
                    Assert.Equal(ZLinkFrameworkRelocationOutcome.Blocked, blocked.Outcome);
                    Assert.Equal(
                        ZLinkFrameworkRelocationReason.OperationInProgress,
                        blocked.Reason
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
                publication = new CommitResponseGateStore(
                    inner,
                    actorId,
                    snapshot =>
                        CompletionJson(snapshot) is { ValueKind: JsonValueKind.Object } progress
                        && progress.GetProperty("pendingDeliveries").GetString() == "1"
                )
        );
    }

    private static JsonElement CompletionJson(ZLinkAuthoritySnapshot snapshot) =>
        ZLinkAuthorityAggregateCodec.Replace(null, snapshot.Relocation) is { } aggregate
        && aggregate.GetProperty("relocation").TryGetProperty("completion", out var completion)
            ? completion
            : default;
}
