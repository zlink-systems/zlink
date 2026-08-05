using Zlink.Framework.Contracts.Handlers;
using SupportChat.Server.Configuration;
using SupportChat.Server.Support.Application.ConversationAssignment;
using SupportChat.Server.Support.Infrastructure.ZLink.Actors;
using SupportChat.Shared.Contracts;
using Zlink.Framework.Contracts.Actors;
using Zlink.Framework.Contracts.Spots;

namespace SupportChat.Server.Support.Infrastructure.ZLink.Spots.EntrySpot.Handlers;

internal sealed class SetAgentAvailableHandler(
    AgentAssignmentService assignment,
    SupportActorDirectory actors)
    : IZLinkEntrySpotActorRequestHandler<SupportEntrySpot, SupportUserActor, SetAgentAvailableReq, SetAgentAvailableRes>
{
    public ValueTask<SetAgentAvailableRes> HandleAsync(
        SupportEntrySpot entrySpot,
        SupportUserActor actor,
        IZLinkMessageContext context,
        SetAgentAvailableReq message,
        CancellationToken cancellationToken)
    {
        if (!string.Equals(actor.Role, SupportChatRoles.Agent, StringComparison.Ordinal))
            throw new InvalidOperationException("Only agent actors can set availability.");

        actors.AddOrUpdate(actor);
        assignment.SetAvailable(actor.ActorId, actor.DisplayName, message.IsAvailable);
        return ValueTask.FromResult(new SetAgentAvailableRes(message.IsAvailable));
    }
}
