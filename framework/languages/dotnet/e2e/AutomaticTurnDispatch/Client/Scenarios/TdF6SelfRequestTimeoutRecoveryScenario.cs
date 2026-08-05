// Verifies TD-F6 self-request rejection and recovery behavior.
using AutomaticTurnDispatch.Shared;

namespace AutomaticTurnDispatch.Client.Scenarios;
internal static class TdF6SelfRequestTimeoutRecoveryScenario
{
    public static async Task RunAsync(ExecutionTurnScenarioContext context)
    {
        var spot = await context.SpotAsync();
        var requestId = ExecutionTurnScenarioContext.NewId("TD-F6");
        await context.SendSpotAsync(new SelfCycleMsg(requestId, 150), spot);
        var evidence = await context.EvidenceAsync(requestId, "self-cycle-rejected");
        ZlinkStreamAssert.Ensure(
            evidence.Any(line => line.Contains($"request={requestId}", StringComparison.Ordinal)
                                 && line.Contains("terminator=async", StringComparison.Ordinal)),
            "TD-F6 self-request was not rejected by same-gate validation.");
        ZlinkStreamAssert.Ensure(
            !evidence.Any(line => line.Contains($"request={requestId}", StringComparison.Ordinal)
                                  && line.Contains("self-cycle-unexpected-completed", StringComparison.Ordinal)),
            "TD-F6 self-request reached an unexpected completion.");
        var reply = await context.SpotRequest(spot, new ProbeReq(requestId, "post-cycle"))
            .Async<AutomaticTurnDispatchRes>();
        ZlinkStreamAssert.Ensure(reply.Marker == "post-cycle", "TD-F6 Spot did not recover after rejection.");
    }
}
