using Zlink.Framework.Contracts.Channels;
using Zlink.Framework.Contracts.Handlers;

namespace ZLink.Framework.Perf;

// §10.11 Subscriber: the typed fanout handler validates each event and records its unique sequence.
public sealed class PerfFanoutHandler(Measurement measurement, FanoutReceipts receipts) : IZLinkFanoutHandler<PerfPublishEvent>
{
    public ValueTask HandleAsync(PerfPublishEvent message, ZLinkPublishMessageContext context, CancellationToken cancellationToken)
    {
        var receivedTicks = PerfClock.Now;
        measurement.HandlerEnter();
        try
        {
            receipts.Record(message, receivedTicks);
            return ValueTask.CompletedTask;
        }
        catch (Exception error) { measurement.RecordDiagnostic(error); throw; }
        finally { measurement.HandlerExit(); }
    }
}
