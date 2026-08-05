namespace PubSub.Server.Subscriber.Configuration;

using Zlink.Framework.E2E.Configuration;

internal sealed record SubscriberOptions(
    string Rid,
    string HttpUrl,
    string LogDir,
    string PublisherEndpoint,
    int HandlerDelayMs = 0,
    string? EvidenceFile = null)
{
    public static SubscriberOptions Parse(string[] args)
        => E2eConfiguration.Load<SubscriberOptions>(args);
}
