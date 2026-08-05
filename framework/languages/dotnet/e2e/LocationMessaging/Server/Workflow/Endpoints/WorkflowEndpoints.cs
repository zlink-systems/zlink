using Microsoft.AspNetCore.Builder;
using Microsoft.AspNetCore.Http;
using Microsoft.Extensions.Hosting;
using LocationMessaging.Server.Workflow.Configuration;
using LocationMessaging.Server.Workflow.Infrastructure;
using LocationMessaging.Shared;
using Zlink.Framework.Contracts.Channels;

namespace LocationMessaging.Server.Workflow.Endpoints;

internal static class WorkflowEndpoints
{
    public static void MapWorkflowEndpoints(this WebApplication app, ServerOptions options)
    {
        app.MapGet("/health", () => Results.Ok(new { status = "ready", options.Role, options.Rid }));
        app.MapGet("/evidence", (EvidenceStore evidence) => Results.Ok(evidence.Snapshot()));
        app.MapPost("/workflow/request", async (
            WorkflowReq request,
            IZLinkRouteClient channel) =>
        {
            var reply = await channel.RequestToChannel("workflow", request)
                .Timeout(TimeSpan.FromSeconds(5))
                .Async<WorkflowRes>();
            return Results.Ok(reply);
        });
        app.MapPost("/evidence/clear", (EvidenceStore evidence) =>
        {
            evidence.Clear();
            return Results.Ok(new { status = "cleared" });
        });
        app.MapPost("/evidence/wait", async (
            EvidenceWaitReq request,
            EvidenceStore evidence,
            CancellationToken cancellationToken) =>
        {
            var timeout = TimeSpan.FromMilliseconds(Math.Clamp(request.TimeoutMilliseconds, 1, 30000));
            var snapshot = await evidence.WaitUntilAsync(
                line => line.Contains(request.Contains, StringComparison.Ordinal),
                timeout,
                cancellationToken);
            return Results.Ok(snapshot);
        });
        app.MapPost("/shutdown", (IHostApplicationLifetime lifetime) =>
        {
            lifetime.StopApplication();
            return Results.Ok(new { status = "stopping" });
        });
    }

}
