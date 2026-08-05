using Microsoft.AspNetCore.Builder;
using Microsoft.AspNetCore.Http;
using ObservabilityOps.Shared;
using Zlink.Framework.Contracts.Configuration;

namespace ObservabilityOps.Server.Support;

public static class MaintenanceEndpointExtensions
{
    public static WebApplication MapMaintenanceOperations(this WebApplication app)
    {
        app.MapPost("/relocate", (
            int? deadlineMs,
            string? mode,
            long? targetApplicationVersion,
            IZLinkFrameworkRuntime runtime,
            RelocationOperation operation) =>
            Results.Ok(operation.Start(
                runtime,
                string.Equals(mode, "rolling-update", StringComparison.Ordinal)
                    ? ZLinkFrameworkRelocationMode.RollingUpdate
                    : ZLinkFrameworkRelocationMode.PlannedMaintenance,
                targetApplicationVersion,
                TimeSpan.FromMilliseconds(deadlineMs ?? 30000))));
        app.MapGet("/relocate/status", (RelocationOperation operation) =>
            Results.Ok(operation.Snapshot()));
        app.MapPost("/relocate/wait", async (
            int? timeoutMs,
            RelocationOperation operation,
            CancellationToken cancellationToken) => Results.Ok(await operation.WaitAsync(
                TimeSpan.FromMilliseconds(timeoutMs ?? 30000), cancellationToken)));
        app.MapPost("/shutdown", (
            int? deadlineMs,
            IZLinkFrameworkRuntime runtime,
            ShutdownOperation operation) =>
            Results.Ok(operation.Start(
                runtime,
                TimeSpan.FromMilliseconds(deadlineMs ?? 30000))));
        app.MapGet("/shutdown/status", (ShutdownOperation operation) =>
            Results.Ok(operation.Snapshot()));
        app.MapPost("/shutdown/wait", async (
            int? timeoutMs,
            ShutdownOperation operation,
            CancellationToken cancellationToken) => Results.Ok(await operation.WaitAsync(
            TimeSpan.FromMilliseconds(timeoutMs ?? 30000), cancellationToken)));
        app.MapPost("/relocate/direct", async (
            RelocateHostReq request,
            IZLinkFrameworkRuntime runtime,
            CancellationToken cancellationToken) =>
        {
            var result = await runtime.RelocateAsync(
                new ZLinkFrameworkRelocationOptions
                {
                    Mode = request.Mode == "rolling-update"
                        ? ZLinkFrameworkRelocationMode.RollingUpdate
                        : ZLinkFrameworkRelocationMode.PlannedMaintenance,
                    TargetApplicationVersion = request.TargetApplicationVersion,
                    Deadline = TimeSpan.FromMilliseconds(
                        request.DeadlineMilliseconds)
                },
                cancellationToken);
            return Results.Ok(new RelocateHostRes(
                result.Mode.ToString(),
                result.TargetApplicationVersion,
                result.Outcome.ToString(),
                result.Reason.ToString()));
        });
        app.MapPost("/shutdown/direct", async (
            ShutdownHostReq request,
            IZLinkFrameworkRuntime runtime,
            CancellationToken cancellationToken) =>
        {
            var result = await runtime.ShutdownAsync(
                TimeSpan.FromMilliseconds(request.DeadlineMilliseconds),
                cancellationToken);
            return Results.Ok(new ShutdownHostRes(
                result.Outcome.ToString(),
                result.Reason.ToString()));
        });
        app.MapGet("/runtime/status", (IZLinkFrameworkRuntime runtime) =>
            Results.Ok(new RuntimeStatusRes(
                runtime.Status.State.ToString(),
                runtime.Status.IsReady,
                runtime.Status.AcceptingWork,
                runtime.Status.Sequence)));
        return app;
    }
}
