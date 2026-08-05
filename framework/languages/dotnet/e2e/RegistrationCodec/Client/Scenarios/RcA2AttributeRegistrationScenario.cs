// Verifies RC-A2 Attribute Registration behavior.
using RegistrationCodec.Client.Support;
using RegistrationCodec.Shared;
using Zlink.HttpClient;

namespace RegistrationCodec.Client.Scenarios;

// RC-A2: verifies attribute-based packet names for request and send handlers.
internal static class RcA2AttributeRegistrationScenario
{
    public static async Task RunAsync(
        ZLinkHttpClient requester,
        ZLinkHttpClient evidenceServer)
    {
        var reply = (await requester.Post("/registration/attribute").Async<EchoRes>()).Body;
        ZlinkStreamAssert.Ensure(reply.Value == "echo:rc-a2", "RC-A2 request reply mismatch.");

        var evidence = (await evidenceServer.Post("/evidence/wait")
            .Body(new EvidenceWaitReq(["echo-command|variant=attr|id=cmd-rc-a2"]))
            .Async<string[]>()).Body;
        ZlinkStreamAssert.Ensure(
            evidence.Any(line => line.Contains("echo-command|variant=attr|id=cmd-rc-a2", StringComparison.Ordinal)),
            "RC-A2 send evidence missing.");

        Console.WriteLine("scenario RC-A2 passed");
    }
}
