using System.Collections.Concurrent;

namespace ZLink.Framework.Perf;

// The harness correlation of a send/send operation (§13): the first public send and the return send are two one-way
// calls, tied together only by the correlationId in the DTO. The first result of a correlation stands; a reply that
// arrives after that is only counted (duplicate, late or unknown). The table clears with the window at reset.
//
// Register fixes the expiry deadline. The operation owner accounts the result returned by CompleteAsync once.
// The return handler calls Reply.
public sealed class SendSendCorrelation
{
    private const int Pending = 0,
        Succeeded = 1,
        Failed = 2,
        Expired = 3;

    public sealed class Entry(PerfEchoRequest request, long startedTicks, long expiresAtTicks)
    {
        public long StartedTicks { get; } = startedTicks;
        internal PerfEchoRequest? Request = request;
        internal readonly long ExpiresAtTicks = expiresAtTicks;
        internal readonly TaskCompletionSource<(Exception? Error, long CompletedTicks)> Result =
            new(TaskCreationOptions.RunContinuationsAsynchronously);
        internal int State;
    }

    private readonly ConcurrentDictionary<string, Entry> entries = [];
    private readonly Measurement measurement;
    private readonly ScenarioMetrics metrics;

    public SendSendCorrelation(Measurement measurement, ScenarioMetrics metrics)
    {
        this.measurement = measurement;
        this.metrics = metrics.Counters(
            "messages.admitted",
            "messages.expired",
            "messages.duplicateReply",
            "messages.lateReply",
            "messages.unknownCorrelation"
        );
        metrics.OnReset(entries.Clear);
    }

    public Entry Register(PerfEchoRequest request, long startedTicks)
    {
        var entry = new Entry(request, startedTicks, measurement.CallDeadlineTicks());
        if (!entries.TryAdd(request.correlationId, entry))
            throw new PerfValidationException(
                "IdentityMismatch",
                "A correlationId was issued twice."
            );
        return entry;
    }

    public Entry? Find(string correlationId) => entries.GetValueOrDefault(correlationId);

    // The terminal of the first public send: a normal admission is counted; a failure is the final result unless
    // the echo was already fixed first.
    public void FirstSendEnded(Entry entry, Exception? error)
    {
        var now = PerfClock.Now;
        if (ExpireIfDue(entry, now))
            return;
        if (error is null)
        {
            if (measurement.Phase != "setup")
                metrics.Count("messages.admitted");
        }
        else
            Close(entry, Failed, error, now);
    }

    // The return handler's one call: the reply's identity and payload decide the first result.
    public void Reply(PerfEchoReply reply)
    {
        if (!entries.TryGetValue(reply.correlationId, out var entry))
        {
            metrics.Count("messages.unknownCorrelation");
            return;
        }
        var request = Volatile.Read(ref entry.Request); // null once the correlation has a result
        Exception? invalid = null;
        if (request is not null)
        {
            try
            {
                PayloadPattern.ValidateIdentity(request, reply);
                measurement.Pattern.Validate(reply.payload);
            }
            catch (PerfValidationException error)
            {
                invalid = error;
            }
        }
        var now = PerfClock.Now;
        ExpireIfDue(entry, now);
        if (
            !Close(entry, invalid is null ? Succeeded : Failed, invalid, now)
            || Volatile.Read(ref entry.State) == Expired
        )
            metrics.Count(
                Volatile.Read(ref entry.State) == Succeeded
                    ? "messages.duplicateReply"
                    : "messages.lateReply"
            );
    }

    private bool Close(Entry entry, int state, Exception? error, long now)
    {
        lock (entry)
        {
            if (entry.State != Pending)
                return false;
            if (IsDue(entry, now))
            {
                state = Expired;
                error = ExpiredError();
            }
            entry.State = state;
            Interlocked.Exchange(ref entry.Request, null);
            if (state == Expired)
                metrics.Count("messages.expired");
            entry.Result.TrySetResult((error, now));
            return true;
        }
    }

    private static bool IsDue(Entry entry, long now) => now >= entry.ExpiresAtTicks;

    private bool ExpireIfDue(Entry entry, long now) =>
        IsDue(entry, now) && Close(entry, Expired, ExpiredError(), now);

    private static PerfValidationException ExpiredError() =>
        new("CorrelationExpired", "No return send arrived before the correlation deadline.");

    // The final result once the first send has ended: the first result of the correlation, or its expiry. The time is
    // when that result was fixed, so an echo seen before the first send's terminal keeps its own time.
    public async ValueTask<(Exception? Error, long CompletedTicks)> CompleteAsync(Entry entry)
    {
        while (true)
        {
            var now = PerfClock.Now;
            ExpireIfDue(entry, now);
            if (entry.Result.Task.IsCompleted)
                return await entry.Result.Task.ConfigureAwait(false);
            var remaining = Math.Max(0, entry.ExpiresAtTicks - now);
            try
            {
                return await entry
                    .Result.Task.WaitAsync(TimeSpan.FromTicks((remaining + 99) / 100))
                    .ConfigureAwait(false);
            }
            catch (TimeoutException) { }
        }
    }
}
