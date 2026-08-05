namespace PubSub.Shared;

public static class PubSubNames
{
    public const string Channel = "events";
    public const string SubscriberSocketSource = Channel + ".subscriber";
    public const string PublisherSocketSource = Channel + ".publisher";
    public const string MainTopic = "orders";
    public const string OtherTopic = "billing";
}

public sealed record EventMsg(string RunId, int Sequence, string Value);

public sealed record MissingEventMsg(string RunId, int Sequence, string Value);

public sealed record DrainResultRes(string Result, string? Reason = null);

public sealed record EvidenceWaitReq(
    string[] ContainsAll,
    string[][] ContainsAnyGroups,
    int TimeoutMilliseconds = 10000)
{
    public int AfterIndex { get; init; }

    public string[][] ContainsAllLineGroups { get; init; } = [];
    public string[][] ContainsAnyLineGroups { get; init; } = [];
}
