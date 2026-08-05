// Verifies SM-B2 Remote Actor Join behavior.
using SpotService.Client.Support;
using SpotService.Shared;
using Systems.Zlink.Stream.Connector.Contracts;
using Zlink.HttpClient;

namespace SpotService.Client.Scenarios;

internal static class SmB2RemoteActorJoinScenario
{
    public static async Task RunAsync(
        ZLinkHttpClient playB,
        ZLinkHttpClient sessionA,
        string sessionAStreamEndpoint)
    {
        var actorId = $"actor-sm-b2-remote-{Guid.NewGuid():N}";
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
        await client.Request(new AuthReq(actorId, "remote actor"))
            .PacketName("AuthReq")
            .Async<AuthRes>();
        var reply = await client.Request(new ActorPingReq("b2"))
            .PacketName("ActorPingReq")
            .Async<ActorPingRes>();

        ZlinkStreamAssert.Ensure(reply.ActorId == actorId, "SM-B2 actor reply mismatch.");
        ZlinkStreamAssert.Ensure(
            reply.NodeRid.StartsWith("play-b-", StringComparison.Ordinal),
            "SM-B2 remote node mismatch.");
        // Same split as SM-B1: the play host stamps its harness role name and
        // the session host stamps the Actor's mesh routing id.
        var created = $"entry-created|rid=play-b|actor={actorId}";
        var createdEvidence = (await playB.Post("/evidence/wait")
            .Body(new EvidenceWaitReq(new[] { created }))
            .Async<string[]>()).Body;
        ZlinkStreamAssert.Ensure(
            createdEvidence.Any(line => line.Contains(created, StringComparison.Ordinal)),
            "SM-B2 entry-created evidence mismatch.");

        var joined = $"entry-joined|rid={reply.NodeRid}|actor={actorId}";
        var joinedEvidence = (await sessionA.Post("/evidence/wait")
            .Body(new EvidenceWaitReq(new[] { joined }))
            .Async<string[]>()).Body;
        ZlinkStreamAssert.Ensure(
            joinedEvidence.Any(line => line.Contains(joined, StringComparison.Ordinal)),
            "SM-B2 entry-joined evidence mismatch.");
        Console.WriteLine("operation SpotService.sm-b2 passed");
    }
}
