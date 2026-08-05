// Verifies SM-B3 Request Message Fidelity behavior.
using SpotService.Client.Support;
using SpotService.Shared;
using Systems.Zlink.Stream.Connector.Contracts;
using Zlink.HttpClient;

namespace SpotService.Client.Scenarios;

internal static class SmB3RequestMessageFidelityScenario
{
    public static async Task RunAsync(ZLinkHttpClient playA, string sessionAStreamEndpoint)
    {
        var actorId = $"actor-sm-b3-complex-{Guid.NewGuid():N}";
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
        await client.Request(new AuthReq(actorId, "complex actor"))
            .PacketName("AuthReq")
            .Async<AuthRes>();
        var complex = await client.Request(new ComplexActorReq(
                "Ada Lovelace",
                42,
                ["alpha", "beta", "gamma"],
                new Dictionary<string, string>
                {
                    ["role"] = "analyst",
                    ["region"] = "west"
                }))
            .PacketName("ComplexActorReq")
            .Async<ComplexActorRes>();
        ZlinkStreamAssert.Ensure(complex.ActorId == actorId, "SM-B3 actor id mismatch.");
        ZlinkStreamAssert.Ensure(
            complex.DisplayName == "Ada Lovelace" && complex.Level == 42,
            "SM-B3 scalar payload mismatch.");
        ZlinkStreamAssert.Ensure(complex.Tags.SequenceEqual(["alpha", "beta", "gamma"]), "SM-B3 tag payload mismatch.");
        ZlinkStreamAssert.Ensure(
            complex.Attributes["role"] == "analyst" && complex.Attributes["region"] == "west",
            "SM-B3 attribute payload mismatch.");
        var expectedEvidence = new[] { $"actor-complex|rid=play-a|actor={actorId}" };
        var evidence = (await playA.Post("/evidence/wait")
            .Body(new EvidenceWaitReq(expectedEvidence))
            .Async<string[]>()).Body;
        ZlinkStreamAssert.Ensure(
            expectedEvidence.All(expected => evidence.Any(line => line.Contains(expected, StringComparison.Ordinal))),
            "SM-B3 evidence mismatch.");
        Console.WriteLine("operation SpotService.sm-b3 passed");
    }
}
