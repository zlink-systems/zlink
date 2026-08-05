// Verifies OBS-C6 relocates a host only to the requested higher version.
using ObservabilityOps.Client.Support;
using ObservabilityOps.Shared;
using Systems.Zlink.Stream.Connector.Contracts;

namespace ObservabilityOps.Client.Scenarios;

internal static class ObsC6RollingUpdateScenario
{
    public static async Task RunAsync(ScenarioContext context)
    {
        var sourceNode = await context.PlayNodeIdAsync("play-a");
        var targetNode = await context.PlayNodeIdAsync("play-b");
        await using var workload = await context.PreparePlayWorkloadAsync(
            sourceNode, "c6");
        using var trafficCancellation = new CancellationTokenSource();
        var traffic = RunTrafficAsync(workload, trafficCancellation.Token);

        var result = (await context.PlayA.Post("/relocate/direct")
            .Body(new RelocateHostReq("rolling-update", 2, 30000))
            .Timeout(TimeSpan.FromSeconds(35))
            .Async<RelocateHostRes>()).Body;
        trafficCancellation.Cancel();
        var trafficResult = await traffic;

        ZlinkStreamAssert.Ensure(
            result is
            {
                Mode: "RollingUpdate",
                TargetApplicationVersion: 2,
                Outcome: "Relocated",
                Reason: "None"
            },
            $"OBS-C6 rolling update returned "
            + $"{result.Mode}/{result.TargetApplicationVersion}/"
            + $"{result.Outcome}/{result.Reason}.");
        ZlinkStreamAssert.Ensure(
            trafficResult.Completed > 0
            && trafficResult.TerminalAttempts > 0,
            "OBS-C6 did not complete finite traffic attempts during the update.");

        var relocated = await context.WaitForPlayWorkloadAsync(
            workload.Room.RoomRid,
            workload.ActorId,
            targetNode,
            TimeSpan.FromSeconds(15));
        ZlinkStreamAssert.Ensure(
            relocated.ActorRows.Single(row =>
                row.ActorId == workload.ActorId).Generation
                == workload.ActorGeneration
            && relocated.SpotRows.Single(row =>
                row.SpotRid == workload.Room.RoomRid).Generation
                == workload.RoomGeneration,
            "OBS-C6 changed Actor or User Spot ObjectGeneration.");
        var instance = (await context.PlayB.Get("/evidence")
            .Async<EvidenceSnapshot>()).Body;
        ZlinkStreamAssert.Ensure(
            instance.Entries.Any(entry =>
                entry.Contains(
                    $"instance-initialized|spot={workload.Instance.SpotId}"
                    + $"|node={targetNode}",
                    StringComparison.Ordinal)),
            "OBS-C6 Instance Spot did not initialize on the target node.");

        //  이동이 막 끝난 창에서는 `Actor is moving`이 `Unavailable`로 돌아올 수
        //  있다. 런타임이 일시 상태로 분류하는 값이므로 예산 안에서 재시도한다.
        GameActionRes? action = null;
        var actionDeadline = DateTime.UtcNow + TimeSpan.FromSeconds(15);
        Exception? lastActionError = null;
        while (DateTime.UtcNow < actionDeadline)
        {
            try
            {
                action = await workload.Connector.Request(
                        new GameActionReq("obs-c6-after"))
                    .Async<GameActionRes>();
                break;
            }
            catch (Exception error)
            {
                lastActionError = error;
                await Task.Delay(TimeSpan.FromMilliseconds(250));
            }
        }

        ZlinkStreamAssert.Ensure(action is not null,
            $"OBS-C6 bound session never reached the new version: {lastActionError?.Message}");
        ZlinkStreamAssert.Ensure(
            action!.NodeRid == targetNode,
            "OBS-C6 bound session did not continue on the new version.");
        var status = (await context.PlayA.Get("/runtime/status")
            .Async<RuntimeStatusRes>()).Body;
        ZlinkStreamAssert.Ensure(
            status.State == "Relocated" && !status.AcceptingWork,
            "OBS-C6 source did not remain in Relocated state.");

        var shutdown = (await context.PlayA.Post("/shutdown/direct")
            .Body(new ShutdownHostReq())
            .Timeout(TimeSpan.FromSeconds(35))
            .Async<ShutdownHostRes>()).Body;
        ZlinkStreamAssert.Ensure(
            shutdown is { Outcome: "Stopped", Reason: "None" },
            $"OBS-C6 source shutdown returned "
            + $"{shutdown.Outcome}/{shutdown.Reason}.");
        Console.WriteLine("scenario OBS-C6 passed");
    }

    private static async Task<TrafficResult> RunTrafficAsync(
        PlayWorkload workload,
        CancellationToken cancellationToken)
    {
        var completed = 0;
        var terminalAttempts = 0;
        var sequence = 0;
        while (!cancellationToken.IsCancellationRequested)
        {
            var marker = $"obs-c6-live-{sequence++}";
            try
            {
                var response = await workload.Connector.Request(
                        new GameActionReq(marker))
                    .Async<GameActionRes>(cancellationToken);
                ZlinkStreamAssert.Ensure(
                    response.Marker == marker,
                    "OBS-C6 request reply ordering changed.");
                if (response.ActorId == workload.ActorId) completed++;
                terminalAttempts++;
            }
            catch (OperationCanceledException)
                when (cancellationToken.IsCancellationRequested)
            {
                break;
            }
            catch (ZlinkStreamException exception)
                when (ScenarioContext.IsTransientTrafficFailure(exception))
            {
                // The Stream connector exposes moving as a RemoteError and a
                // request deadline as RequestTimeout. Both are terminal
                // results for this request; the same operation is not retried.
                terminalAttempts++;
            }
        }

        return new TrafficResult(completed, terminalAttempts);
    }

    private sealed record TrafficResult(
        int Completed,
        int TerminalAttempts);
}
