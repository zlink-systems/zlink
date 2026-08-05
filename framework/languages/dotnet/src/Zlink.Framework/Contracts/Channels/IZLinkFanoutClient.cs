namespace Zlink.Framework.Contracts.Channels;

public interface IZLinkFanoutClient
{
    IZLinkFanoutPublishCall Publish<TEvent>(
        string channelName,
        TEvent message);

    IZLinkFanoutPublishCall Publish<TEvent>(
        string channelName,
        string topic,
        TEvent message);
}
