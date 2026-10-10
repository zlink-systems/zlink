using Microsoft.Extensions.DependencyInjection;
using Microsoft.Extensions.Hosting;
using Zlink.Framework.Contracts.Channels;
using Zlink.Framework.Contracts.Configuration;
using Zlink.Framework.Contracts.Spots;

namespace ZLink.Framework.Perf;

// §10.8 spot-worker-offload-echo. Question: how much a CPU worker call and the delivery of its result add to the
// local Spot echo of §10.7. Roles: HTTP Client x1, Spot process (Object Server + local driver + Framework worker,
// this file) x1; no remote echo process. The caller is the same local IZLinkSpotClient.RequestToSpot as §10.7 and its
// interval is the primary latency; the Spot handler runs one IZLinkSpotContext.RunCpuWorker call per request:
// Yield (default) hands the turn back while the worker runs, ordinary keeps it. The worker callback is the
// self-contained xorshift32-v1 task of worker-task-millis (no sleep); it checks time and cancellation every 1024
// iterations. Public worker options carry min/max threads and idle timeout; worker calls use the phase deadline.
// worker-offload; payload 1024 bytes. Store: run Docker Redis (Spot addresses). Null: worker queue depth and Spot
// internals have no public observation; remote call, Actor, fanout do not apply.
public sealed class SpotWorkerOffloadEchoScenario(
    IZLinkSpotClient spots,
    IZLinkSpotManager manager,
    Measurement measurement,
    IZLinkRouteMeshRuntime meshRuntime,
    ObjectsReadiness readiness
)
{
    private readonly RoleConfig config = measurement.Config;
    private long[] sequences = [];

    public static async Task RunAsync(RoleConfig config)
    {
        if (config.role != "spot" || !config.source || config.worker is null)
            throw new ArgumentException(
                "The Spot role with worker config is the source of this scenario."
            );
        var worker = config.worker;
        var builder = SpotRole.Builder<SpotWorkerOffloadSpot>(
            config,
            callsChannel: false,
            options =>
            {
                // §5.2: only the public worker options; the .NET options carry no queue length.
                options.Worker.MinThreads = worker.minThreads;
                options.Worker.MaxThreads = worker.maxThreads;
                options.Worker.IdleTimeout = TimeSpan.FromMilliseconds(worker.idleTimeoutMs);
            }
        );
        builder.Services.AddSingleton(sp =>
            new ScenarioMetrics(sp.GetRequiredService<Measurement>())
                .Counters("spot.applicationHandlerEntries", "spot.applicationYieldCalls")
                .Latency("workerCallLatencyMs", "worker.callLatency")
                .Latency("workerSubmitToStartMs", "worker.submitToStart")
                .Latency("workerTaskLatencyMs", "worker.taskLatency")
                .Latency("workerResultToContinuationMs", "worker.resultToContinuation")
                .Unsupported(
                    "PUBLIC_OBSERVATION_UNSUPPORTED",
                    "Public worker options are settings; no queue depth snapshot exists.",
                    "worker.pool.queueDepth.max",
                    "worker.pool.queueDepth.mean"
                )
                .SpotInternalsUnsupported()
                .Provenance(
                    "workerOptions",
                    new
                    {
                        algorithm = worker.algorithm,
                        taskMillis = worker.taskMillis,
                        applied = new
                        {
                            worker.minThreads,
                            worker.maxThreads,
                            worker.idleTimeoutMs,
                        },
                        callDeadlineRule = "phaseEnd+drainTimeoutMs",
                    }
                )
        );
        builder.Services.AddSingleton<SpotWorkerOffloadEchoScenario>();
        var app = builder.Build();
        _ = app.Services.GetRequiredService<ScenarioMetrics>();
        var scenario = app.Services.GetRequiredService<SpotWorkerOffloadEchoScenario>();
        ServerApplication.Map(app, scenario.RunAsync);
        await app.StartAsync();
        await scenario.PrepareAsync(app.Lifetime.ApplicationStopping);
        await app.WaitForShutdownAsync();
    }

    public async Task PrepareAsync(CancellationToken stopping)
    {
        var objects = await SpotRole.CreateSpotsAsync(
            config,
            manager,
            meshRuntime,
            measurement,
            stopping
        );
        if (objects is null)
            return;
        try
        {
            sequences = new long[config.workload.logicalStreams!.Value];
            List<object> probes = [];
            for (var target = 0; target < config.spotIds.Length; target++)
            {
                var request = measurement.Request(
                    target,
                    (ulong)Interlocked.Increment(ref sequences[target % sequences.Length]),
                    probe: true
                );
                var reply = await spots
                    .RequestToSpot(config.spotIds[target], request)
                    .Timeout(measurement.CallTimeout())
                    .Async<PerfEchoReply>();
                PayloadPattern.ValidateIdentity(request, reply);
                measurement.Pattern.Validate(reply.payload);
                probes.Add(
                    new
                    {
                        request.correlationId,
                        reply.receivedTicks,
                        reply.clockDomainId,
                    }
                );
            }
            SpotRole.Publish(readiness, objects); // objectsReady only after every probe, so warmup never overlaps one
            measurement.SetupEvidence =
            [
                new
                {
                    kind = "typedProbeEcho",
                    source = "IZLinkSpotClient.RequestToSpot -> RunCpuWorker",
                    observedValue = probes,
                },
            ];
        }
        catch (Exception error)
        {
            measurement.RecordDiagnostic(error);
        }
    }

    public Task RunAsync() =>
        ServerDrivenStreams.RunTerminalStreamsAsync(
            measurement,
            config.workload.logicalStreams!.Value,
            LoopAsync
        );

    private async Task LoopAsync(int stream)
    {
        var spotId = config.spotIds[stream % config.spotIds.Length];
        if (!measurement.BeginOperation(out var started))
            return;
        var request = measurement.Request(
            stream,
            checked((ulong)Interlocked.Increment(ref sequences[stream]))
        ) with
        {
            sentTicks = DecimalText.Of(started),
        };
        try
        {
            var reply = await spots
                .RequestToSpot(spotId, request)
                .Timeout(measurement.CallTimeout())
                .Async<PerfEchoReply>();
            PayloadPattern.ValidateIdentity(request, reply);
            measurement.Pattern.Validate(reply.payload);
            measurement.CompleteOperation(started);
        }
        catch (Exception error)
        {
            measurement.CompleteOperation(started, error);
        }
    }
}

