using System.Diagnostics;
using System.Text.Json;
using System.Text.Json.Serialization;
using Microsoft.AspNetCore.Builder;
using Microsoft.AspNetCore.Hosting;
using Microsoft.AspNetCore.Http;
using Microsoft.Extensions.Logging;

namespace ZLink.Framework.Perf;

public sealed record BenchTriggerRequest
{
    public required string runId { get; init; }
    public required string cellId { get; init; }
    public required string pattern { get; init; }
    public required int payloadBytes { get; init; }
    public required string phase { get; init; }
    public required int durationMs { get; init; }
    public required int requestWindow { get; init; }
    public required int sendConcurrency { get; init; }
}

public sealed record BenchTriggerObservation(
    string runId,
    string cellId,
    string pattern,
    int payloadBytes,
    string phase,
    int durationMs,
    int requestWindow,
    int sendConcurrency,
    long receivedAtUnixMs);

public sealed record BenchTriggerReply(
    bool accepted,
    string runId,
    string cellId,
    string phase,
    long startedAt,
    string? reason = null);

public sealed record BenchControlSnapshot(
    bool ready,
    string phase,
    long submitted,
    long completed,
    long errors,
    long received,
    long currentInFlight,
    long peakInFlight,
    string? failure);

public sealed record BenchCounterSnapshot(
    long submitted,
    long completed,
    long errors,
    long received,
    long currentInFlight,
    long peakInFlight);

/// <summary>
/// Owns the common server-driven benchmark trigger contract. Transport-specific
/// sources supply only readiness, counters, and the workload delegate.
/// </summary>
public sealed class BenchPhaseController(
    Func<bool> isReady,
    Func<BenchCounterSnapshot> counters,
    Func<BenchTriggerRequest, CancellationToken, Task> workload)
{
    private static readonly JsonSerializerOptions Json = CreateJson();
    private readonly object gate = new();
    private readonly Dictionary<string, BenchTriggerReply> acknowledgements = new(StringComparer.Ordinal);
    private string phase = "idle";
    private string? failure;
    private CancellationToken stopping;

    public BenchTriggerObservation? LastTrigger { get; private set; }

    internal static JsonSerializerOptions JsonOptions => Json;

    public void SetStoppingToken(CancellationToken cancellationToken) => stopping = cancellationToken;

    public (BenchTriggerReply Reply, int StatusCode) Start(BenchTriggerRequest request)
    {
        Validate(request);
        lock (gate)
        {
            var key = $"{request.runId}/{request.cellId}/{request.phase}";
            if (acknowledgements.TryGetValue(key, out var existing))
            {
                return (existing, StatusCodes.Status200OK);
            }

            if (!isReady())
            {
                return (Reject(request, "Source is not ready."), StatusCodes.Status409Conflict);
            }

            if (phase != "idle")
            {
                return (Reject(request, $"Phase {phase} has not completed."), StatusCodes.Status409Conflict);
            }

            phase = request.phase;
            failure = null;
            var startedAt = NowNs();
            LastTrigger = new(
                request.runId,
                request.cellId,
                request.pattern,
                request.payloadBytes,
                request.phase,
                request.durationMs,
                request.requestWindow,
                request.sendConcurrency,
                DateTimeOffset.UtcNow.ToUnixTimeMilliseconds());
            var reply = new BenchTriggerReply(
                true,
                request.runId,
                request.cellId,
                request.phase,
                startedAt);
            acknowledgements.Add(key, reply);
            _ = Task.Run(() => RunAsync(request), CancellationToken.None);
            return (reply, StatusCodes.Status200OK);
        }
    }

    public BenchControlSnapshot Snapshot()
    {
        var values = counters();
        lock (gate)
        {
            return new BenchControlSnapshot(
                isReady(),
                phase,
                values.submitted,
                values.completed,
                values.errors,
                values.received,
                values.currentInFlight,
                values.peakInFlight,
                failure);
        }
    }

    private async Task RunAsync(BenchTriggerRequest request)
    {
        try
        {
            await workload(request, stopping).ConfigureAwait(false);
            lock (gate) phase = "idle";
        }
        catch (Exception error)
        {
            lock (gate)
            {
                failure = $"{error.GetType().FullName}: {error.Message}";
                phase = "failed";
            }
        }
    }

    private static BenchTriggerReply Reject(BenchTriggerRequest request, string reason) =>
        new(false, request.runId, request.cellId, request.phase, NowNs(), reason);

    private static void Validate(BenchTriggerRequest request)
    {
        if (string.IsNullOrWhiteSpace(request.runId) || string.IsNullOrWhiteSpace(request.cellId))
            throw new JsonException("runId and cellId must be non-empty strings.");
        if (request.pattern is not ("request-serial" or "request-window" or "request-backpressure" or "send-saturation"))
            throw new JsonException("Unknown benchmark pattern.");
        if (request.phase is not ("warmup" or "active"))
            throw new JsonException("phase must be warmup or active.");
        if (request.payloadBytes < 29 || request.durationMs <= 0 || request.requestWindow <= 0 || request.sendConcurrency <= 0)
            throw new JsonException("payloadBytes, durationMs, requestWindow, and sendConcurrency must be positive.");
    }

    private static long NowNs() =>
        (long)(Stopwatch.GetTimestamp() * (1_000_000_000.0 / Stopwatch.Frequency));

    private static JsonSerializerOptions CreateJson() => new(JsonSerializerDefaults.Web)
    {
        PropertyNamingPolicy = JsonNamingPolicy.CamelCase,
        PropertyNameCaseInsensitive = false,
        UnmappedMemberHandling = JsonUnmappedMemberHandling.Disallow
    };
}

