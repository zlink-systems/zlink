// Verifies SM-A7 Spot Type Mismatch behavior.
using SpotService.Client.Support;
using SpotService.Shared;
using Zlink.HttpClient;

namespace SpotService.Client.Scenarios;

internal static class SmA7SpotTypeMismatchScenario
{
    public static async Task RunAsync(ZLinkHttpClient playA)
    {
        var spotRid = $"spot-sm-a7-{Guid.NewGuid():N}";
        var mismatch = (await playA.Post("/spot/type-mismatch")
            .Body(new SpotTypeMismatchReq(spotRid))
            .Async<SpotTypeMismatchRes>()).Body;
        ZlinkStreamAssert.Ensure(mismatch.Failed, "SM-A7 expected a Spot type mismatch.");
        ZlinkStreamAssert.Ensure(mismatch.ErrorKind == "TypeMismatch", "SM-A7 error kind mismatch.");
        var expectedEvidence = new[] { $"spot-type-mismatch|rid=play-a|spot={mismatch.SpotRid}|kind=TypeMismatch" };
        var evidence = (await playA.Post("/evidence/wait")
            .Body(new EvidenceWaitReq(expectedEvidence))
            .Async<string[]>()).Body;
        ZlinkStreamAssert.Ensure(
            expectedEvidence.All(expected => evidence.Any(line => line.Contains(expected, StringComparison.Ordinal))),
            "SM-A7 evidence did not include TypeMismatch.");
        Console.WriteLine("operation SpotService.sm-a7 passed");
    }
}
