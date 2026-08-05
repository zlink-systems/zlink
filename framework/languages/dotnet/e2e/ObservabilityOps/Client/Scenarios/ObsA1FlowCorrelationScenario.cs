// Verifies OBS-A1 Flow Correlation behavior.
using ObservabilityOps.Client.Support;
using ObservabilityOps.Shared;
using Systems.Zlink.Stream.Connector.Contracts;

namespace ObservabilityOps.Client.Scenarios;

internal static class ObsA1FlowCorrelationScenario
{
    public static async Task RunAsync(ScenarioContext context)
    {
        var suffix = Guid.NewGuid().ToString("N");
        var actorId = $"obs-a1-{suffix}";
        var roomRid = $"room-a1-{suffix}";
        var room = (await context.PlayA.Post("/rooms")
            .Body(new CreateRoomReq(roomRid))
            .Async<CreateRoomRes>()).Body;
        await using var connector = await context.ConnectAsync();
        var authenticated = await connector.Request(new AuthenticateReq(actorId)).Async<AuthenticateRes>();
        ZlinkStreamAssert.Ensure(authenticated.ActorId == actorId, "OBS-A1 authentication actor mismatch.");
        var joined = await context.JoinRoomAsync(connector, actorId, roomRid);
        ZlinkStreamAssert.Ensure(joined.SpotId == roomRid, "OBS-A1 room join mismatch.");
        var action = await connector.Request(new GameActionReq("obs-a1-action")).Async<GameActionRes>();
        ZlinkStreamAssert.Ensure(action.ActorId == actorId && action.RoomRid == roomRid,
            "OBS-A1 action did not traverse the bound actor and room Spot.");
        var evidence = await context.WaitPlayEvidenceForNodeAsync(
            room.NodeRid, $"game-action|actor={actorId}", "marker=obs-a1-action");
        ZlinkStreamAssert.Ensure(evidence.Any(line => line.Contains("game-action|", StringComparison.Ordinal)),
            "OBS-A1 room action evidence missing.");
        _ = context.RequireSharedFlow(
            nameof(GameActionReq), "session-a", await context.PlayRoleForNodeAsync(room.NodeRid));
        await connector.Close.Async();
        Console.WriteLine("scenario OBS-A1 passed");
    }
}
