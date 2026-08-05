using Microsoft.AspNetCore.Builder;
using Microsoft.AspNetCore.Http;

namespace ObservabilityOps.Server.Support;

public sealed class SpotClosingGate
{
    private readonly object _sync = new();
    private Generation? _generation;

    public void Arm(TimeSpan maximumWait)
    {
        if (maximumWait <= TimeSpan.Zero)
            throw new ArgumentOutOfRangeException(nameof(maximumWait));
        lock (_sync)
        {
            if (_generation is { Released.Task.IsCompleted: false })
                throw new InvalidOperationException(
                    "The Spot closing gate is already armed.");
            _generation = new Generation(maximumWait);
        }
    }

    public async ValueTask EnterAsync(CancellationToken cancellationToken)
    {
        Generation generation;
        lock (_sync)
            generation = _generation
                ?? throw new InvalidOperationException(
                    "The Spot closing gate is not armed.");
        generation.Started.TrySetResult();
        await generation.Released.Task.WaitAsync(
            generation.MaximumWait,
            cancellationToken);
    }

    public async Task WaitUntilStartedAsync(
        TimeSpan timeout,
        CancellationToken cancellationToken)
    {
        Generation generation;
        lock (_sync)
            generation = _generation
                ?? throw new InvalidOperationException(
                    "The Spot closing gate is not armed.");
        await generation.Started.Task.WaitAsync(timeout, cancellationToken);
    }

    public void Release()
    {
        lock (_sync)
            (_generation ?? throw new InvalidOperationException(
                "The Spot closing gate is not armed."))
            .Released.TrySetResult();
    }

    private sealed class Generation(TimeSpan maximumWait)
    {
        public TimeSpan MaximumWait { get; } = maximumWait;
        public TaskCompletionSource Started { get; } =
            new(TaskCreationOptions.RunContinuationsAsynchronously);
        public TaskCompletionSource Released { get; } =
            new(TaskCreationOptions.RunContinuationsAsynchronously);
    }
}

public static class SpotClosingGateEndpoints
{
    public static WebApplication MapSpotClosingGate(this WebApplication app)
    {
        app.MapPost("/closing-gate/arm", (
            int? maximumWaitMs,
            SpotClosingGate gate) =>
        {
            gate.Arm(TimeSpan.FromMilliseconds(
                Math.Clamp(maximumWaitMs ?? 30000, 1, 60000)));
            return Results.Ok();
        });
        app.MapPost("/closing-gate/wait-started", async (
            int? timeoutMs,
            SpotClosingGate gate,
            CancellationToken cancellationToken) =>
        {
            await gate.WaitUntilStartedAsync(
                TimeSpan.FromMilliseconds(
                    Math.Clamp(timeoutMs ?? 10000, 1, 30000)),
                cancellationToken);
            return Results.Ok();
        });
        app.MapPost("/closing-gate/release", (SpotClosingGate gate) =>
        {
            gate.Release();
            return Results.Ok();
        });
        return app;
    }
}
