using TicTacToe.Server.Configuration;
using TicTacToe.Shared.Contracts;
using Zlink.Framework.Contracts.Channels;
using Zlink.Framework.Contracts.Handlers;
using Zlink.Framework.Contracts.Spots;

namespace TicTacToe.Server.Play.Infrastructure.ZLink.Spots.EntrySpot.Handlers;

[ZLinkSpotSubscriptionHandler(SampleTopics.PlayerMilestoneChannel, SampleTopics.PlayerMilestone)]
internal sealed class PlayerWinMilestoneEventHandler(
    ILogger<PlayerWinMilestoneEventHandler> logger)
    : IZLinkSpotSubscriptionHandler<PlayEntrySpot, PlayerWinMilestoneEvent>
{
    public async ValueTask HandleAsync(
        PlayEntrySpot spot,
        PlayerWinMilestoneEvent message,
        ZLinkPublishMessageContext context,
        CancellationToken cancellationToken)
    {
        _ = context;
        logger.LogInformation(
            "entry spot: milestone event received. actor={ActorId}, roomId={RoomId}, wins={Wins}",
            message.ActorId,
            message.RoomId,
            message.Wins);

        await spot.NotifyMilestoneAsync(message, cancellationToken);
    }
}
