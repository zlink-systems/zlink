// Verifies TA-B1 Missing Actor behavior.
using ToActorMessaging.Client.Support;

namespace ToActorMessaging.Client.Scenarios;

internal static class TaB1MissingActorScenario
{
    public static async Task RunAsync(ToActorScenarioContext context)
    {
        const string actorId = "missing-actor";
        await context.AssertRouteAbsentAsync(actorId);
        //  The send's submit result is not the verification here (config 9
        //  TA-B1). What must hold is that no handler ran and no authority was
        //  created, which the evidence and route checks below assert.
        await context.SendWithoutOutcomeAssertionAsync(
            "TA-B1-missing",
            actorId,
            "missing",
            targetNodeRid: "actor-a",
            targetGeneration: 1);
        await context.AssertFailureAsync(
            "TA-B1-missing-request",
            actorId,
            "NotFound",
            send: false,
            targetNodeRid: "actor-a",
            targetGeneration: 1);
        await context.AssertNoActorEvidenceAsync(actorId);
        await context.AssertRouteAbsentAsync(actorId);
    }
}
