using Zlink.Framework.Contracts.Spots;

namespace ZLink.Framework.Perf;

// The User Spot that answers a typed PerfEchoRequest with the typed echo at once: the target of §10.3 and the
// local echo Spot of §10.7. The typed request handler is the only application code on the Spot.
public sealed class PerfEchoSpot(IZLinkSpotContext context) : IZLinkSpot
{
    public IZLinkSpotContext Context { get; } = context;
    public void Configure() => Context.Handlers.AddPacket<PerfEchoRequestHandler>();
}

public sealed class PerfEchoRequestHandler(Measurement measurement, ScenarioMetrics metrics)
    : IZLinkSpotRequestHandler<PerfEchoSpot, PerfEchoRequest, PerfEchoReply>
{
    public ValueTask<PerfEchoReply> HandleAsync(PerfEchoSpot spot, PerfEchoRequest request, CancellationToken cancellationToken)
    {
        var received = PerfClock.Now;
        measurement.HandlerEnter();
        try
        {
            measurement.ValidateRequest(request);
            var reply = PayloadPattern.Reply(request, received);
            measurement.RecordReply(request);
            if (request.phase == "measured") metrics.Count("spot.applicationHandlerEntries");
            if (measurement.Phase == "setup" && !measurement.Config.source) measurement.SetupEvidence =
                [new { kind = "typedProbeReply", source = "IZLinkSpotRequestHandler<PerfEchoRequest,PerfEchoReply>", observedValue = request.correlationId }];
            return ValueTask.FromResult(reply);
        }
        catch (Exception error) { measurement.RecordDiagnostic(error); throw; }
        finally { measurement.HandlerExit(); }
    }
}
