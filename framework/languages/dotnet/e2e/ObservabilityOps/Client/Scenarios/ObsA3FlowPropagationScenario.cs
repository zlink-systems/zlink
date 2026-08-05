// Verifies OBS-A3 Flow Propagation behavior.
using ObservabilityOps.Client.Support;
using ObservabilityOps.Shared;
using Systems.Zlink.Stream.Connector.Contracts;

namespace ObservabilityOps.Client.Scenarios;

internal static class ObsA3FlowPropagationScenario
{
    public static async Task RunAsync(ScenarioContext context)
    {
        var suffix = Guid.NewGuid().ToString("N");
        var actorId = $"obs-a3-{suffix}";
        var roomRid = $"room-a3-{suffix}";
        var room = (await context.PlayA.Post("/rooms")
            .Body(new CreateRoomReq(roomRid))
            .Async<CreateRoomRes>()).Body;
        var sessionActionLinesBefore = context.ReadFlowLines("session-a")
            .Count(line => line.Contains("packet=GameActionReq", StringComparison.Ordinal));
        await context.Session.Post("/diagnostics/level")
            .Query("level", "off").AsyncRaw();
        await using var connector = await context.ConnectAsync();
        await connector.Request(new AuthenticateReq(actorId)).Async<AuthenticateRes>();
        await context.JoinRoomAsync(connector, actorId, roomRid);
        var response = await connector.Request(new GameActionReq("obs-a3-off-hop")).Async<GameActionRes>();
        ZlinkStreamAssert.Ensure(response.Marker == "obs-a3-off-hop", "OBS-A3 action failed after the off hop.");
        var playLines = context.ReadFlowLines(await context.PlayRoleForNodeAsync(room.NodeRid));
        var sessionLines = context.ReadFlowLines("session-a");
        ZlinkStreamAssert.Ensure(playLines.Any(line => line.Contains("packet=GameActionReq", StringComparison.Ordinal)
                                                     && line.Contains("flow=", StringComparison.Ordinal)),
            "OBS-A3 downstream Play flow was not preserved.");
        ZlinkStreamAssert.Ensure(sessionLines.Count(line =>
                line.Contains("packet=GameActionReq", StringComparison.Ordinal)) == sessionActionLinesBefore,
            "OBS-A3 tracing-off Session unexpectedly wrote a new flow line.");
        await connector.Close.Async();
        Console.WriteLine("scenario OBS-A3 passed");
    }
}
