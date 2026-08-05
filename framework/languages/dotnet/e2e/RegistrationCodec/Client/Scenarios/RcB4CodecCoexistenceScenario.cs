// Verifies RC-B4 Codec Coexistence behavior.
using RegistrationCodec.Client.Support;
using RegistrationCodec.Shared;
using Zlink.HttpClient;

namespace RegistrationCodec.Client.Scenarios;

// RC-B4 verifies JSON, Protobuf, and MessagePack coexistence on one channel.
internal static class RcB4CodecCoexistenceScenario
{
    public static async Task RunAsync(
        ZLinkHttpClient requester,
        ZLinkHttpClient evidenceServer)
    {
        await requester.Post("/codec/roundtrip").Async<CodecScenarioRes>();
        var evidence = (await evidenceServer.Post("/evidence/wait")
            .Body(new EvidenceWaitReq([
                "codec-request|codec=json", "codec-request|codec=protobuf", "codec-request|codec=msgpack"
            ]))
            .Async<string[]>()).Body;
        ZlinkStreamAssert.Ensure(
            EvidenceText.HasCodec(evidence, "json", "application/json")
            && EvidenceText.HasCodec(evidence, "protobuf", "application/x-protobuf")
            && EvidenceText.HasCodec(evidence, "msgpack", "application/x-msgpack"),
            "RC-B4 expected all codec evidence.");

        Console.WriteLine("scenario RC-B4 passed");
    }
}
