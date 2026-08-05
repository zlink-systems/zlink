using PubSub.Shared;

namespace PubSub.Server.Subscriber;

public static class OperationalEndpoints
{
    public static WebApplication MapOperationalEndpoints(this WebApplication app, string role, string rid)
    {
        app.MapGet("/health", () => Results.Ok(new { status = "ready", role, rid }));
        app.MapGet("/evidence", (EvidenceStore evidence) => Results.Ok(evidence.Snapshot()));
        app.MapPost("/evidence/wait", async (
            EvidenceWaitReq request,
            EvidenceStore evidence,
            CancellationToken cancellationToken) =>
        {
            var timeout = TimeSpan.FromMilliseconds(Math.Clamp(request.TimeoutMilliseconds, 1, 30000));
            var snapshot = await evidence.WaitUntilAsync(
                entries => request.ContainsAll.All(expected =>
                               entries.Skip(request.AfterIndex)
                                   .Any(entry => entry.Contains(expected, StringComparison.Ordinal)))
                           && request.ContainsAnyGroups.All(group =>
                               group.Any(expected =>
                                   entries.Skip(request.AfterIndex)
                                       .Any(entry => entry.Contains(expected, StringComparison.Ordinal))))
                           && request.ContainsAllLineGroups.All(group =>
                               entries.Skip(request.AfterIndex).Any(entry =>
                                   group.All(expected => entry.Contains(expected, StringComparison.Ordinal))))
                           && (request.ContainsAnyLineGroups.Length == 0
                               || request.ContainsAnyLineGroups.Any(group =>
                                   entries.Skip(request.AfterIndex).Any(entry =>
                                       group.All(expected => entry.Contains(expected, StringComparison.Ordinal))))),
                timeout,
                cancellationToken);
            return Results.Ok(snapshot.Skip(request.AfterIndex).ToArray());
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
        return app;
    }
}
