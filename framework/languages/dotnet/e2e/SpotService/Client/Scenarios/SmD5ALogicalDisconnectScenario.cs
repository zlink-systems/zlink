// Verifies SM-D5A application logical disconnect behavior.
using SpotService.Client.Support;
using SpotService.Shared;
using Systems.Zlink.Stream.Connector.Contracts;
using Zlink.HttpClient;

namespace SpotService.Client.Scenarios;

internal static class SmD5ALogicalDisconnectScenario
{
    public static async Task RunAsync(
        ZLinkHttpClient playA,
        string sessionAStreamEndpoint)
    {
        var selectedActorId = $"actor-sm-d5a-selected-{Guid.NewGuid():N}";
        var otherActorId = $"actor-sm-d5a-other-{Guid.NewGuid():N}";
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
        var bound = await client
            .Request(new MultiBindReq(selectedActorId, otherActorId))
            .PacketName("MultiBindReq")
            .Async<MultiBindRes>();
        ZlinkStreamAssert.Ensure(bound.BoundCount == 2, "SM-D5A expected two bound actors.");

        var notified = await client
            .Request(new NotifyBoundActorDisconnectedReq(selectedActorId))
            .PacketName("NotifyBoundActorDisconnectedReq")
            .Async<NotifyBoundActorDisconnectedRes>();
        ZlinkStreamAssert.Ensure(
            notified.Completed && notified.ActorId == selectedActorId,
            "SM-D5A logical notification did not wait for the selected Actor callback.");

        var otherReply = await client.Request(new ActorPingReq("still-connected"))
            .PacketName("ActorPingReq")
            .Metadata(SpotServiceNames.ActorIdMetadata, otherActorId)
            .Async<ActorPingRes>();
        ZlinkStreamAssert.Ensure(
            otherReply.ActorId == otherActorId && otherReply.Value == "still-connected",
            "SM-D5A changed the physical connection or the other Actor binding.");

        var selectedEvidence =
            $"entry-disconnected|rid=play-a|actor={selectedActorId}";
        var otherEvidence =
            $"entry-disconnected|rid=play-a|actor={otherActorId}";
        var evidence = (await playA.Post("/evidence/wait")
            .Body(new EvidenceWaitReq([selectedEvidence]))
            .Async<string[]>()).Body;
        ZlinkStreamAssert.Ensure(
            evidence.Count(line => line.Contains(selectedEvidence, StringComparison.Ordinal)) == 1,
            "SM-D5A expected exactly one callback for the selected Actor.");
        ZlinkStreamAssert.Ensure(
            evidence.All(line => !line.Contains(otherEvidence, StringComparison.Ordinal)),
            "SM-D5A notified an Actor that was not selected.");

        Console.WriteLine("operation SpotService.sm-d5a passed");
    }
}
