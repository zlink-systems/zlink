// Verifies TD-B3 Yield Lost Update behavior.
using AutomaticTurnDispatch.Shared;

namespace AutomaticTurnDispatch.Client.Scenarios;
internal static class TdB3YieldLostUpdateScenario
{
    public static async Task RunAsync(ExecutionTurnScenarioContext context)
    {
        var spot = await context.SpotAsync();
        var requestId = ExecutionTurnScenarioContext.NewId("TD-B3");
        await context.SendSpotAsync(new CounterResetMsg(requestId), spot);
        await context.EvidenceAsync(requestId, "counter-reset");
        for (var index = 0; index < 8; index++)
            await context.SendSpotAsync(new CounterAwaitMsg(requestId, $"op-{index}", 200, "yield"), spot);
        await context.EvidenceAsync(requestId, "counter-yield-completed", minimumCount: 8);
        var counter = await context.SpotRequest(spot, new CounterReadReq(requestId)).Async<CounterReadRes>();
        ZlinkStreamAssert.Ensure(counter.Value == 1, $"TD-B3 expected counter 1, actual {counter.Value}.");
    }
}
