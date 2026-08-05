using System.Diagnostics;
using Microsoft.AspNetCore.Mvc;
using StoreFailure.Shared;
using Zlink.Framework.Contracts.Channels;
using Zlink.Framework.Contracts.Configuration;

namespace StoreFailure.Server.Provider;

internal static class ProviderEndpoints
{
    public static WebApplication MapProviderEndpoints(this WebApplication app, ServerOptions options)
    {
        app.MapGet("/health", () => Results.Ok(new { status = "ready", options.Role, options.Rid }));
        app.MapGet("/query/status", async (
            Zlink.Framework.Contracts.Locations.IZLinkLocationRuntimeQuery query,
            CancellationToken cancellationToken) =>
        {
            var status = await query.GetStatusAsync(cancellationToken);
            return Results.Ok(new RuntimeStatusRes(
                status.StoreHealthy,
                status.OwnerLeaseHealthy,
                status.OwnerLeaseRenewedAt,
                status.LastRefreshAt));
        });
        app.MapPost("/query/status/wait", async (
            RuntimeStatusWaitReq request,
            Zlink.Framework.Contracts.Locations.IZLinkLocationRuntimeQuery query,
            CancellationToken cancellationToken) =>
        {
            var response = await RuntimeStatusWaiter.WaitAsync(async token =>
            {
                var status = await query.GetStatusAsync(token);
                return new RuntimeStatusRes(
                    status.StoreHealthy,
                    status.OwnerLeaseHealthy, status.OwnerLeaseRenewedAt, status.LastRefreshAt);
            }, request, cancellationToken);
            if (response is not null) return Results.Ok(response);

            return Results.Problem("Runtime status did not reach the requested state.",
                statusCode: StatusCodes.Status504GatewayTimeout);
        });
        app.MapGet("/evidence", (EvidenceStore evidence) => Results.Ok(evidence.Snapshot()));
        app.MapPost("/evidence/wait", async (
            EvidenceWaitReq request,
            EvidenceStore evidence,
            CancellationToken cancellationToken) =>
        {
            var timeout = TimeSpan.FromMilliseconds(Math.Clamp(request.TimeoutMilliseconds, 1, 30000));
            var snapshot = await evidence.WaitUntilAsync(
                entries => request.ContainsAll.All(expected =>
                               entries.Any(entry => entry.Contains(expected, StringComparison.Ordinal)))
                           && request.ContainsAnyGroups.All(group =>
                               group.Any(expected =>
                                   entries.Any(entry => entry.Contains(expected, StringComparison.Ordinal)))),
                timeout,
                cancellationToken);
            return Results.Ok(snapshot);
        });
        app.MapPost("/evidence/clear", (EvidenceStore evidence) =>
        {
            evidence.Clear();
            return Results.Ok(new { status = "cleared" });
        });
        app.MapPost("/drain", async (IZLinkFrameworkRuntime runtime) =>
        {
            var relocation = await runtime.RelocateAsync(
                new ZLinkFrameworkRelocationOptions
                {
                    Mode = ZLinkFrameworkRelocationMode.PlannedMaintenance,
                    Deadline = TimeSpan.FromSeconds(30)
                });
            if (relocation.Outcome != ZLinkFrameworkRelocationOutcome.Relocated)
                return Results.Ok(new DrainResultRes(
                    relocation.Outcome.ToString(),
                    relocation.Reason.ToString()));
            var result = await runtime.ShutdownAsync(TimeSpan.FromSeconds(30));
            return Results.Ok(new DrainResultRes(
                result.Outcome.ToString(),
                result.Reason.ToString()));
        });
        app.MapPost("/shutdown", (IHostApplicationLifetime lifetime) =>
        {
            lifetime.StopApplication();
            return Results.Ok(new { status = "stopping" });
        });
        app.MapPost("/admin/fault/{mode}", (
            string mode,
            [FromServices] FaultState fault,
            [FromServices] EvidenceStore evidence) =>
        {
            fault.Mode = mode;
            evidence.Add($"admin|rid={evidence.Rid}|action=fault|mode={mode}");
            return Results.Ok(new { status = "fault", mode });
        });
        app.MapPost("/admin/weight/wait", async (
            WeightWaitReq request,
            [FromServices] IZLinkRouteMeshRuntimeOptions runtimeOptions,
            CancellationToken cancellationToken) =>
        {
            var timeout = TimeSpan.FromMilliseconds(Math.Clamp(request.TimeoutMilliseconds, 1, 30000));
            var elapsed = Stopwatch.StartNew();
            while (elapsed.Elapsed < timeout)
            {
                var weight = runtimeOptions.Channel(StoreFailureNames.Channel).Weight;
                if (weight == request.Expected) return Results.Ok(new { weight });

                await Task.Delay(TimeSpan.FromMilliseconds(50), cancellationToken);
            }

            return Results.Problem(
                $"Provider weight did not become {request.Expected}.",
                statusCode: StatusCodes.Status504GatewayTimeout);
        });
        return app;
    }
}
