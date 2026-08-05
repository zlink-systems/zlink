// Verifies OBS-C4 Forced Session Drain behavior.
using ObservabilityOps.Client.Support;
using ObservabilityOps.Shared;
using Systems.Zlink.Stream.Connector.Contracts;

namespace ObservabilityOps.Client.Scenarios;

internal static class ObsC4ForcedSessionDrainScenario
{
    public static async Task RunAsync(ScenarioContext context)
    {
        _ = await context.PlayNodeIdAsync("play-a");
        _ = await context.PlayNodeIdAsync("play-b");
        var suffix = Guid.NewGuid().ToString("N");
        var actorId = $"obs-c4-{suffix}";
        var room = (await context.PlayA.Post("/rooms")
            .Body(new CreateRoomReq($"room-c4-{suffix}"))
            .Async<CreateRoomRes>()).Body;
        var playOwner = context.Play(room.NodeRid);
        var instance = await context.ActivateInstanceOnObservedNodeAsync(
            room.NodeRid, $"instance-c4-{suffix}");

        var disconnected = new TaskCompletionSource<ZlinkStreamCloseReason>(
            TaskCreationOptions.RunContinuationsAsynchronously);
        var closingObserved = new TaskCompletionSource(
            TaskCreationOptions.RunContinuationsAsynchronously);
        await using var connector = await context.ConnectAsync(
            reconnectEnabled: false,
            configure: candidate =>
            {
                candidate.Disconnected += (closed, _) =>
                {
                    disconnected.TrySetResult(closed.CloseReason);
                    return ValueTask.CompletedTask;
                };
                candidate.ObserveInbound((frame, _) =>
                {
                    if (frame.Name == "session-closing") closingObserved.TrySetResult();
                    return ValueTask.CompletedTask;
                });
            });
        await connector.Request(new AuthenticateReq(actorId))
            .Async<AuthenticateRes>();
        await context.JoinRoomAsync(connector, actorId, room.RoomRid);

        await playOwner.Post("/shutdown?deadlineMs=10000").AsyncRaw();
        var playShutdown = await ScenarioContext.WaitForShutdownAsync(
            playOwner, TimeSpan.FromSeconds(12));
        ZlinkStreamAssert.Ensure(
            playShutdown.Result == "Stopped"
            && playShutdown.Reason == "None",
            $"OBS-C4 Play shutdown returned "
            + $"{playShutdown.Result}/{playShutdown.Reason}.");
        var closing = (await playOwner.Post("/evidence/wait")
            .Body(new EvidenceWaitReq(
                [
                    $"spot-closing|kind=user|spot={room.RoomRid}",
                    $"spot-closing|kind=instance|spot={instance.SpotId}",
                    "spot-closing|kind=entry|"
                ],
                [],
                10000))
            .Async<string[]>()).Body;
        ZlinkStreamAssert.Ensure(
            closing.Count(line =>
                line.Contains("reason=HostShutdown", StringComparison.Ordinal))
            >= 3,
            "OBS-C4 did not deliver HostShutdown to Entry, User and Instance Spots.");
        var playMetrics = (await playOwner.Get("/evidence")
            .Async<EvidenceSnapshot>()).Body.Metrics;
        ZlinkStreamAssert.Ensure(
            playMetrics.All(sample =>
                sample.Name != "zlink.relocation.started"),
            "OBS-C4 Shutdown unexpectedly started relocation.");

        await context.Session.Post("/operation-gate/arm")
            .Query("maximumWaitMs", "10000").AsyncRaw();
        var blockedOperation = connector.Request(new SessionBoundedOperationReq("obs-c4-blocked"))
            .Async<SessionBoundedOperationRes>().AsTask();
        await context.Session.Post("/operation-gate/wait-started")
            .Query("timeoutMs", "5000").AsyncRaw();
        await context.Session.Post("/shutdown?deadlineMs=100").AsyncRaw();
        await closingObserved.Task.WaitAsync(TimeSpan.FromSeconds(10));
        var reason = await disconnected.Task.WaitAsync(TimeSpan.FromSeconds(10));
        ZlinkStreamAssert.Ensure(reason == ZlinkStreamCloseReason.ServerDrain,
            $"OBS-C4 connector close reason was {reason}, not ServerDrain.");
        var result = await ScenarioContext.WaitForShutdownAsync(
            context.Session, TimeSpan.FromSeconds(10));
        ZlinkStreamAssert.Ensure(result.Result == "ForceStopped" && result.Reason == "DeadlineExceeded",
            $"OBS-C4 short deadline returned {result.Result}/{result.Reason}.");
        await context.Session.Post("/operation-gate/release").AsyncRaw();
        try
        {
            await blockedOperation.WaitAsync(TimeSpan.FromSeconds(2));
        }
        catch (Exception)
        {
            // Forced session cleanup is allowed to terminate the blocked request.
        }
        var metrics = (await context.Session.Get("/evidence").Async<EvidenceSnapshot>()).Body.Metrics;
        ZlinkStreamAssert.Ensure(metrics.Any(sample =>
                sample.Name == "zlink.host.shutdown.forced"
                && sample.Tags.GetValueOrDefault("reason") == "deadline_exceeded"
                && sample.Value >= 1),
            "OBS-C4 forced session metric was not recorded.");
        Console.WriteLine("scenario OBS-C4 passed");
    }

}
