using Microsoft.Extensions.Configuration;

using AutomaticTurnDispatch.Shared;
using Zlink.Framework.E2E.Configuration;

var options = E2eConfiguration.Load<ExternalApiOptions>(args);
var builder = WebApplication.CreateBuilder(args);
        builder.Configuration.Sources.Clear();
        builder.Configuration.AddInMemoryCollection();
builder.WebHost.UseUrls(options.HttpUrl);
var app = builder.Build();
app.MapGet("/health", () => Results.Ok(new { status = "ready", role = "external-api" }));
app.MapGet("/delay", async (string requestId, string marker, int delayMs, CancellationToken cancellationToken) =>
{
    await Task.Delay(TimeSpan.FromMilliseconds(delayMs), cancellationToken);
    return Results.Ok(new ExternalDelayRes(requestId, marker));
});
await app.RunAsync();

internal sealed record ExternalApiOptions(string HttpUrl);
