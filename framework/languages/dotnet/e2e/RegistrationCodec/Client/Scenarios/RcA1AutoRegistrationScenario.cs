// Verifies RC-A1 Scan And Attribute Registration behavior.
using RegistrationCodec.Client.Support;
using RegistrationCodec.Shared;
using Zlink.HttpClient;

namespace RegistrationCodec.Client.Scenarios;

// RC-A1 verifies assembly/module scanning and attribute packet names for request and send handlers.
internal static class RcA1AutoRegistrationScenario
{
    public static async Task RunAsync(
        ZLinkHttpClient requester,
        ZLinkHttpClient evidenceServer)
    {
        var reply = (await requester.Post("/registration/auto").Async<EchoRes>()).Body;
        ZlinkStreamAssert.Ensure(reply.Value == "echo:rc-a1", "RC-A1 request reply mismatch.");

        var evidence = (await evidenceServer.Post("/evidence/wait")
            .Body(new EvidenceWaitReq(["echo-command|variant=auto|id=cmd-rc-a1"]))
            .Async<string[]>()).Body;
        ZlinkStreamAssert.Ensure(
            evidence.Any(line => line.Contains("echo-command|variant=auto|id=cmd-rc-a1", StringComparison.Ordinal)),
            "RC-A1 scan registration send evidence missing.");

        var attributeReply = (await requester.Post("/registration/attribute").Async<EchoRes>()).Body;
        ZlinkStreamAssert.Ensure(
            attributeReply.Value == "echo:rc-a2",
            "RC-A1 attribute registration request reply mismatch.");

        var attributeEvidence = (await evidenceServer.Post("/evidence/wait")
            .Body(new EvidenceWaitReq(["echo-command|variant=attr|id=cmd-rc-a2"]))
            .Async<string[]>()).Body;
        ZlinkStreamAssert.Ensure(
            attributeEvidence.Any(line =>
                line.Contains("echo-command|variant=attr|id=cmd-rc-a2", StringComparison.Ordinal)),
            "RC-A1 attribute registration send evidence missing.");

        Console.WriteLine("scenario RC-A1 passed");
    }
}
