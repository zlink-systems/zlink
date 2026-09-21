namespace Zlink.Framework.Runtime.Channels;

internal static class ZLinkFanoutSubscriptionPolicy
{
    internal static void Apply(ISubSocket subscriber, IReadOnlySet<string> applicationTopics)
    {
        if (applicationTopics.Count == 0)
            subscriber.SetSubscription(string.Empty);
        else
            foreach (var topic in applicationTopics)
                subscriber.SetSubscription(topic);

        subscriber.SetSubscription(ZLinkFanoutLivenessProtocol.Topic);
    }
}
