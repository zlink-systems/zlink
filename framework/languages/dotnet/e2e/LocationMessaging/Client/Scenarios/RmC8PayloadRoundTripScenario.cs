// Verifies RM-C8 Payload Round Trip behavior.
using System.Security.Cryptography;
using System.Text;
using LocationMessaging.Client.Support;
using LocationMessaging.Shared;
using Zlink.HttpClient;

namespace LocationMessaging.Client.Scenarios;

// RM-C8 verifies RouteMesh SS payload round trips by checking returned length
// and hash for several message sizes.
internal static class RmC8PayloadRoundTripScenario
{
    public static async Task RunAsync(
        ZLinkHttpClient directConsumer,
        ZLinkHttpClient providerA,
        ZLinkHttpClient providerB)
    {
        var beforeA = (await providerA.Get("/evidence").Async<string[]>()).Body;
        var beforeB = (await providerB.Get("/evidence").Async<string[]>()).Body;
        var markers = new List<string>();
        foreach (var size in new[] { 1, 4096, 256 * 1024, 1024 * 1024 })
        {
            var marker = $"rm-c8-{size}-{Guid.NewGuid():N}";
            markers.Add(marker);
            var payload = BuildPayload(size);
            var expectedHash = HashPayload(payload);
            var reply = (await directConsumer.Post("/profile/payload")
                .Body(new PayloadReq(marker, payload))
                .Async<PayloadRes>()).Body;
            ZlinkStreamAssert.Ensure(reply.Marker == marker, "RM-C8 marker mismatch.");
            ZlinkStreamAssert.Ensure(reply.Length == payload.Length, "RM-C8 payload length mismatch.");
            ZlinkStreamAssert.Ensure(reply.Sha256 == expectedHash, "RM-C8 payload hash mismatch.");
        }

        var followUp = (await directConsumer.Post("/profile/request")
            .Body(new ProfileReq("rm-c8-after"))
            .Async<ProfileRes>()).Body;
        ZlinkStreamAssert.Ensure(followUp.Value == "profile:rm-c8-after", "RM-C8 follow-up request failed.");

        var afterA = (await providerA.Get("/evidence").Async<string[]>()).Body;
        var afterB = (await providerB.Get("/evidence").Async<string[]>()).Body;
        ZlinkStreamAssert.Ensure(
            markers.All(marker =>
                EvidenceDelta.CountMatching(afterA, beforeA, "payload-request|rid=api-a", marker)
                + EvidenceDelta.CountMatching(afterB, beforeB, "payload-request|rid=api-b", marker) == 1),
            "RM-C8 payload evidence missing.");
    }

    private static string BuildPayload(int size)
    {
        var builder = new StringBuilder(size);
        for (var i = 0; i < size; i++) builder.Append((char)('a' + i % 26));

        return builder.ToString();
    }

    private static string HashPayload(string payload)
    {
        return Convert.ToHexString(SHA256.HashData(Encoding.UTF8.GetBytes(payload)));
    }
}
