// Verifies SM-B6 Actor Disconnect Callback behavior.
using SpotService.Client.Support;
using SpotService.Shared;
using Systems.Zlink.Stream.Connector.Contracts;
using Zlink.HttpClient;

namespace SpotService.Client.Scenarios;

internal static class SmB6ActorDisconnectCallbackScenario
{
    public static async Task RunAsync(
        ZLinkHttpClient playA,
        ZLinkHttpClient playB,
        ZLinkHttpClient sessionA,
        string sessionAStreamEndpoint)
    {
        // The common e2e document verifies this on "the Actor's owning node",
        // not a named one, and placement decides which play node holds the
        // Spot. The create reply carries that node, so the evidence queries
        // follow it.
        ZLinkHttpClient owner = playA;
        var ownerRid = "play-a";
        var spotRid = $"spot-sm-b6-{Guid.NewGuid():N}";
        var leaveActorId = $"actor-sm-b6-left-{Guid.NewGuid():N}";
        var disconnectActorId = $"actor-sm-b6-disconnected-{Guid.NewGuid():N}";

        await using (var client = ZlinkStreamConnectorFactory.Create(new ZlinkStreamConnectorOptions
        {
            Endpoint = new Uri(sessionAStreamEndpoint),
            ConnectTimeout = TimeSpan.FromSeconds(5),
            RequestTimeout = TimeSpan.FromSeconds(5),
            Heartbeat = new ZlinkStreamHeartbeatOptions { Enabled = false },
            DispatchMode = ZlinkStreamDispatchMode.Immediate,
            MaxReceivedMessages = 1024
        }))
        {
            await client.Connect.Async();
        await client.Request(new UserSpotAuthReq(spotRid, leaveActorId, leaveActorId))
                .PacketName("UserSpotAuthReq")
                .Async<AuthRes>();
            var createdSpot = (await playA.Post("/spot/create")
                .Body(new CreateSpotReq(spotRid))
                .Async<CreateSpotRes>()).Body;
            owner = createdSpot.NodeRid.StartsWith("play-a-", StringComparison.Ordinal)
                ? playA
                : playB;
            ownerRid = createdSpot.NodeRid.StartsWith("play-a-", StringComparison.Ordinal)
                ? "play-a"
                : "play-b";
            await client.Request(new JoinUserSpotActorReq(spotRid, leaveActorId))
                .PacketName("JoinUserSpotActorReq")
                .Async<JoinUserSpotActorRes>();
            var left = await client.Request(new LeaveReq(leaveActorId))
                .PacketName("LeaveReq")
                .Async<LeaveRes>();

            ZlinkStreamAssert.Ensure(left.Accepted && left.ActorId == leaveActorId, "SM-B6 leave reply mismatch.");
        }
        var expectedLeaveEvidence = new[] { $"spot-actor-left|rid={ownerRid}|spot={spotRid}|actor={leaveActorId}" };
        var playAAfterLeave = (await owner.Post("/evidence/wait")
            .Body(new EvidenceWaitReq(expectedLeaveEvidence))
            .Async<string[]>()).Body;
        ZlinkStreamAssert.Ensure(
            expectedLeaveEvidence.All(expected =>
                playAAfterLeave.Any(line => line.Contains(expected, StringComparison.Ordinal))),
            "SM-B6 expected explicit leave evidence.");
        ZlinkStreamAssert.Ensure(
            playAAfterLeave.Any(line => line.Contains(
                $"spot-actor-left|rid={ownerRid}|spot={spotRid}|actor={leaveActorId}",
                StringComparison.Ordinal)),
            "SM-B6 expected explicit leave evidence.");
        ZlinkStreamAssert.Ensure(
            playAAfterLeave.All(line => !line.Contains(
                $"spot-actor-disconnected|rid={ownerRid}|spot={spotRid}|actor={leaveActorId}",
                StringComparison.Ordinal)),
            "SM-B6 explicit leave incorrectly emitted disconnect evidence.");

        // Case (b) of the document is an abnormal termination of the bound
        // connection. The connector exposes only a graceful Close, so the
        // transport is cut underneath it with a proxy.
        await using var fault = new ReconnectProxy(new Uri(sessionAStreamEndpoint));
        await using (var disconnectClient = ZlinkStreamConnectorFactory.Create(new ZlinkStreamConnectorOptions
        {
            Endpoint = fault.Endpoint,
            ConnectTimeout = TimeSpan.FromSeconds(5),
            RequestTimeout = TimeSpan.FromSeconds(5),
            Heartbeat = new ZlinkStreamHeartbeatOptions { Enabled = false },
            DispatchMode = ZlinkStreamDispatchMode.Immediate,
            MaxReceivedMessages = 1024
        }))
        {
            await disconnectClient.Connect.Async();
        await disconnectClient.Request(new UserSpotAuthReq(spotRid, disconnectActorId, disconnectActorId))
                .PacketName("UserSpotAuthReq")
                .Async<AuthRes>();
            await owner.Post("/spot/create")
                .Body(new CreateSpotReq(spotRid))
                .Async<CreateSpotRes>();
            await disconnectClient.Request(new JoinUserSpotActorReq(spotRid, disconnectActorId))
                .PacketName("JoinUserSpotActorReq")
                .Async<JoinUserSpotActorRes>();
            // The join is deferred: the reply only acknowledges the intent. The
            // document verifies disconnect for an Actor that has joined, and a
            // cross-node join keeps running past the Spot's own record - the
            // handoff still has to commit the session route. Waiting for the
            // Actor's join completion covers both.
            await owner.Post("/evidence/wait")
                .Body(new EvidenceWaitReq(
                    [$"actor-join-completed|rid={ownerRid}|actor={disconnectActorId}"]))
                .Async<string[]>();
            await fault.WaitForConnectionAsync();
            fault.DropConnection();
        }

        var expectedDisconnectEvidence =
            $"spot-actor-disconnected|rid={ownerRid}|spot={spotRid}|actor={disconnectActorId}";
        var playAAfterDisconnect = (await owner.Post("/evidence/wait")
            .Body(new EvidenceWaitReq([expectedDisconnectEvidence]))
            .Async<string[]>()).Body;
        ZlinkStreamAssert.Ensure(
            playAAfterDisconnect.Any(line => line.Contains(expectedDisconnectEvidence, StringComparison.Ordinal)),
            "SM-B6 expected disconnect evidence.");
        ZlinkStreamAssert.Ensure(
            playAAfterDisconnect.All(line => !line.Contains(
                $"spot-actor-left|rid={ownerRid}|spot={spotRid}|actor={disconnectActorId}",
                StringComparison.Ordinal)),
            "SM-B6 disconnect incorrectly emitted leave evidence.");

        Console.WriteLine("operation SpotService.sm-b6 passed");
    }
}
