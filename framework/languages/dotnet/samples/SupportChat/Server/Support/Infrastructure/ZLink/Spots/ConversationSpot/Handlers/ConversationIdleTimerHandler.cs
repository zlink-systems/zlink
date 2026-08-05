using Zlink.Framework.Contracts.Handlers;
using Zlink.Framework.Contracts.Spots;
using Zlink.Framework.Contracts.Timers;

namespace SupportChat.Server.Support.Infrastructure.ZLink.Spots.ConversationSpot.Handlers;

// Periodic tick that advances the conversation's idle lifecycle
// (Active -> WaitingForClose -> Closed). It runs inside the spot dispatch context, so
// the idle/close pushes it triggers are delivered to the bound sessions.
[ZLinkSpotTimerHandler("conversation-idle", 200)]
internal sealed class ConversationIdleTimerHandler : IZLinkSpotTimerHandler<ConversationSpot>
{
    public ValueTask HandleAsync(
        ConversationSpot spot,
        ZLinkTimerTick tick,
        CancellationToken cancellationToken)
    {
        _ = tick;
        return spot.CheckIdleAsync(cancellationToken);
    }
}
