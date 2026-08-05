namespace PubSub.Server.Publisher.Configuration;

using Zlink.Framework.E2E.Configuration;

internal sealed record PublisherOptions(
    string Rid,
    string HttpUrl,
    string LogDir,
    string PublisherEndpoint,
    string? EvidenceFile = null)
{
    public static PublisherOptions Parse(string[] args)
        => E2eConfiguration.Load<PublisherOptions>(args);
}
