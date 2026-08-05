// Verifies RC-B5 Codec Mismatch behavior.
using RegistrationCodec.Shared;
using Systems.Zlink.Stream.Connector.Contracts;
using Zlink.HttpClient;

namespace RegistrationCodec.Client.Scenarios;

// RC-B5: verifies a codec mismatch failure through the server app endpoint, then JSON recovery.
internal static class RcB5CodecMismatchScenario
{
    public static async Task RunAsync(ZLinkHttpClient codecRequester)
    {
        var mismatch = (await codecRequester.Post("/codec/protobuf/request")
            .Timeout(TimeSpan.FromSeconds(5))
            .Async<CodecMismatchProbeRes>()).Body;
        ZlinkStreamAssert.Ensure(mismatch.Rejected, "RC-B5 Protobuf mismatch was not rejected.");

        var json = (await codecRequester.Post("/codec/json/request")
            .Timeout(TimeSpan.FromSeconds(5))
            .Async<EchoRes>()).Body;
        ZlinkStreamAssert.Ensure(json.Value == "echo:rc-b5-json", "RC-B5 JSON recovery reply did not match.");

        Console.WriteLine("scenario RC-B5 passed");
    }
}
