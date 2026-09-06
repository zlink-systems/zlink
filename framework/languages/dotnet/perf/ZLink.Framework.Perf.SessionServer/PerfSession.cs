using Zlink.Framework.Contracts.Messaging;
using Zlink.Framework.Contracts.Streams;

namespace ZLink.Framework.Perf;

// §11.1: STREAM-only receiver; no Object Server, Actor, Store or automatic discovery.
public sealed class PerfSession(IZLinkSessionContext context, Measurement measurement) : IZLinkSession
{
    public IZLinkSessionContext Context { get; } = context;
    public void Configure() => Context.Handlers.AddHandler<SessionEchoHandler>(nameof(PerfEchoRequest));
    public ValueTask OnConnectedAsync(CancellationToken cancellationToken) => ValueTask.CompletedTask;
    public ValueTask OnDisconnectedAsync(CancellationToken cancellationToken) => ValueTask.CompletedTask;
    public ValueTask OnErrorAsync(ZLinkStreamError error, CancellationToken cancellationToken)
    {
        measurement.RecordDiagnostic(new InvalidOperationException($"STREAM {error.Error}: {error.Message}"));
        return ValueTask.CompletedTask;
    }
    public async ValueTask OnDispatchAsync(ZLinkSessionDispatchContext dispatch, ZLinkMessage payload, CancellationToken cancellationToken)
    {
        if (!await Context.Handlers.TryHandleAsync(dispatch, payload, cancellationToken))
            throw new InvalidOperationException("No typed perf session handler was registered for the packet.");
    }
}
public sealed class SessionEchoHandler(Measurement measurement) : IZLinkSessionPacketHandler<IZLinkSessionContext, PerfEchoRequest>
{
    public async ValueTask HandleAsync(IZLinkSessionContext context, ZLinkSessionDispatchContext dispatch,
        PerfEchoRequest request, CancellationToken cancellationToken)
    {
        var received = PerfClock.Now;
        measurement.HandlerEnter();
        try
        {
            measurement.ValidateRequest(request);
            var reply = PayloadPattern.Reply(request, received);
            measurement.RecordReply(request);
            await context.Client.Reply(reply).Async(cancellationToken);
            if (measurement.Phase == "setup") measurement.SetupEvidence =
                [new { kind = "typedProbeReply", source = "SessionEchoHandler.Client.Reply.Async", observedValue = request.correlationId }];
        }
        catch (Exception error) { measurement.RecordDiagnostic(error); throw; }
        finally { measurement.HandlerExit(); }
    }
}
