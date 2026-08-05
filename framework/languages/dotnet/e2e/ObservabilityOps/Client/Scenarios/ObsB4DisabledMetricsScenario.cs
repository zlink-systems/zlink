// Verifies OBS-B4 Disabled Metrics behavior.
using ObservabilityOps.Client.Support;
using ObservabilityOps.Shared;
using Systems.Zlink.Stream.Connector.Contracts;

namespace ObservabilityOps.Client.Scenarios;

internal static class ObsB4DisabledMetricsScenario
{
    private const int TrafficCount = 80;

    public static async Task RunAsync(ScenarioContext context)
    {
        var playBNode = await context.PlayNodeIdAsync("play-b");
        var suffix = Guid.NewGuid().ToString("N");
        var actorId = $"obs-b4-{suffix}";
        string roomRid;
        // B4 follows B3's 11s store pause on the same topology: the first
        // store write can still sit behind the multiplexer's recovery, so the
        // room creation polls within a bounded window instead of racing it.
        var roomDeadline = DateTimeOffset.UtcNow + TimeSpan.FromSeconds(20);
        while (true)
        {
            try
            {
                var room = await context.CreateRoomOnObservedNodeAsync(
                    playBNode, $"room-b4-{suffix}");
                roomRid = room.RoomRid;
                break;
            }
            catch (Exception) when (DateTimeOffset.UtcNow < roomDeadline)
            {
                await Task.Delay(500);
            }
        }
        await using var connector = await context.ConnectAsync();
        //  Room 생성과 같은 이유로 여기도 창을 둔다. B3의 store 정지 동안 owner
        //  lease 갱신이 실패하면 owner admission이 닫히고, 복구 뒤 첫 갱신이
        //  성공해야 다시 열린다. 그 사이에 들어온 호출은
        //  "not accepting operations (owner admission is closed)"로 끝난다.
        //  위 room 생성만 기다리고 이 호출은 기다리지 않던 것이 비대칭이었다.
        var authenticateDeadline = DateTimeOffset.UtcNow + TimeSpan.FromSeconds(20);
        while (true)
        {
            try
            {
                await connector.Request(new AuthenticateReq(actorId)).Async<AuthenticateRes>();
                break;
            }
            catch (Exception) when (DateTimeOffset.UtcNow < authenticateDeadline)
            {
                await Task.Delay(500);
            }
        }
        var joined = await context.JoinRoomAsync(connector, actorId, roomRid);
        ZlinkStreamAssert.Ensure(
            joined.NodeRid == playBNode,
            "OBS-B4 actor join did not publish its target owner.");
        for (var index = 0; index < TrafficCount; index++)
        {
            var marker = $"obs-b4-{index}";
            var action = await ScenarioContext.RequestWithRebindRetryAsync(
                () => connector.Request(new GameActionReq(marker)).Async<GameActionRes>().AsTask());
            ZlinkStreamAssert.Ensure(
                action.Marker == marker && action.NodeRid == joined.NodeRid,
                "OBS-B4 messaging changed while metrics were disabled.");
        }
        var evidence = (await context.PlayB.Get("/evidence").Async<EvidenceSnapshot>()).Body;
        ZlinkStreamAssert.Ensure(evidence.Metrics.Length == 0,
            "OBS-B4 node without a reader retained metric samples.");
        ZlinkStreamAssert.Ensure(evidence.Entries.Count(entry => entry.Contains("game-action|", StringComparison.Ordinal))
                                >= TrafficCount,
            "OBS-B4 disabled metrics changed message processing.");
        await connector.Close.Async();
        Console.WriteLine("scenario OBS-B4 passed");
    }
}
