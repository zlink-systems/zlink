// Verifies that Yield is rejected outside an allowed application callback.
using AutomaticTurnDispatch.Shared;
using Systems.Zlink.Stream.Connector.Contracts;

namespace AutomaticTurnDispatch.Client.Scenarios;

internal static class TdD5UnsupportedYieldScenario
{
    public static async Task RunAsync(ExecutionTurnScenarioContext context)
    {
        var actors = await context.ActorsAsync();
        var requestId = ExecutionTurnScenarioContext.NewId("TD-D5");
        var failure = await ZlinkStreamAssert.ExpectFailureAsync(
            async cancellationToken => _ = await context.ActorRequest(
                    actors.ActorA,
                    new ActorAwaitReq(requestId, 100, "yield"))
                .Async<ActorAwaitRes>(cancellationToken),
            nameof(ZlinkStreamErrorCode.RemoteError));
        ZlinkStreamAssert.Ensure(
            failure.Message.Contains("Yield", StringComparison.OrdinalIgnoreCase),
            "TD-D5 did not report the unsupported Yield contract.");

        var asyncRequest = ExecutionTurnScenarioContext.NewId("TD-D5-async");
        var asyncReply = await context.ActorRequest(
                actors.ActorA,
                new ActorAwaitReq(asyncRequest, 20, "async"))
            .Async<ActorAwaitRes>();
        ZlinkStreamAssert.Ensure(
            asyncReply.Marker == "actor-await-completed",
            "TD-D5 Async contrast did not complete.");
    }
}
