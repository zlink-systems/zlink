// Verifies RC-B3 Message Pack Codec behavior.
using RegistrationCodec.Client.Support;
using Zlink.HttpClient;

namespace RegistrationCodec.Client.Scenarios;

// RC-B3 verifies MessagePack codec round trip.
internal static class RcB3MessagePackCodecScenario
{
    public static async Task RunAsync(ZLinkHttpClient requester)
    {
        var result = (await requester.Post("/codec/roundtrip").Async<CodecScenarioRes>()).Body;
        ZlinkStreamAssert.Ensure(result.MessagePackValue.Contains("echo:rc-b3", StringComparison.Ordinal),
            "RC-B3 MessagePack reply mismatch.");
        ZlinkStreamAssert.Ensure(
            result.MessagePackValue.Contains("content:application/x-msgpack", StringComparison.Ordinal),
            "RC-B3 MessagePack content type mismatch.");

        Console.WriteLine("scenario RC-B3 passed");
    }
}
