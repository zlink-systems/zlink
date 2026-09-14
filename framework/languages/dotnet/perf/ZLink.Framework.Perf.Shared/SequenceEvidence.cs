namespace ZLink.Framework.Perf;

public sealed record SequenceRange(string first, string last);

// Compact numeric evidence only; payload and correlation text are not retained per delivery.
public sealed class SequenceEvidence
{
    private sealed class Stream
    {
        public readonly HashSet<ulong> Attempted = [], WindowSuccess = [], SettleSuccess = [], WindowReceipt = [], SettleReceipt = [];
        public ulong Duplicates;
    }
    private readonly Dictionary<int, Stream> streams = [];
    private Stream Get(int id)
    {
        if (!streams.TryGetValue(id, out var stream)) streams.Add(id, stream = new());
        return stream;
    }
    public void Attempt(int id, ulong sequence) => Get(id).Attempted.Add(sequence);
    public void Admission(int id, ulong sequence, bool inWindow) =>
        (inWindow ? Get(id).WindowSuccess : Get(id).SettleSuccess).Add(sequence);
    public bool Receipt(int id, ulong sequence, bool inWindow)
    {
        var stream = Get(id);
        if (stream.WindowReceipt.Contains(sequence) || stream.SettleReceipt.Contains(sequence))
        { stream.Duplicates++; return false; }
        return (inWindow ? stream.WindowReceipt : stream.SettleReceipt).Add(sequence);
    }
    public ulong Unique => streams.Values.Aggregate(0UL, (a, s) => checked(a + (ulong)s.WindowReceipt.Count + (ulong)s.SettleReceipt.Count));
    public ulong Duplicates => streams.Values.Aggregate(0UL, (a, s) => checked(a + s.Duplicates));
    public object[] SendSources() => streams.OrderBy(p => p.Key).Select(p => (object)new
    { clientId = p.Key, attemptedRanges = Ranges(p.Value.Attempted), windowAdmissionRanges = Ranges(p.Value.WindowSuccess),
        settleAdmissionRanges = Ranges(p.Value.SettleSuccess) }).ToArray();
    public object[] SendReceivers() => streams.OrderBy(p => p.Key).Select(p => (object)new
    { clientId = p.Key, windowRanges = Ranges(p.Value.WindowReceipt), settleRanges = Ranges(p.Value.SettleReceipt),
        duplicateCount = DecimalText.Of(p.Value.Duplicates) }).ToArray();
    public object Publisher(string runId, string cellId, string resetSeq) => new { runId, cellId, resetSeq, phase = resetSeq == "0" ? "warmup" : "measured", attemptedRanges = Ranges(Get(0).Attempted), windowSuccessRanges = Ranges(Get(0).WindowSuccess),
        settleSuccessRanges = Ranges(Get(0).SettleSuccess) };
    public object Subscriber(int id, string runId, string cellId, string resetSeq) => new { runId, cellId, resetSeq, phase = resetSeq == "0" ? "warmup" : "measured", subscriberId = id, windowRanges = Ranges(Get(0).WindowReceipt),
        settleRanges = Ranges(Get(0).SettleReceipt), duplicateEvents = DecimalText.Of(Get(0).Duplicates), timingEvidence = (object?)null,
        nullReasons = new Dictionary<string, NullReason> { ["/timingEvidence"] = new("CLOCK_DOMAIN_UNVERIFIED", "Separate process clock domains are not aligned.") } };
    public long RetainedBytes => streams.Values.Sum(s => 8L * (s.Attempted.Count + s.WindowSuccess.Count + s.SettleSuccess.Count + s.WindowReceipt.Count + s.SettleReceipt.Count));
    private static SequenceRange[] Ranges(HashSet<ulong> values)
    {
        List<SequenceRange> ranges = [];
        ulong first = 0, last = 0;
        var have = false;
        foreach (var value in values.Order())
        {
            if (!have) { first = last = value; have = true; }
            else if (last != ulong.MaxValue && value == last + 1) last = value;
            else { ranges.Add(new(DecimalText.Of(first), DecimalText.Of(last))); first = last = value; }
        }
        if (have) ranges.Add(new(DecimalText.Of(first), DecimalText.Of(last)));
        return ranges.ToArray();
    }
}
