using Microsoft.AspNetCore.Builder;
using Microsoft.AspNetCore.Http;

namespace ObservabilityOps.Server.Support;

public sealed class BoundedOperationGate
{
    private readonly object _gate = new();
    private GateGeneration? _generation;

    public void Arm(TimeSpan maximumWait)
    {
        if (maximumWait <= TimeSpan.Zero)
            throw new ArgumentOutOfRangeException(nameof(maximumWait));
        lock (_gate)
        {
            if (_generation is { Released.Task.IsCompleted: false })
                throw new InvalidOperationException("The operation gate is already armed.");
            _generation = new GateGeneration(maximumWait);
        }
    }

    public async ValueTask EnterAsync(CancellationToken cancellationToken)
    {
        GateGeneration generation;
        lock (_gate)
            generation = _generation
                ?? throw new InvalidOperationException("The operation gate is not armed.");
        generation.Started.TrySetResult();
        await generation.Released.Task.WaitAsync(generation.MaximumWait, cancellationToken);
    }

    public async Task WaitUntilStartedAsync(TimeSpan timeout, CancellationToken cancellationToken)
    {
        GateGeneration generation;
        lock (_gate)
            generation = _generation
                ?? throw new InvalidOperationException("The operation gate is not armed.");
        await generation.Started.Task.WaitAsync(timeout, cancellationToken);
    }

    public void Release()
    {
        lock (_gate)
            (_generation ?? throw new InvalidOperationException("The operation gate is not armed."))
                .Released.TrySetResult();
    }

    private sealed class GateGeneration(TimeSpan maximumWait)
    {
        public TimeSpan MaximumWait { get; } = maximumWait;
        public TaskCompletionSource Started { get; } = new(TaskCreationOptions.RunContinuationsAsynchronously);
        public TaskCompletionSource Released { get; } = new(TaskCreationOptions.RunContinuationsAsynchronously);
    }
}

public static class BoundedOperationGateEndpoints
{
    public static WebApplication MapBoundedOperationGate(this WebApplication app)
    {
        app.MapPost("/operation-gate/arm", (int? maximumWaitMs, BoundedOperationGate gate) =>
        {
            gate.Arm(TimeSpan.FromMilliseconds(Math.Clamp(maximumWaitMs ?? 30000, 1, 60000)));
            return Results.Ok();
        });
        app.MapPost("/operation-gate/wait-started", async (
            int? timeoutMs,
            BoundedOperationGate gate,
            CancellationToken cancellationToken) =>
        {
            await gate.WaitUntilStartedAsync(
                TimeSpan.FromMilliseconds(Math.Clamp(timeoutMs ?? 10000, 1, 30000)),
                cancellationToken);
            return Results.Ok();
        });
        app.MapPost("/operation-gate/release", (BoundedOperationGate gate) =>
        {
            gate.Release();
            return Results.Ok();
        });
        return app;
    }
}