public sealed class SpotWorkerOffloadSpot(IZLinkSpotContext context) : IZLinkSpot
{
    public IZLinkSpotContext Context { get; } = context;

    public void Configure() => Context.Handlers.AddPacket<SpotWorkerOffloadHandler>();
}

// The Spot handler: one CPU worker call, then the typed echo. The worker callback returns its own timing evidence.
public sealed class SpotWorkerOffloadHandler(
    Measurement measurement,
    ScenarioMetrics metrics,
    RoleConfig config
) : IZLinkSpotRequestHandler<SpotWorkerOffloadSpot, PerfEchoRequest, PerfEchoReply>
{
    public async ValueTask<PerfEchoReply> HandleAsync(
        SpotWorkerOffloadSpot spot,
        PerfEchoRequest request,
        CancellationToken cancellationToken
    )
    {
        var received = PerfClock.Now;
        measurement.HandlerEnter();
        try
        {
            measurement.ValidateRequest(request);
            if (request.phase == "measured")
                metrics.Count("spot.applicationHandlerEntries");
            var taskMillis = config.worker!.taskMillis;
            var submitted = PerfClock.Now;
            var call = spot
                .Context.RunCpuWorker(worker => XorShift32(taskMillis, worker))
                .Timeout(measurement.CallTimeout());
            WorkerObservation observation;
            if (config.terminal == "yield")
            {
                metrics.Count("spot.applicationYieldCalls");
                observation = await call.Yield(cancellationToken);
            }
            else
                observation = await call.Async(cancellationToken);
            var resumed = PerfClock.Now;
            RecordWorker(request, observation, submitted, resumed);
            var reply = PayloadPattern.Reply(request, received);
            measurement.RecordReply(request);
            if (measurement.Phase == "setup" && !measurement.Config.source)
                measurement.SetupEvidence =
                [
                    new
                    {
                        kind = "typedProbeReply",
                        source = "IZLinkSpotRequestHandler -> RunCpuWorker",
                        observedValue = new
                        {
                            request.correlationId,
                            observation.iterations,
                            observation.checksum,
                        },
                    },
                ];
            return reply;
        }
        catch (Exception error)
        {
            measurement.RecordDiagnostic(error);
            throw;
        }
        finally
        {
            measurement.HandlerExit();
        }
    }

    // §10.8 xorshift32-v1: x=0x12345678; x^=x<<13; x^=x>>>17; x^=x<<5 in 32 bits. Time and cancellation are checked
    // every 1024 iterations and the task ends at or after the target duration. It uses no sleep and no shared state.
    private static WorkerObservation XorShift32(int taskMillis, CancellationToken cancellation)
    {
        var started = PerfClock.Now;
        var target = started + taskMillis * 1_000_000L;
        uint x = 0x12345678;
        ulong iterations = 0;
        do
        {
            for (var i = 0; i < 1024; i++)
            {
                x ^= x << 13;
                x ^= x >> 17;
                x ^= x << 5;
            }
            iterations += 1024;
            cancellation.ThrowIfCancellationRequested();
        } while (PerfClock.Now < target);
        return new WorkerObservation(
            DecimalText.Of(started),
            DecimalText.Of(PerfClock.Now),
            PerfClock.Domain,
            DecimalText.Of(iterations),
            x
        );
    }

    // The intervals of one worker call in the window. They are taken from one process clock; an observation from
    // another clock domain would be discarded rather than subtracted.
    private void RecordWorker(
        PerfEchoRequest request,
        WorkerObservation observation,
        long submitted,
        long resumed
    )
    {
        if (request.phase != "measured")
            return;
        if (
            observation.clockDomainId != PerfClock.Domain
            || DecimalText.U64(observation.iterations) == 0
        )
            throw new PerfValidationException(
                "SchemaMismatch",
                "The worker observation is not from this clock domain or is empty."
            );
        var started = DecimalText.I64(observation.startedTicks);
        var ended = DecimalText.I64(observation.endedTicks);
        metrics.Record("workerCallLatencyMs", submitted, resumed);
        metrics.Record("workerSubmitToStartMs", submitted, started, resumed);
        metrics.Record("workerTaskLatencyMs", started, ended, resumed);
        metrics.Record("workerResultToContinuationMs", ended, resumed, resumed);
    }
}
