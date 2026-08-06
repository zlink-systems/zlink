using Microsoft.AspNetCore.Mvc;
using Microsoft.Extensions.Hosting;
using ResilienceLifecycle.Shared;
using Zlink.Framework.Contracts.Channels;
using Zlink.Framework.Contracts.Configuration;

namespace ResilienceLifecycle.Server.Provider;

internal static class ProviderEndpoints
{
    public static WebApplication MapProviderEndpoints(this WebApplication app, ServerOptions options)
    {
        app.MapGet("/health", () => Results.Ok(new { status = "ready", options.Role, options.Rid }));
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
        app.MapPost("/admin/profile/hold/{marker}", (
            string marker,
            [FromServices] FaultState fault,
            [FromServices] EvidenceStore evidence) =>
        {
            fault.HoldRequest(marker);
            evidence.Add($"admin|rid={evidence.Rid}|action=profile-hold|marker={marker}");
            return Results.Ok(new { status = "held", marker });
        });
        app.MapPost("/admin/profile/release/{marker}", (
            string marker,
            [FromServices] FaultState fault,
            [FromServices] EvidenceStore evidence) =>
        {
            if (!fault.ReleaseRequest(marker))
                return Results.NotFound(new { status = "not-held", marker });

            evidence.Add($"admin|rid={evidence.Rid}|action=profile-release|marker={marker}");
            return Results.Ok(new { status = "released", marker });
        });
        app.MapPost("/admin/weight/exclude", (
            [FromServices] IZLinkRouteMeshRuntimeOptions runtimeOptions,
            [FromServices] EvidenceStore evidence) =>
        {
            runtimeOptions.Channel(ResilienceLifecycleNames.Channel).Weight = 0;
            evidence.Add($"admin|rid={evidence.Rid}|action=weight-exclude|weight=0");
            return Results.Ok(new { status = "excluded", weight = 0 });
        });
        app.MapPost("/admin/weight/include", (
            [FromServices] IZLinkRouteMeshRuntimeOptions runtimeOptions,
            [FromServices] EvidenceStore evidence) =>
        {
            runtimeOptions.Channel(ResilienceLifecycleNames.Channel).Weight = 100;
            evidence.Add($"admin|rid={evidence.Rid}|action=weight-include|weight=100");
            return Results.Ok(new { status = "included", weight = 100 });
        });
        app.MapPost("/admin/graceful-drain", async (
            [FromServices] IZLinkFrameworkRuntime runtime,
            [FromServices] IHostApplicationLifetime applicationLifetime,
            CancellationToken cancellationToken) =>
        {
            var relocation = await runtime.RelocateAsync(
                new ZLinkFrameworkRelocationOptions
                {
                    Mode = ZLinkFrameworkRelocationMode.PlannedMaintenance,
                    Deadline = TimeSpan.FromSeconds(30)
                },
                cancellationToken);
            if (relocation.Outcome != ZLinkFrameworkRelocationOutcome.Relocated)
                return Results.Ok(new DrainResultRes(
                    relocation.Outcome.ToString(),
                    relocation.Reason.ToString()));
            var result = await runtime.ShutdownAsync(TimeSpan.FromSeconds(30), cancellationToken);
            var outcome = result.Outcome == ZLinkFrameworkTerminationOutcome.Stopped
                ? "Drained"
                : result.Outcome.ToString();
            applicationLifetime.StopApplication();
            return Results.Ok(new DrainResultRes(outcome, result.Reason.ToString()));
        });
        app.MapGet("/admin/weight", ([FromServices] IZLinkRouteMeshRuntimeOptions runtimeOptions) =>
        {
            var weight = runtimeOptions.Channel(ResilienceLifecycleNames.Channel).Weight;
            return Results.Ok(new { weight });
        });
        app.MapPost("/admin/weight/wait", async (
            WeightWaitReq request,
            [FromServices] IZLinkRouteMeshRuntimeOptions runtimeOptions,
            CancellationToken cancellationToken) =>
        {
            var timeout = TimeSpan.FromMilliseconds(Math.Clamp(request.TimeoutMilliseconds, 1, 30000));
            var deadline = DateTimeOffset.UtcNow + timeout;
            while (DateTimeOffset.UtcNow < deadline)
            {
                var weight = runtimeOptions.Channel(ResilienceLifecycleNames.Channel).Weight;
                if (weight == request.Expected) return Results.Ok(new { weight });

                await Task.Delay(TimeSpan.FromMilliseconds(50), cancellationToken);
            }

            return Results.Problem(
                $"Provider weight did not become {request.Expected}.",
                statusCode: StatusCodes.Status504GatewayTimeout);
        });
        app.MapPost("/admin/clientserver/weight/exclude", (
            [FromServices] IZLinkRouteMeshRuntimeOptions runtimeOptions,
            [FromServices] EvidenceStore evidence) =>
        {
            runtimeOptions.Channel(ResilienceLifecycleNames.ClientServerChannel).Weight = 0;
            evidence.Add($"admin|rid={evidence.Rid}|action=clientserver-weight-exclude|weight=0");
            return Results.Ok(new { status = "excluded", weight = 0 });
        });
        app.MapPost("/admin/clientserver/weight/include", (
            [FromServices] IZLinkRouteMeshRuntimeOptions runtimeOptions,
            [FromServices] EvidenceStore evidence) =>
        {
            runtimeOptions.Channel(ResilienceLifecycleNames.ClientServerChannel).Weight = 100;
            evidence.Add($"admin|rid={evidence.Rid}|action=clientserver-weight-include|weight=100");
            return Results.Ok(new { status = "included", weight = 100 });
        });
        app.MapPost("/admin/clientserver/weight/wait", async (
            WeightWaitReq request,
            [FromServices] IZLinkRouteMeshRuntimeOptions runtimeOptions,
            CancellationToken cancellationToken) =>
        {
            var timeout = TimeSpan.FromMilliseconds(Math.Clamp(request.TimeoutMilliseconds, 1, 30000));
            var deadline = DateTimeOffset.UtcNow + timeout;
            while (DateTimeOffset.UtcNow < deadline)
            {
                var weight = runtimeOptions.Channel(ResilienceLifecycleNames.ClientServerChannel).Weight;
                if (weight == request.Expected) return Results.Ok(new { weight });

                await Task.Delay(TimeSpan.FromMilliseconds(50), cancellationToken);
            }

            return Results.Problem(
                $"ClientServer weight did not become {request.Expected}.",
                statusCode: StatusCodes.Status504GatewayTimeout);
        });
        return app;
    }
}
