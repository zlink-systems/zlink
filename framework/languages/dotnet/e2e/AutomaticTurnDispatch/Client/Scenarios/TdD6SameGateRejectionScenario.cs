// Verifies same-gate awaited requests fail before target dispatch.
using AutomaticTurnDispatch.Client.Support;
using AutomaticTurnDispatch.Shared;

namespace AutomaticTurnDispatch.Client.Scenarios;

internal static class TdD6SameGateRejectionScenario
{
    public static async Task RunAsync(ExecutionTurnScenarioContext context)
    {
        var spot = await context.SpotAsync();
        foreach (var terminator in new[] { "async", "yield" })
        {
            var requestId = ExecutionTurnScenarioContext.NewId($"TD-D6-{terminator}");
            await context.SendSpotAsync(
                new SelfCycleMsg(requestId, 1000, terminator),
                spot);
            var evidence = await context.EvidenceAsync(requestId, "self-cycle-rejected");
            ZlinkStreamAssert.Ensure(
                evidence.Any(line => line.Contains($"request={requestId}", StringComparison.Ordinal)
                                     && line.Contains($"terminator={terminator}", StringComparison.Ordinal)),
                $"TD-D6 {terminator} request was not rejected by same-gate validation.");
            ZlinkStreamAssert.Ensure(
                !evidence.Any(line => line.Contains($"request={requestId}", StringComparison.Ordinal)
                                      && line.Contains("self-cycle-unexpected-completed", StringComparison.Ordinal)),
                $"TD-D6 {terminator} request reached an unexpected completion.");
        }

        var sendRequestId = ExecutionTurnScenarioContext.NewId("TD-D6-send");
        await context.SendSpotAsync(new SelfSendMsg(sendRequestId, "self-send"), spot);
        var sendEvidence = await context.EvidenceAsync(sendRequestId, "probe-completed");
        EvidenceOrder.ContainsExactRequestInOrder(
            sendEvidence,
            sendRequestId,
            ["self-send-started", "self-send-completed", "probe-completed"]);
    }
}