/// <summary>Maps the shared benchmark trigger/admin endpoints onto ASP.NET Core.</summary>
public static class BenchHttpApplication
{
    public static WebApplicationBuilder Builder(params string[] urls)
    {
        var builder = WebApplication.CreateBuilder(Array.Empty<string>());
        builder.Logging.ClearProviders();
        builder.Logging.AddConsole();
        builder.Logging.SetMinimumLevel(LogLevel.Warning);
        builder.WebHost.UseUrls(urls);
        return builder;
    }

    public static void MapSource(
        WebApplication app,
        string triggerUrl,
        string statsUrl,
        BenchPhaseController controller)
    {
        var triggerPort = new Uri(triggerUrl).Port;
        var statsPort = new Uri(statsUrl).Port;
        controller.SetStoppingToken(app.Lifetime.ApplicationStopping);
        RestrictPorts(app, triggerPort, statsPort);
        app.MapPost("/bench/start", async (HttpContext context) =>
        {
            try
            {
                var request = await JsonSerializer.DeserializeAsync<BenchTriggerRequest>(
                    context.Request.Body,
                    BenchPhaseController.JsonOptions,
                    context.RequestAborted).ConfigureAwait(false)
                    ?? throw new JsonException("JSON null is not a trigger request.");
                var outcome = controller.Start(request);
                return Results.Json(outcome.Reply, BenchPhaseController.JsonOptions, statusCode: outcome.StatusCode);
            }
            catch (JsonException error)
            {
                return Results.Json(new { reason = error.Message }, statusCode: StatusCodes.Status400BadRequest);
            }
        });
        app.MapGet("/bench/stats", () => Results.Json(controller.Snapshot(), BenchPhaseController.JsonOptions));
    }

    private static void RestrictPorts(WebApplication app, int triggerPort, int statsPort)
    {
        app.Use(async (context, next) =>
        {
            var expected = context.Request.Path.StartsWithSegments("/bench/start")
                ? triggerPort
                : statsPort;
            if (context.Connection.LocalPort != expected)
            {
                context.Response.StatusCode = StatusCodes.Status404NotFound;
                return;
            }
            await next().ConfigureAwait(false);
        });
    }
}
