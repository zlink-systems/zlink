// Verifies that failed handlers discard all deferred Actor Join intents.
using AutomaticTurnDispatch.Shared;

namespace AutomaticTurnDispatch.Client.Scenarios;

internal static class TdE2ADeferredJoinFailureScenario
{
    public static async Task RunAsync(ExecutionTurnScenarioContext context)
    {
        var actors = await context.ActorsAsync();
        await context.EnsureActorInSpotAsync(actors.ActorA, actors.SpotRid, "TD-E2A-prepare-a");
        await context.EnsureActorInSpotAsync(actors.ActorB, actors.SpotRid, "TD-E2A-prepare-b");

        foreach (var mode in new[] { "exception", "cancel" })
        {
            var requestId = ExecutionTurnScenarioContext.NewId($"TD-E2A-{mode}");
            var targetA = $"td-e2a-{mode}-a-{Guid.NewGuid():N}";
            var targetB = $"td-e2a-{mode}-b-{Guid.NewGuid():N}";
            await context.EnsureSpotAsync(targetA, "play-a");
            await context.EnsureSpotAsync(targetB, "play-a");
            await context.SendSpotAsync(
                new DeferredJoinFailureMsg(
                    requestId,
                    actors.ActorA,
                    actors.ActorB,
                    targetA,
                    targetB,
                    mode),
                actors.SpotRid);
            await context.EvidenceAsync(requestId, "deferred-join-failure-registered");

            // A source-spot request after the failing handler proves that the
            // source mailbox continued processing after deferred intents were discarded.
            var probe = await context.ActorRequest(
                    actors.ActorA,
                    new ActorFastReq(requestId, $"{mode}-source-still-member"))
                .Async<ActorAwaitRes>();
            ZlinkStreamAssert.Ensure(
                probe.Marker == $"{mode}-source-still-member",
                $"TD-E2A {mode} source Actor did not remain usable.");
            var evidence = await context.EvidenceAsync(requestId, "actor-fast-completed");
            ZlinkStreamAssert.Ensure(
                !evidence.Any(line => line.Contains($"request={requestId}", StringComparison.Ordinal)
                                      && (line.Contains("td-e2a-first-completed", StringComparison.Ordinal)
                                          || line.Contains("td-e2a-second-completed", StringComparison.Ordinal))),
                $"TD-E2A {mode} unexpectedly started a deferred Join.");
        }
    }
}
