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
        ZLinkFanoutLivenessProtocol.ValidateApplicationTopic(topic, nameof(topic));
        return new ZLinkPublishCall(runtime, registration, channelName, topic, message);
    }
}
