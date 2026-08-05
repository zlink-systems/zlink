namespace PubSub.Server.Publisher.Endpoints;

using PubSub.Shared;
using Zlink.Framework.Contracts.Configuration;

public static class OperationalEndpoints
{
    public static WebApplication MapOperationalEndpoints(this WebApplication app, string role, string rid)
    {
        app.MapGet("/health", () => Results.Ok(new { status = "ready", role, rid }));
        app.MapGet("/evidence", (EvidenceStore evidence) => Results.Ok(evidence.Snapshot()));
        app.MapPost("/evidence/clear", (EvidenceStore evidence) =>
        {
            evidence.Clear();
            return Results.Ok(new { status = "cleared" });
        });
        app.MapPost("/admin/drain", async (
            IZLinkFrameworkRuntime runtime,
            CancellationToken cancellationToken) =>
        {
            var result = await runtime.ShutdownAsync(TimeSpan.FromSeconds(30), cancellationToken);
            return Results.Ok(new DrainResultRes(result.Outcome.ToString(), result.Reason.ToString()));
        });
        app.MapPost("/shutdown", (IHostApplicationLifetime lifetime) =>
        {
            lifetime.StopApplication();
            return Results.Ok(new { status = "stopping" });
        });
        return app;
    }
}
