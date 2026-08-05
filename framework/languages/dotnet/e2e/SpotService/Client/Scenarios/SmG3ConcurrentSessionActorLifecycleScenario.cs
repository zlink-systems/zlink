// Verifies SM-G3 Concurrent Session Actor Lifecycle behavior.
using SpotService.Client.Support;
using SpotService.Shared;
using Systems.Zlink.Stream.Connector.Contracts;
using Zlink.HttpClient;

namespace SpotService.Client.Scenarios;

// Verifies concurrent actor join, request, and leave through real stream clients.
internal static class SmG3ConcurrentSessionActorLifecycleScenario
{
    public static async Task RunAsync(ZLinkHttpClient playA, string sessionAStreamEndpoint)
    {
        const int actorCount = 2;
        var key = Guid.NewGuid().ToString("N");
        var spotRid = $"spot-sm-g3-{key}";
        var actorIds = Enumerable.Range(0, actorCount)
            .Select(index => $"actor-sm-g3-{key}-{index}")
            .ToArray();
        var clients = new List<IZlinkStreamConnector>();
        try
        {
            foreach (var actorId in actorIds)
            {
                var client = ZlinkStreamConnectorFactory.Create(new ZlinkStreamConnectorOptions
                {
                    Endpoint = new Uri(sessionAStreamEndpoint)
                });
                await client.Connect.Async();
                await client.Request(new UserSpotAuthReq(spotRid, actorId, actorId))
                    .PacketName("UserSpotAuthReq").Async<AuthRes>();
                await playA.Post("/spot/create").Body(new CreateSpotReq(spotRid)).Async<CreateSpotRes>();
                await client.Request(new JoinUserSpotActorReq(spotRid, actorId))
                    .PacketName("JoinUserSpotActorReq").Async<JoinUserSpotActorRes>();

                clients.Add(client);
            }

            await Task.WhenAll(actorIds.Select(async (actorId, index) =>
            {
                var client = clients[index];
                var ping = await client.Request(new ActorPingReq(actorId))
                    .PacketName("UserActorPingReq")
                    .Async<ActorPingRes>();
                ZlinkStreamAssert.Ensure(ping.ActorId == actorId, "SM-G3 actor request target mismatch.");
                ZlinkStreamAssert.Ensure(ping.NodeRid == "play-a", "SM-G3 actor request reached the wrong node.");
                var left = await client.Request(new LeaveReq(actorId))
                    .PacketName("LeaveReq")
                    .Async<LeaveRes>();
                ZlinkStreamAssert.Ensure(left.Accepted && left.ActorId == actorId, "SM-G3 leave reply mismatch.");
            }));

            var expectedEvidence = actorIds
                .SelectMany(actorId => new[]
                {
                    $"spot-actor-joined|rid=play-a|spot={spotRid}|actor={actorId}",
                    $"spot-actor-left|rid=play-a|spot={spotRid}|actor={actorId}"
                })
                .ToArray();
            var evidence = (await playA.Post("/evidence/wait")
                .Body(new EvidenceWaitReq(expectedEvidence))
                .Async<string[]>()).Body;
            ZlinkStreamAssert.Ensure(
                expectedEvidence.All(expected =>
                    evidence.Any(line => line.Contains(expected, StringComparison.Ordinal))),
                "SM-G3 expected concurrent join and leave evidence.");
            foreach (var actorId in actorIds)
            {
                ZlinkStreamAssert.Ensure(
                    evidence.Any(line => line.Contains(
                        $"spot-actor-joined|rid=play-a|spot={spotRid}|actor={actorId}",
                        StringComparison.Ordinal)),
                    $"SM-G3 join evidence count mismatch for {actorId}.");
                ZlinkStreamAssert.Ensure(
                    evidence.Any(line => line.Contains(
                        $"spot-actor-left|rid=play-a|spot={spotRid}|actor={actorId}",
                        StringComparison.Ordinal)),
                    $"SM-G3 leave evidence count mismatch for {actorId}.");
            }
        }
        finally
        {
            foreach (var client in clients) await client.DisposeAsync();
        }

        Console.WriteLine("operation SpotService.sm-g3 passed");
    }
}
