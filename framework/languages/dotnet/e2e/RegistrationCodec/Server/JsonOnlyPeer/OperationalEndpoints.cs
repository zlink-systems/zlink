using RegistrationCodec.Server.JsonOnlyPeer.Infrastructure;
using RegistrationCodec.Shared;

namespace RegistrationCodec.Server.JsonOnlyPeer;

internal static class OperationalEndpoints
{
    public static WebApplication MapOperationalEndpoints(this WebApplication app, ServerOptions options)
    {
        app.MapGet("/health", () => Results.Ok(new { status = "ready", options.Rid }));
        app.MapGet("/evidence", (EvidenceStore evidence) => Results.Ok(evidence.Snapshot()));
        app.MapPost("/evidence/wait", async (
            EvidenceWaitReq request,
            EvidenceStore evidence,
            CancellationToken cancellationToken) =>
        {
            var timeout = TimeSpan.FromMilliseconds(Math.Clamp(request.TimeoutMilliseconds, 1, 30000));
            var snapshot = await evidence.WaitUntilAsync(
                entries => request.ContainsAll.All(expected =>
                    entries.Any(entry => entry.Contains(expected, StringComparison.Ordinal))),
                timeout,
                cancellationToken);
            return Results.Ok(snapshot);
        });
        app.MapPost("/evidence/clear", (EvidenceStore evidence) =>
        {
            evidence.Clear();
            return Results.Ok(new { status = "cleared" });
        });
        return app;
    }
}