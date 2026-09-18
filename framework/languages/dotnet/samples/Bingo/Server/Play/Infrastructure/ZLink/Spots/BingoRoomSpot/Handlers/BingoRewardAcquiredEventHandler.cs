using Bingo.Server.Configuration;
using Bingo.Shared.Contracts;
using Zlink.Framework.Contracts.Channels;
using Zlink.Framework.Contracts.Handlers;
using Zlink.Framework.Contracts.Spots;

namespace Bingo.Server.Play.Infrastructure.ZLink.Spots.BingoRoomSpot.Handlers;

// --8<-- [start:doc-bingo-reward-subscribe]
[ZLinkSpotSubscriptionHandler(SampleNames.RoomChannel, SampleNames.RewardTopic)]
internal sealed class
    BingoRewardAcquiredEventHandler : IZLinkSpotSubscriptionHandler<BingoRoom, BingoRewardAcquiredEvent>
{
    public ValueTask HandleAsync(
        BingoRoom spot,
        BingoRewardAcquiredEvent message,
        ZLinkPublishMessageContext context,
        CancellationToken cancellationToken)
    {
        return spot.AnnounceRewardAsync(message, cancellationToken);
    }
}
// --8<-- [end:doc-bingo-reward-subscribe]
