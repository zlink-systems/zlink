using System.Collections.Concurrent;

namespace ZLink.Framework.Perf;

// The harness correlation of a send/send operation (§13): the first public send and the return send are two one-way
// calls, tied together only by the correlationId in the DTO. The first result of a correlation stands; a reply that
// arrives after that is only counted (duplicate, late or unknown). The table clears with the window at reset.
//
// Order of one operation: Register (right before the first public send, fixing the expiry deadline), then
// FirstSendEnded with that send's terminal, then CompleteAsync for the final result. The return handler calls Reply.
public sealed class SendSendCorrelation
{
    private const int Pending = 0, Succeeded = 1, Failed = 2, Expired = 3;

    public sealed class Entry(PerfEchoRequest request, long startedTicks, long expiresAtTicks)
    {
        public long StartedTicks { get; } = startedTicks;
        internal readonly PerfEchoRequest Request = request;
        internal readonly long ExpiresAtTicks = expiresAtTicks;
        internal readonly TaskCompletionSource<Exception?> Result = new(TaskCreationOptions.RunContinuationsAsynchronously);
        internal int State;
        internal long ClosedTicks;
        internal bool Close(int state, Exception? error)
        {
            if (Interlocked.CompareExchange(ref State, state, Pending) != Pending) return false;
            ClosedTicks = PerfClock.Now;
            Result.SetResult(error);
            return true;
        }
    }

    private readonly ConcurrentDictionary<string, Entry> entries = [];
    private readonly Measurement measurement;
    private readonly ScenarioMetrics metrics;

    public SendSendCorrelation(Measurement measurement, ScenarioMetrics metrics)
    {
        this.measurement = measurement;
        this.metrics = metrics.Counters("messages.admitted", "messages.expired", "messages.duplicateReply",
            "messages.lateReply", "messages.unknownCorrelation");
        metrics.OnReset(entries.Clear);
    }

    public Entry Register(PerfEchoRequest request, long startedTicks)
    {
        var entry = new Entry(request, startedTicks, PerfClock.Now + measurement.Config.workload.correlationExpiryMs * 1_000_000L);
        if (!entries.TryAdd(request.correlationId, entry))
            throw new PerfValidationException("IdentityMismatch", "A correlationId was issued twice.");
        return entry;
    }

    public Entry? Find(string correlationId) => entries.GetValueOrDefault(correlationId);

    // The terminal of the first public send: a normal admission is counted; a failure is the final result unless
    // the echo was already fixed first.
    public void FirstSendEnded(Entry entry, Exception? error)
    {
        if (error is null) { if (measurement.Phase != "setup") metrics.Count("messages.admitted"); }
        else entry.Close(Failed, error);
    }

    // The return handler's one call: the reply's identity and payload decide the first result.
    public void Reply(PerfEchoReply reply)
    {
        if (!entries.TryGetValue(reply.correlationId, out var entry)) { metrics.Count("messages.unknownCorrelation"); return; }
        Exception? invalid = null;
        try
        {
            PayloadPattern.ValidateIdentity(entry.Request, reply);
            measurement.Pattern.Validate(reply.payload);
        }
        catch (PerfValidationException error) { invalid = error; }
        if (!entry.Close(invalid is null ? Succeeded : Failed, invalid))
            metrics.Count(Volatile.Read(ref entry.State) == Succeeded ? "messages.duplicateReply" : "messages.lateReply");
    }

    // The final result once the first send has ended: the first result of the correlation, or its expiry. The time is
    // when that result was fixed, so an echo seen before the first send's terminal keeps its own time.
    public async ValueTask<(Exception? Error, long CompletedTicks)> CompleteAsync(Entry entry)
    {
        try
        {
            var remaining = Math.Max(0, entry.ExpiresAtTicks - PerfClock.Now);
            var error = await entry.Result.Task.WaitAsync(TimeSpan.FromTicks(remaining / 100)).ConfigureAwait(false);
            return (error, entry.ClosedTicks);
        }
        catch (TimeoutException)
        {
            if (entry.Close(Expired, new PerfValidationException("CorrelationExpired", "No return send arrived before the correlation deadline.")))
                metrics.Count("messages.expired");
            return (await entry.Result.Task.ConfigureAwait(false), entry.ClosedTicks);
        }
    }
}
