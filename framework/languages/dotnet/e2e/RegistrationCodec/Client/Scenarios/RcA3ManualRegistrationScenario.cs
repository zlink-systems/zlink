// Verifies RC-A3 Manual Registration behavior.
using RegistrationCodec.Client.Support;
using RegistrationCodec.Shared;
using Zlink.HttpClient;

namespace RegistrationCodec.Client.Scenarios;

// RC-A3: verifies manually registered handlers for request and send messages.
internal static class RcA3ManualRegistrationScenario
{
    public static async Task RunAsync(
        ZLinkHttpClient requester,
        ZLinkHttpClient evidenceServer)
    {
        var reply = (await requester.Post("/registration/manual").Async<EchoRes>()).Body;
        ZlinkStreamAssert.Ensure(reply.Value == "echo:rc-a3", "RC-A3 request reply mismatch.");

        var evidence = (await evidenceServer.Post("/evidence/wait")
            .Body(new EvidenceWaitReq(["echo-command|variant=manual|id=cmd-rc-a3"]))
            .Async<string[]>()).Body;
        ZlinkStreamAssert.Ensure(
            evidence.Any(line => line.Contains("echo-command|variant=manual|id=cmd-rc-a3", StringComparison.Ordinal)),
            "RC-A3 send evidence missing.");

        Console.WriteLine("scenario RC-A3 passed");
    }
}
