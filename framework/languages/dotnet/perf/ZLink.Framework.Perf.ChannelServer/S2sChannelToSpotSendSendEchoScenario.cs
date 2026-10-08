using Microsoft.Extensions.DependencyInjection;
using Microsoft.Extensions.Hosting;
using Zlink.Framework.Contracts.Channels;
using Zlink.Framework.Contracts.Configuration;
using Zlink.Framework.Contracts.Handlers;
using Zlink.Framework.Contracts.Spots;

namespace ZLink.Framework.Perf;

// §10.4 s2s-channel-to-spot-send-send-echo. Question: how completion rate, throughput and round trip differ from the
// request form of §10.3 when both directions are one-way sends. Roles: HTTP Client x1, Channel process (Object Client
// plus the Server of a run/cell-only return ChannelName, this file) x1, Spot process (Object Server) x1.
// One operation: the correlation is registered and the first SendToSpot starts; it ends when the return Channel
// handler validates the echo (§13). send-send, ordinary; payload 4096 bytes; streamId mod spotCount picks the Spot.
// Store: run Docker Redis. Null: physical connections, worker, Actor, fanout; Spot internals are not observable.
public sealed class S2sChannelToSpotSendSendEchoScenario(
    IZLinkSpotClient spots,
    IZLinkSpotManager manager,
    Measurement measurement,
    IZLinkRouteMeshRuntime meshRuntime,
    ObjectsReadiness readiness,
    SendSendCorrelation correlations
)
{
    private readonly RoleConfig config = measurement.Config;
    private long[] sequences = [];

    public static async Task RunAsync(RoleConfig config)
    {
        if (config.role != "channel" || !config.source)
            throw new ArgumentException("The Channel role is the source of this scenario.");
        var builder = ServerApplication.Builder(
            config,
            options =>
            {
                var mesh = options
                    .AddRouteMesh(config.meshName!)
                    .SetRoutingIdPrefix("perf-channel")
                    .Listen(config.transportEndpoints["mesh"]);
                mesh.Objects().Client();
                mesh.Channel(config.channelName!)
                    .Server()
                    .AddSendHandler<S2sReturnHandler, PerfEchoReply>();
            }
        );
        builder.Services.AddSingleton(
            new ObjectsReadiness(
                false,
                "No User Spot has been found through the public manager yet."
            )
        );
        builder.Services.AddSingleton(sp =>
            new ScenarioMetrics(sp.GetRequiredService<Measurement>()).SpotInternalsUnsupported()
        );
        builder.Services.AddSingleton<SendSendCorrelation>();
        builder.Services.AddSingleton<S2sChannelToSpotSendSendEchoScenario>();
        var app = builder.Build();
        var scenario = app.Services.GetRequiredService<S2sChannelToSpotSendSendEchoScenario>();
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
            while (
                meshRuntime.GetStatus(config.meshName!)
                    is not { IsReady: true, ReadyPeerCount: > 0 }
            )
                await Task.Delay(10, timeout.Token);
            List<object> found = [];
            foreach (var spotId in config.spotIds)
            {
                SpotRef? spot;
                while ((spot = await manager.FindAsync(spotId, timeout.Token)) is null)
                    await Task.Delay(10, timeout.Token);
                found.Add(
                    new
                    {
                        spotId,
                        spot.Value.MeshName,
                        nodeRid = spot.Value.NodeRid.ToHex(),
                    }
                );
            }
            sequences = new long[config.workload.logicalStreams!.Value];
            List<object> probes = [];
            for (var target = 0; target < config.spotIds.Length; target++)
            {
                var request = measurement.Request(
                    target,
                    (ulong)Interlocked.Increment(ref sequences[target % sequences.Length]),
                    probe: true
                ) with
                {
                    returnChannel = config.channelName,
                };
                var entry = correlations.Register(request, PerfClock.Now);
                await spots.SendToSpot(config.spotIds[target], request).Async();
                var (error, _) = await correlations.CompleteAsync(entry);
                if (error is not null)
                    throw error;
                probes.Add(new { request.correlationId });
            }
            readiness.Set(
                true,
                "",
                [
                    new
                    {
                        kind = "spotFind",
                        source = "IZLinkSpotManager.FindAsync",
                        observedValue = found,
                    },
                ]
            ); // only after every probe
            measurement.SetupEvidence =
            [
                new
                {
                    kind = "typedProbeEcho",
                    source = "IZLinkSpotClient.SendToSpot -> return Channel send handler",
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
        ServerDrivenStreams.RunAdmissionsAsync(
            measurement,
            config.workload.logicalStreams!.Value,
            LoopAsync
        );

    private async Task LoopAsync(int stream)
    {
        var spotId = config.spotIds[stream % config.spotIds.Length];
        var request = measurement.Request(
            stream,
            checked((ulong)Interlocked.Increment(ref sequences[stream]))
        ) with
        {
            returnChannel = config.channelName,
        };
        if (!measurement.BeginOperation(out var started, "send"))
            return;
        request = request with { sentTicks = DecimalText.Of(started) };
        SendSendCorrelation.Entry entry;
        try
        {
            entry = correlations.Register(request, started); // §13: register immediately before the first public send
        }
        catch (Exception error)
        {
            measurement.CompleteOperation(started, error);
            return;
        }
        try
        {
            await spots.SendToSpot(spotId, request).Async();
            correlations.FirstSendEnded(entry, null);
        }
        catch (Exception error)
        {
            correlations.FirstSendEnded(entry, error);
        }
        _ = CompleteCorrelationAsync(entry, started);
    }

    private async Task CompleteCorrelationAsync(SendSendCorrelation.Entry entry, long started)
    {
        try
        {
            var (result, completed) = await correlations.CompleteAsync(entry).ConfigureAwait(false);
            measurement.CompleteOperation(started, result, completedTicks: completed);
        }
        catch (Exception error)
        {
            measurement.CompleteOperation(started, error);
        }
    }
}

// The return Channel handler of this caller: the Spot's echo arrives as a second one-way send.
public sealed class S2sReturnHandler(SendSendCorrelation correlations)
    : IZLinkSendHandler<PerfEchoReply>
{
    public ValueTask HandleAsync(
        PerfEchoReply message,
        IZLinkMessageContext context,
        CancellationToken cancellationToken
    )
    {
        correlations.Reply(message);
        return ValueTask.CompletedTask;
    }
}
