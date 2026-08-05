// Verifies OBS-C12 resolves concurrent Relocate and Shutdown deterministically.
using ObservabilityOps.Client.Support;
using ObservabilityOps.Shared;

namespace ObservabilityOps.Client.Scenarios;

internal static class ObsC12RelocateShutdownRaceScenario
{
    public static async Task RunAsync(ScenarioContext context)
    {
        var sourceNode = await context.PlayNodeIdAsync("play-a");
        await using var workload = await context.PreparePlayWorkloadAsync(
            sourceNode, "c12");
        var intent = new RelocateHostReq(
            "planned-maintenance", null, 30000);
        var primary = context.PlayA.Post("/relocate/direct")
            .Body(intent)
            .Timeout(TimeSpan.FromSeconds(35))
            .Async<RelocateHostRes>()
            .AsTask();
        await WaitForServingPreflightAsync(context, primary);

        using var cancelledWaiter = new CancellationTokenSource(
            TimeSpan.FromMilliseconds(150));
        var cancelled = context.PlayA.Post("/relocate/direct")
            .Body(intent)
            .Async<RelocateHostRes>(cancelledWaiter.Token)
            .AsTask();
        try
        {
            await cancelled;
            throw new InvalidOperationException(
                "OBS-C12 cancelled waiter returned a terminal result.");
        }
        catch (OperationCanceledException)
            when (cancelledWaiter.IsCancellationRequested)
        {
            // Only this HTTP waiter is cancelled; the host operation continues.
        }

        var shutdown = context.PlayA.Post("/shutdown/direct")
            .Body(new ShutdownHostReq(30000))
            .Timeout(TimeSpan.FromSeconds(35))
            .Async<ShutdownHostRes>()
            .AsTask();
        var relocationResult = (await primary).Body;
        var shutdownResult = (await shutdown).Body;
        ZlinkStreamAssert.Ensure(
            relocationResult is
            {
                Outcome: "Blocked",
                Reason: "ShutdownRequested"
            },
            $"OBS-C12 relocation returned "
            + $"{relocationResult.Outcome}/{relocationResult.Reason}.");
        ZlinkStreamAssert.Ensure(
            shutdownResult.Outcome is "Stopped" or "ForceStopped",
            "OBS-C12 shutdown did not reach a terminal result.");

        var relocationReplay = (await context.PlayA.Post("/relocate/direct")
            .Body(intent)
            .Async<RelocateHostRes>()).Body;
        var shutdownReplay = (await context.PlayA.Post("/shutdown/direct")
            .Body(new ShutdownHostReq(30000))
            .Async<ShutdownHostRes>()).Body;
        ZlinkStreamAssert.Ensure(
            relocationReplay == relocationResult
            && shutdownReplay == shutdownResult,
            "OBS-C12 terminal result replay changed.");
        Console.WriteLine("scenario OBS-C12 passed");
    }

    private static async Task WaitForServingPreflightAsync(
        ScenarioContext context,
        Task primary)
    {
        var deadline = DateTimeOffset.UtcNow + TimeSpan.FromSeconds(10);
        while (DateTimeOffset.UtcNow < deadline)
        {
            var status = (await context.PlayA.Get("/runtime/status")
                .Async<RuntimeStatusRes>()).Body;
            if (status.State == "Serving" && !primary.IsCompleted) return;
            await Task.Delay(50);
        }

        throw new TimeoutException(
            "OBS-C12 relocation did not wait in Serving preflight.");
    }
}
