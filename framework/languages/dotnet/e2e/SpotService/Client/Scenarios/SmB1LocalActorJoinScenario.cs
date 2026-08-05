// Verifies SM-B1 Local Actor Join behavior.
using SpotService.Client.Support;
using SpotService.Shared;
using Systems.Zlink.Stream.Connector.Contracts;
using Zlink.HttpClient;

namespace SpotService.Client.Scenarios;

internal static class SmB1LocalActorJoinScenario
{
    public static async Task RunAsync(
        ZLinkHttpClient playA,
        ZLinkHttpClient sessionA,
        string sessionAStreamEndpoint)
    {
        var actorId = $"actor-sm-b1-local-{Guid.NewGuid():N}";
        ZlinkStreamAssert.Ensure(!string.IsNullOrWhiteSpace(sessionAStreamEndpoint),
            "session-a stream endpoint is required.");
        await using var client = ZlinkStreamConnectorFactory.Create(new ZlinkStreamConnectorOptions
        {
            Endpoint = new Uri(sessionAStreamEndpoint),
            ConnectTimeout = TimeSpan.FromSeconds(5),
            RequestTimeout = TimeSpan.FromSeconds(5),
            Heartbeat = new ZlinkStreamHeartbeatOptions { Enabled = false },
            DispatchMode = ZlinkStreamDispatchMode.Immediate,
            MaxReceivedMessages = 1024
        });
        await client.Connect.Async();
        await client.Request(new AuthReq(actorId, "local actor"))
            .PacketName("AuthReq")
            .Async<AuthRes>();
        var ping = await client.Request(new ActorPingReq("b1"))
            .PacketName("ActorPingReq")
            .Async<ActorPingRes>();
        ZlinkStreamAssert.Ensure(ping.ActorId == actorId, "SM-B1 actor reply mismatch.");
        ZlinkStreamAssert.Ensure(
            ping.NodeRid.StartsWith("play-a-", StringComparison.Ordinal),
            "SM-B1 local node mismatch.");
        // The two markers come from different hosts and mean different things
        // by "rid": the play host stamps its harness role name, while the
        // session host stamps the Actor's mesh routing id - now an automatic
        // 'play-a-<uuid>', so the role name only prefixes it.
        var created = $"entry-created|rid=play-a|actor={actorId}";
        var createdEvidence = (await playA.Post("/evidence/wait")
            .Body(new EvidenceWaitReq(new[] { created }))
            .Async<string[]>()).Body;
        ZlinkStreamAssert.Ensure(
            createdEvidence.Any(line => line.Contains(created, StringComparison.Ordinal)),
            "SM-B1 entry-created evidence mismatch.");

        var joined = $"entry-joined|rid={ping.NodeRid}|actor={actorId}";
        var joinedEvidence = (await sessionA.Post("/evidence/wait")
            .Body(new EvidenceWaitReq(new[] { joined }))
            .Async<string[]>()).Body;
        ZlinkStreamAssert.Ensure(
            joinedEvidence.Any(line => line.Contains(joined, StringComparison.Ordinal)),
            "SM-B1 entry-joined evidence mismatch.");
        Console.WriteLine("operation SpotService.sm-b1 passed");
    }
}
