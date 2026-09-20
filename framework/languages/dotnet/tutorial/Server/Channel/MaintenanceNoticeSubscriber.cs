using Tutorial.Shared;
using Zlink.Framework.Contracts.Channels;
using Zlink.Framework.Contracts.Handlers;

namespace Tutorial.Server.Channel;

// --8<-- [start:fanout-handler]
// Receives what any publisher on this channel sends. The publisher does not know
// this node exists, so adding or removing a subscriber changes nothing there.
public sealed class MaintenanceNoticeSubscriber(ILogger<MaintenanceNoticeSubscriber> logger)
    : IZLinkFanoutHandler<MaintenanceNotice>
{
    public ValueTask HandleAsync(
        MaintenanceNotice message,
        // The context carries the topic this event arrived on. A fanout
        // subscriber receives every topic on the channel, so this is how a
        // handler tells them apart.
        ZLinkPublishMessageContext context,
        CancellationToken cancellationToken
    )
    {
        logger.LogInformation(
            "maintenance notice: {Message} (topic {Topic})",
            message.Message,
            context.Topic
        );
        return ValueTask.CompletedTask;
    }
}
// --8<-- [end:fanout-handler]
