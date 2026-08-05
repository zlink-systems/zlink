// Verifies SM-E4 Timer Overrun Policy behavior.
using SpotService.Client.Support;
using SpotService.Shared;
using Zlink.HttpClient;

namespace SpotService.Client.Scenarios;

// Verifies the final E-track spot-service behavior.
internal static class SmE4TimerOverrunPolicyScenario
{
    public static async Task RunAsync(ZLinkHttpClient playA)
    {
        var policySpots = new Dictionary<string, string>
        {
            ["SkipLateTicks"] = $"spot-sm-e4-skip-{Guid.NewGuid():N}",
            ["CatchUpBounded"] = $"spot-sm-e4-catch-{Guid.NewGuid():N}",
            ["DelayNextTick"] = $"spot-sm-e4-delay-{Guid.NewGuid():N}"
        };

        foreach (var spotRid in policySpots.Values)
        {
            var created = (await playA.Post("/spot/create")
                .Body(new CreateSpotReq(spotRid))
                .Async<CreateSpotRes>()).Body;
            ZlinkStreamAssert.Ensure(created.SpotRid == spotRid && created.NodeRid == "play-a",
                "SM-E4 timer spot was not created on play-a.");
            var ready = (await playA.Post("/spot/state/request")
                .Body(new SpotStateRouteReq(spotRid, "noop", 0))
                .Async<StateRes>()).Body;
            ZlinkStreamAssert.Ensure(ready.SpotRid == spotRid && ready.NodeRid == "play-a",
                "SM-E4 timer spot route did not become ready.");
        }

        foreach (var (policy, spotRid) in policySpots)
        {
            var started = (await playA.Post("/spot/overrun/start")
                .Body(new SpotOverrunStartReq(spotRid, $"sm-e4-{policy}", policy, 25))
                .Async<SpotOverrunStartRes>()).Body;
            ZlinkStreamAssert.Ensure(started.Started, $"SM-E4 {policy} overrun timer did not start.");
        }

        var evidenceRequest = new EvidenceWaitReq(
            policySpots.Select(pair => $"timer-overrun|rid=play-a|spot={pair.Value}|name=sm-e4-{pair.Key}").ToArray(),
            15000);
        var evidence = (await playA.Post("/evidence/wait")
            .Body(evidenceRequest)
            .Async<string[]>()).Body;
        ZlinkStreamAssert.Ensure(
            EvidenceIncludesExpectedTimerPolicies(evidence),
            "SM-E4 timer overrun evidence missing or incomplete.");
        var skipLateTicksSkipped = evidence
            .Where(line => line.Contains("name=sm-e4-SkipLateTicks", StringComparison.Ordinal))
            .Any(line => ExtractUInt64(line, "skipped") > 0);
        var catchUpBoundedSkipped = evidence
            .Where(line => line.Contains("name=sm-e4-CatchUpBounded", StringComparison.Ordinal))
            .Any(line => ExtractUInt64(line, "skipped") > 0);
        var delayNextTickStayedOnSchedule = evidence
            .Where(line => line.Contains("name=sm-e4-DelayNextTick", StringComparison.Ordinal))
            .Take(3)
            .All(line => ExtractUInt64(line, "skipped") == 0
                         && ExtractUInt64(line, "delivery") == ExtractUInt64(line, "scheduled"));

        ZlinkStreamAssert.Ensure(skipLateTicksSkipped, "SM-E4 SkipLateTicks did not skip late ticks.");
        ZlinkStreamAssert.Ensure(catchUpBoundedSkipped, "SM-E4 CatchUpBounded did not show bounded catch-up evidence.");
        ZlinkStreamAssert.Ensure(delayNextTickStayedOnSchedule, "SM-E4 DelayNextTick did not stay on schedule.");
        ZlinkStreamAssert.Ensure(EvidenceIncludesExpectedTimerPolicies(evidence), "SM-E4 timer evidence missing.");

        Console.WriteLine("operation SpotService.sm-e4 passed");

        bool EvidenceIncludesExpectedTimerPolicies(string[] entries)
        {
            return policySpots.All(pair =>
                entries.Count(line =>
                    line.Contains($"timer-overrun|rid=play-a|spot={pair.Value}|name=sm-e4-{pair.Key}",
                        StringComparison.Ordinal)) >= 3);
        }
    }

    private static ulong ExtractUInt64(string line, string key)
    {
        var prefix = key + "=";
        var start = line.IndexOf(prefix, StringComparison.Ordinal);
        if (start < 0) throw new InvalidOperationException($"Missing field '{key}' in evidence line: {line}");

        start += prefix.Length;
        var end = line.IndexOf('|', start);
        var value = end < 0 ? line[start..] : line[start..end];
        return ulong.Parse(value);
    }
}
