using Microsoft.Extensions.DependencyInjection;
using Microsoft.Extensions.Hosting;
using Zlink.Framework.Contracts.Channels;
using Zlink.Framework.Contracts.Configuration;
using Zlink.Framework.Contracts.Spots;

namespace ZLink.Framework.Perf;

// The Spot process of §10.3 and §10.4 (Object Server, no driver): User Spots that answer a request with a typed
// echo (§10.3) or answer a send by sending the echo to the caller's return Channel (§10.4). The measured
// operation lives in the Channel process; this side only echoes.
public static class S2sChannelToSpotEchoTarget
{
    public static async Task RunAsync(RoleConfig config)
    {
        var request = config.scenario == "s2s-channel-to-spot-request-echo";
        var builder = request ? SpotRole.Builder<PerfEchoSpot>(config, callsChannel: false)
                              : SpotRole.Builder<S2sSendEchoSpot>(config, callsChannel: true);   // the echo goes to the caller return ChannelName
        builder.Services.AddSingleton(sp => new ScenarioMetrics(sp.GetRequiredService<Measurement>()).SpotInternalsUnsupported());
        var app = builder.Build();
        _ = app.Services.GetRequiredService<ScenarioMetrics>();
        ServerApplication.Map(app);
        await app.StartAsync();
        var objects = await SpotRole.CreateSpotsAsync(config, app.Services.GetRequiredService<IZLinkSpotManager>(),
            app.Services.GetRequiredService<IZLinkRouteMeshRuntime>(), app.Services.GetRequiredService<Measurement>(), app.Lifetime.ApplicationStopping);
        if (objects is not null) SpotRole.Publish(app.Services.GetRequiredService<ObjectsReadiness>(), objects);
        await app.WaitForShutdownAsync();
    }
}

public sealed class S2sSendEchoSpot(IZLinkSpotContext context) : IZLinkSpot
{
    public IZLinkSpotContext Context { get; } = context;
    public void Configure() => Context.Handlers.AddPacket<S2sSendEchoHandler>();
}

// §10.4: the echo goes back as a second one-way send to the caller's own return ChannelName (in the DTO).
public sealed class S2sSendEchoHandler(Measurement measurement) : IZLinkSpotPacketHandler<S2sSendEchoSpot, PerfEchoRequest>
{
    public async ValueTask HandleAsync(S2sSendEchoSpot spot, PerfEchoRequest message, CancellationToken cancellationToken)
    {
        var received = PerfClock.Now;
        measurement.HandlerEnter();
        try
        {
            measurement.ValidateRequest(message, returnChannel: message.returnChannel);
            if (string.IsNullOrEmpty(message.returnChannel)) throw new PerfValidationException("IdentityMismatch", "No return Channel in the request.");
            var reply = PayloadPattern.Reply(message, received);
            measurement.RecordApplicationCall(message, "send");
            await spot.Context.Outbound.SendToChannel(message.returnChannel, reply).Async(cancellationToken);
            if (measurement.Phase == "setup") measurement.SetupEvidence =
                [new { kind = "typedProbeReply", source = "IZLinkSpotPacketHandler<PerfEchoRequest> -> SendToChannel", observedValue = message.correlationId }];
        }
        catch (Exception error) { measurement.RecordDiagnostic(error); throw; }
        finally { measurement.HandlerExit(); }
    }
}
