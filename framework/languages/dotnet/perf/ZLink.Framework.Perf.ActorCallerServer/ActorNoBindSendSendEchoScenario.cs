using Microsoft.Extensions.DependencyInjection;
using Microsoft.Extensions.Hosting;
using Zlink.Framework.Contracts.Actors;
using Zlink.Framework.Contracts.Handlers;

namespace ZLink.Framework.Perf;

// §10.10 actor-no-bind-send-send-echo. Question: what do the source admission of a global-ActorId send and the
// application echo round trip cost, measured apart? Roles: HTTP Client x1 (trigger only), ActorCaller (Object Client
// plus the Server of a run/cell-only return ChannelName, this file) x1, Actor (Object Server) x1.
// One operation: the correlation is registered and SendToActor starts; it ends when the return Channel handler
// validates the echo (§13). Separately, actor.sourceAdmission.* is the same call's start to the SendToActor terminal.
// send-send, ordinary; 4096 bytes; one unbound Actor per logical stream. Store: run Docker Redis.
// Null: physical connections, Spot, worker, fanout; the remote mailbox acceptance time is not publicly observable.
public sealed class ActorNoBindSendSendEchoScenario(IZLinkActorClient actorClient, Measurement measurement, ActorCallerSetup setup, ObjectsReadiness readiness,
    SendSendCorrelation correlations, ScenarioMetrics metrics)
{
    private readonly RoleConfig config = measurement.Config;
    private long[] sequences = [];

    public static async Task RunAsync(RoleConfig config)
    {
        var builder = ServerApplication.Builder(config, options =>
        {
            var mesh = options.AddRouteMesh(config.meshName!).SetRoutingIdPrefix("perf-actor-caller").Listen(config.transportEndpoints["mesh"]);
            mesh.Objects().Client();
            mesh.Channel(config.channelName!).Server().AddSendHandler<ActorReturnHandler, PerfEchoReply>();
        });
        builder.Services.AddSingleton(new ObjectsReadiness(false, "Actors are not yet created and probed through the public API."));
        builder.Services.AddSingleton(sp => new ScenarioMetrics(sp.GetRequiredService<Measurement>()).Latency("sourceAdmissionMs", "actor.sourceAdmission.latency"));
        builder.Services.AddSingleton<SendSendCorrelation>();
        builder.Services.AddSingleton<ActorCallerSetup>();
        builder.Services.AddSingleton<ActorNoBindSendSendEchoScenario>();
        var app = builder.Build();
        var scenario = app.Services.GetRequiredService<ActorNoBindSendSendEchoScenario>();
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
                    var request = measurement.Request(stream, (ulong)Interlocked.Increment(ref sequences[stream]), probe: true)
                        with { returnChannel = config.channelName };
                    var entry = correlations.Register(request, PerfClock.Now);
                    await actorClient.SendToActor(config.actorIds[stream], request).Async(timeout.Token);
                    var (error, _) = await correlations.CompleteAsync(entry);
                    if (error is not null) throw error;
                }
                finally { concurrency.Release(); }
            }));
            measurement.SetupEvidence = [new { kind = "typedProbeEcho", source = "IZLinkActorClient.SendToActor -> return Channel send handler",
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
            var request = measurement.Request(stream, checked((ulong)Interlocked.Increment(ref sequences[stream])))
                with { returnChannel = config.channelName };
            if (!measurement.BeginOperation(out var started, "send")) break;
            request = request with { sentTicks = DecimalText.Of(started) };
            var entry = correlations.Register(request, started);   // §13: registered right before the first public send
            try
            {
                await actorClient.SendToActor(actorId, request).Async();
                var admitted = PerfClock.Now;
                metrics.Record("sourceAdmissionMs", started, admitted);
                correlations.FirstSendEnded(entry, null);
            }
            catch (Exception error) { correlations.FirstSendEnded(entry, error); }
            var (result, completed) = await correlations.CompleteAsync(entry);   // the return Channel handler decides
            measurement.CompleteOperation(started, result, completed);
        }
    }
}

// The return Channel handler of this caller: the Actor's echo arrives as a second one-way send.
public sealed class ActorReturnHandler(SendSendCorrelation correlations) : IZLinkSendHandler<PerfEchoReply>
{
    public ValueTask HandleAsync(PerfEchoReply message, IZLinkMessageContext context, CancellationToken cancellationToken)
    {
        correlations.Reply(message);
        return ValueTask.CompletedTask;
    }
}
