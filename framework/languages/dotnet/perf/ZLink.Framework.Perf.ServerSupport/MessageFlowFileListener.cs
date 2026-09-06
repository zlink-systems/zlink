using System.Diagnostics;

namespace ZLink.Framework.Perf;

// Diagnostic runs only (perf §21, message-flow spec 26). All flow/error tags are preserved.
public sealed class MessageFlowFileListener : IDisposable
{
    private readonly ActivityListener listener;
    private readonly StreamWriter writer;
    private readonly object gate = new();
    public MessageFlowFileListener(string path)
    {
        writer = new StreamWriter(new FileStream(path, FileMode.CreateNew, FileAccess.Write, FileShare.Read));
        listener = new ActivityListener
        {
            ShouldListenTo = source => source.Name == "Zlink.Framework",
            Sample = (ref ActivityCreationOptions<ActivityContext> _) => ActivitySamplingResult.AllData,
            ActivityStopped = Capture
        };
        ActivitySource.AddActivityListener(listener);
    }
    private void Capture(Activity activity)
    {
        if (activity.OperationName is not ("zlink.message_flow" or "zlink.dispatch_error")) return;
        var record = new { eventId = activity.OperationName, observedTicks = DecimalText.Of(PerfClock.Now),
            clockDomainId = PerfClock.Domain, tags = activity.TagObjects.ToDictionary(tag => tag.Key, tag => tag.Value) };
        lock (gate)
        {
            writer.WriteLine(PerfJson.Write(record));
            writer.Flush();
        }
    }
    public void Dispose()
    {
        listener.Dispose();
        lock (gate) writer.Dispose();
    }
}
