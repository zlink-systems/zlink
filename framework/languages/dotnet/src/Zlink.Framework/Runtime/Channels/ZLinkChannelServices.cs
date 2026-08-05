namespace Zlink.Framework.Runtime.Channels;

internal sealed class ZLinkFanoutClient(ZLinkFrameworkRuntime runtime, ZLinkFrameworkRegistration registration)
    : IZLinkFanoutClient
{
    public IZLinkFanoutPublishCall Publish<TEvent>(string channelName, TEvent message) =>
        Publish(
            channelName,
            ZLinkMessageNameResolver.ResolveFromMessage(message),
            message);

    public IZLinkFanoutPublishCall Publish<TEvent>(string channelName, string topic, TEvent message)
    {
        if (ZLinkFanoutLivenessProtocol.IsReservedTopic(topic))
            throw new ArgumentException(
                "The fanout liveness topic is reserved by the Framework.",
                nameof(topic));
        return new ZLinkPublishCall(runtime, registration, channelName, topic, message);
    }
}
