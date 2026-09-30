using Microsoft.Extensions.DependencyInjection;
using Microsoft.Extensions.Hosting;
using Zlink.Framework.Contracts.Actors;

namespace ZLink.Framework.Perf;

// §10.9 actor-no-bind-request-echo. Question: what do address lookup and a remote request cost when a global ActorId
// is reached without any Session binding? Roles: HTTP Client x1 (trigger only), ActorCaller (Object Client, this
// file) x1, Actor (Object Server) x1. One operation: RequestToActor starts and ends when the typed echo has been
// validated. request, ordinary; 4096 bytes; one unbound Actor per logical stream, created during setup.
// Store: run Docker Redis. Null: physical connections, Spot, worker, fanout and actor.sourceAdmission (no send).
public sealed class ActorNoBindRequestEchoScenario(IZLinkActorClient actorClient, Measurement measurement, ActorCallerSetup setup, ObjectsReadiness readiness)
{
    private readonly RoleConfig config = measurement.Config;
    private long[] sequences = [];

    public static async Task RunAsync(RoleConfig config)
    {
        var builder = ServerApplication.Builder(config, options =>
        {
            var mesh = options.AddRouteMesh(config.meshName!).SetRoutingIdPrefix("perf-actor-caller").Listen(config.transportEndpoints["mesh"]);
            mesh.Objects().Client();
        });
        builder.Services.AddSingleton(new ObjectsReadiness(false, "Actors are not yet created and probed through the public API."));
        builder.Services.AddSingleton<ActorCallerSetup>();
        builder.Services.AddSingleton<ActorNoBindRequestEchoScenario>();
        var app = builder.Build();
        var scenario = app.Services.GetRequiredService<ActorNoBindRequestEchoScenario>();
        ServerApplication.Map(app, scenario.RunAsync);
        await app.StartAsync();
        await scenario.PrepareAsync(app.Lifetime.ApplicationStopping);
        await app.WaitForShutdownAsync();
    }

    public async Task PrepareAsync(CancellationToken stopping)
    {
        using var timeout = CancellationTokenSource.CreateLinkedTokenSource(stopping);
        timeout.CancelAfter(config.workload.setupTimeoutMs);
        try
        {
            var created = await setup.CreateActorsAsync(timeout.Token);
            sequences = new long[config.workload.logicalStreams!.Value];
            using var concurrency = new SemaphoreSlim(config.workload.connectConcurrency!.Value);   // §5: probes per prepared target
            await Task.WhenAll(Enumerable.Range(0, sequences.Length).Select(async stream =>
            {
                await concurrency.WaitAsync(timeout.Token);
                try
                {
                    var request = measurement.Request(stream, (ulong)Interlocked.Increment(ref sequences[stream]), probe: true);
                    var reply = await actorClient.RequestToActor(config.actorIds[stream], request)
                        .Timeout(TimeSpan.FromMilliseconds(config.workload.setupTimeoutMs)).Async<PerfEchoReply>(timeout.Token);
                    PayloadPattern.ValidateIdentity(request, reply);
                    measurement.Pattern.Validate(reply.payload);
                }
                finally { concurrency.Release(); }
            }));
            measurement.SetupEvidence = [new { kind = "typedProbeEcho", source = "IZLinkActorClient.RequestToActor.Async<PerfEchoReply>",
                observedValue = new { probes = sequences.Length, streams = sequences.Length } }];
            // §16.1: objectsReady means the create and the probe echo of every Actor are done, so warmup starts on quiet roles.
            readiness.Set(true, "", [created, ..measurement.SetupEvidence]);
        }
        catch (Exception error) { measurement.RecordDiagnostic(error); }
    }

    public Task RunAsync() => Task.WhenAll(Enumerable.Range(0, config.workload.logicalStreams!.Value)
        .SelectMany(stream => Enumerable.Range(0, config.workload.inflight).Select(_ => LoopAsync(stream))));

    private async Task LoopAsync(int stream)
    {
        var actorId = config.actorIds[stream];
        while (measurement.CanIssue)
        {
            var request = measurement.Request(stream, checked((ulong)Interlocked.Increment(ref sequences[stream])));
            if (!measurement.BeginOperation(out var started)) break;
            request = request with { sentTicks = DecimalText.Of(started) };
            try
            {
                var reply = await actorClient.RequestToActor(actorId, request)
                    .Timeout(TimeSpan.FromMilliseconds(config.workload.requestTimeoutMs)).Async<PerfEchoReply>();
                PayloadPattern.ValidateIdentity(request, reply);
                measurement.Pattern.Validate(reply.payload);
                measurement.CompleteOperation(started);
            }
            catch (Exception error) { measurement.CompleteOperation(started, error); }
        }
    }
}
