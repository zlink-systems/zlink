// Verifies SM-G4 Concurrent Bound Session Push behavior.
using SpotService.Client.Support;
using SpotService.Shared;
using Systems.Zlink.Stream.Connector.Contracts;

namespace SpotService.Client.Scenarios;

// Verifies bound-session push isolation under several concurrent actors.
internal static class SmG4ConcurrentBoundSessionPushScenario
{
    public static async Task RunAsync(string sessionAStreamEndpoint)
    {
        const int sessionCount = 6;
        var clients = new List<IZlinkStreamConnector>();
        try
        {
            for (var index = 0; index < sessionCount; index++)
            {
                var client = ZlinkStreamConnectorFactory.Create(new ZlinkStreamConnectorOptions
                {
                    Endpoint = new Uri(sessionAStreamEndpoint)
                });
                clients.Add(client);
                await client.Connect.Async();
                await client.Request(new AuthReq($"actor-sm-g4-{index}", $"bound-load-{index}"))
                    .PacketName("AuthReq")
                    .Async<AuthRes>();
            }

            var results = new List<(string ActorId, string Value, ActorPingRes Reply, ActorPushNotify Notify)>();
            for (var index = 0; index < clients.Count; index++)
            {
                var client = clients[index];
                var actorId = $"actor-sm-g4-{index}";
                var value = $"push-{index}";
                var pushed = client.WaitFor<ActorPushNotify>().Async().AsTask();
                var reply = await client.Request(new ActorPushReq(value))
                    .PacketName("ActorPushReq")
                    .Async<ActorPingRes>();
                var notify = await pushed;
                results.Add((actorId, value, reply, notify.Payload));
            }

            foreach (var (actorId, value, reply, notify) in results)
            {
                ZlinkStreamAssert.Ensure(reply.ActorId == actorId, "SM-G4 push reply actor mismatch.");
                ZlinkStreamAssert.Ensure(reply.Value == value, "SM-G4 push reply value mismatch.");
                ZlinkStreamAssert.Ensure(notify.ActorId == actorId, "SM-G4 push notify actor mismatch.");
                ZlinkStreamAssert.Ensure(notify.Value == value, "SM-G4 push notify value mismatch.");
            }
        }
        finally
        {
            foreach (var client in clients) await client.DisposeAsync();
        }

        Console.WriteLine("operation SpotService.sm-g4 passed");
    }
}
