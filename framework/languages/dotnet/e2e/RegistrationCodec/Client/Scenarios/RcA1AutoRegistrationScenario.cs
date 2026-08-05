// Verifies RC-A1 Auto Registration behavior.
using RegistrationCodec.Client.Support;
using RegistrationCodec.Shared;
using Zlink.HttpClient;

namespace RegistrationCodec.Client.Scenarios;

// RC-A1: verifies assembly/module auto registration for request and send handlers.
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
            "RC-A1 send evidence missing.");

        Console.WriteLine("scenario RC-A1 passed");
    }
}
