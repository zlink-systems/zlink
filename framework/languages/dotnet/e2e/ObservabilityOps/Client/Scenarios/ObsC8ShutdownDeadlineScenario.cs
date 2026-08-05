// Verifies OBS-C8 reports forced shutdown when graceful drain misses its deadline.
using System.Globalization;
using ObservabilityOps.Client.Support;
using ObservabilityOps.Shared;

namespace ObservabilityOps.Client.Scenarios;

internal static class ObsC8ShutdownDeadlineScenario
{
    public static async Task RunAsync(ScenarioContext context)
    {
        var room = await context.CreateRoomOnObservedNodeAsync(
            "play-a",
            $"room-c8-{Guid.NewGuid():N}",
            "block-closing");
        await context.PlayA.Post("/closing-gate/arm")
            .Query("maximumWaitMs", "30000")
            .AsyncRaw();

        var before = DateTimeOffset.UtcNow;
        var shutdown = context.PlayA.Post("/shutdown/direct")
            .Body(new ShutdownHostReq(300))
            .Timeout(TimeSpan.FromSeconds(10))
            .Async<ShutdownHostRes>()
            .AsTask();
        await context.PlayA.Post("/closing-gate/wait-started")
            .Query("timeoutMs", "5000")
            .AsyncRaw();
        var result = (await shutdown).Body;
        var after = DateTimeOffset.UtcNow;

        ZlinkStreamAssert.Ensure(
            result is
            {
                Outcome: "ForceStopped",
                Reason: "DeadlineExceeded"
            },
            $"OBS-C8 shutdown returned {result.Outcome}/{result.Reason}.");
        ZlinkStreamAssert.Ensure(
            after - before < TimeSpan.FromSeconds(5),
            "OBS-C8 shutdown exceeded its bounded teardown window.");

        await context.PlayA.Post("/closing-gate/release").AsyncRaw();
        var evidence = (await context.PlayA.Get("/evidence")
            .Query("spotRid", room.RoomRid)
            .Async<EvidenceSnapshot>()).Body;
        var closing = evidence.Entries.Single(entry =>
            entry.Contains(
                $"spot-closing|kind=user|spot={room.RoomRid}",
                StringComparison.Ordinal));
        var deadlineText = closing.Split('|', StringSplitOptions.RemoveEmptyEntries)
            .Single(part => part.StartsWith(
                "deadline=", StringComparison.Ordinal))
            ["deadline=".Length..];
        var callbackDeadline = DateTimeOffset.Parse(
            deadlineText,
            CultureInfo.InvariantCulture,
            DateTimeStyles.RoundtripKind);
        ZlinkStreamAssert.Ensure(
            callbackDeadline >= before.AddMilliseconds(250)
            && callbackDeadline <= after.AddSeconds(1),
            "OBS-C8 closing callback did not receive the host deadline.");
        ZlinkStreamAssert.Ensure(
            evidence.Entries.Any(entry => entry.Contains(
                $"spot-closing-cancelled|spot={room.RoomRid}",
                StringComparison.Ordinal)),
            "OBS-C8 cleanup cancellation was not signaled at the deadline.");
        ZlinkStreamAssert.Ensure(
            evidence.Metrics.Any(sample =>
                sample.Name == "zlink.host.shutdown.forced"
                && sample.Tags.GetValueOrDefault("reason")
                == "deadline_exceeded"
                && sample.Value == 1),
            "OBS-C8 forced shutdown metric was not recorded exactly once.");
        ZlinkStreamAssert.Ensure(
            evidence.Metrics.All(sample =>
                sample.Name != "zlink.relocation.started"),
            "OBS-C8 shutdown started relocation while closing was blocked.");
        Console.WriteLine("scenario OBS-C8 passed");
    }
}
