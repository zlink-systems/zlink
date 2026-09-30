using Microsoft.Extensions.DependencyInjection;
using Microsoft.Extensions.Hosting;
using Zlink.Framework.Contracts.Channels;
using Zlink.Framework.Contracts.Configuration;
using Zlink.Framework.Contracts.Handlers;
using Zlink.Framework.Contracts.Spots;

namespace ZLink.Framework.Perf;

// The Channel target of §10.6 (Object Client): the Channel send handler receives the Spot's send and answers with a
// second one-way send to the SpotId that the DTO names in returnSpotId. The measured operation lives in the Spot
// process; this side does not assume the Channel context carries the source SpotId.
public static class S2sSpotToChannelSendSendEchoTarget
{
    public static async Task RunAsync(RoleConfig config)
    {
        if (config.role != "channel" || config.source) throw new ArgumentException("The Channel role is the echo target of this scenario.");
        var builder = ServerApplication.Builder(config, options =>
        {
            var mesh = options.AddRouteMesh(config.meshName!).SetRoutingIdPrefix("perf-channel").Listen(config.transportEndpoints["mesh"]);
            mesh.Objects().Client();
            mesh.Channel(config.channelName!).Server().AddSendHandler<S2sReturnToSpotHandler, PerfEchoRequest>();
        });
        var app = builder.Build();
        ServerApplication.Map(app);
        await app.RunAsync();
    }
}

public sealed class S2sReturnToSpotHandler(Measurement measurement, IZLinkSpotClient spots) : IZLinkSendHandler<PerfEchoRequest>
{
    public async ValueTask HandleAsync(PerfEchoRequest message, IZLinkMessageContext context, CancellationToken cancellationToken)
    {
        var received = PerfClock.Now;
        measurement.HandlerEnter();
        try
        {
            if (string.IsNullOrEmpty(message.returnSpotId)) throw new PerfValidationException("IdentityMismatch", "No return SpotId in the request.");
            measurement.ValidateRequest(message, returnSpotId: message.returnSpotId);
            var reply = PayloadPattern.Reply(message, received);
            measurement.RecordApplicationCall(message, "send");
            await spots.SendToSpot(message.returnSpotId, reply).Async(cancellationToken);
            if (measurement.Phase == "setup") measurement.SetupEvidence =
                [new { kind = "typedProbeReply", source = "IZLinkSendHandler<PerfEchoRequest> -> IZLinkSpotClient.SendToSpot", observedValue = message.correlationId }];
        }
        catch (Exception error) { measurement.RecordDiagnostic(error); throw; }
        finally { measurement.HandlerExit(); }
    }
}
