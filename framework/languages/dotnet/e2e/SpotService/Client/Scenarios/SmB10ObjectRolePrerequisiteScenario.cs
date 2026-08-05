// Verifies SM-B10 object roles require a Location Store while Node/Channel does not.
using SpotService.Client.Support;
using SpotService.Shared;
using Zlink.HttpClient;

namespace SpotService.Client.Scenarios;

internal static class SmB10ObjectRolePrerequisiteScenario
{
    public static async Task RunAsync(
        ZLinkHttpClient playA,
        string? controlEndpoint,
        string? controlRid)
    {
        if (string.IsNullOrWhiteSpace(controlEndpoint)
            || string.IsNullOrWhiteSpace(controlRid))
            throw new InvalidOperationException(
                "SM-B10 requires the manual RouteMesh endpoint and routing id.");

        var status = (await playA.Get("/b10/manual/status")
            .Async<B10ManualStatusRes>()).Body;
        ZlinkStreamAssert.Ensure(
            !status.ActorManagerProvided
            && !status.SpotManagerProvided
            && !status.ObjectOperationsProvided,
            "SM-B10 role-None manual host exposed object operations.");

        var reply = await B10ManualRouteClient.RequestAsync(
            controlEndpoint,
            controlRid,
            new ControlPingReq("sm-b10-manual-channel"));
        ZlinkStreamAssert.Ensure(
            reply is { Value: "sm-b10-manual-channel", NodeRid: "b10-manual" },
            "SM-B10 role-None manual Channel request did not complete.");

        Console.WriteLine("operation SpotService.sm-b10 passed");
    }
}
