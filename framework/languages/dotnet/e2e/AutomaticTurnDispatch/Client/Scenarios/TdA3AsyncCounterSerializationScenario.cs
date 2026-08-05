// Verifies TD-A3 Async Counter Serialization behavior.
using AutomaticTurnDispatch.Shared;

namespace AutomaticTurnDispatch.Client.Scenarios;
internal static class TdA3AsyncCounterSerializationScenario
{
    public static async Task RunAsync(ExecutionTurnScenarioContext context)
    {
        var spot = await context.SpotAsync();
        var requestId = ExecutionTurnScenarioContext.NewId("TD-A3");
        await context.SendSpotAsync(new CounterResetMsg(requestId), spot);
        await context.EvidenceAsync(requestId, "counter-reset");
        for (var index = 0; index < 8; index++)
            await context.SendSpotAsync(new CounterAwaitMsg(requestId, $"op-{index}", 200, "async"), spot);
        await context.EvidenceAsync(requestId, "counter-async-completed", minimumCount: 8);
        var counter = await context.SpotRequest(spot, new CounterReadReq(requestId)).Async<CounterReadRes>();
        ZlinkStreamAssert.Ensure(counter.Value == 8, $"TD-A3 expected counter 8, actual {counter.Value}.");
    }
}
