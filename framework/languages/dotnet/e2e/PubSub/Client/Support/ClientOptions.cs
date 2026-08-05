using Zlink.Framework.E2E.Configuration;
namespace PubSub.Client.Support;

internal sealed record ClientOptions(
    string PublisherUrl,
    string LateSubscriberUrl,
    string PublisherEndpoint,
    string PublisherProject,
    string SubscriberProject,
    string ConfigDir,
    string LogDir,
    string Scenario,
    string[] SubscriberUrls)
{
    public static ClientOptions Parse(string[] args)
        => E2eConfiguration.Load<ClientOptions>(args);
}
