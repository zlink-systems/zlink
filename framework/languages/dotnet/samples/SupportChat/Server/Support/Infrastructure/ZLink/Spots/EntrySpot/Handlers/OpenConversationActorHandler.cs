using Zlink.Framework.Contracts.Handlers;
using Microsoft.Extensions.Logging;
using SupportChat.Server.Configuration;
using SupportChat.Server.Support.Infrastructure.ZLink.Actors;
using SupportChat.Shared.Contracts;
using Zlink.Framework.Contracts.Actors;
using Zlink.Framework.Contracts.Spots;

namespace SupportChat.Server.Support.Infrastructure.ZLink.Spots.EntrySpot.Handlers;

internal sealed class OpenConversationActorHandler(
    ILogger<OpenConversationActorHandler> logger)
    : IZLinkEntrySpotActorRequestHandler<SupportEntrySpot, SupportUserActor, OpenConversationReq, OpenConversationRes>
{
    public async ValueTask<OpenConversationRes> HandleAsync(
        SupportEntrySpot entrySpot,
        SupportUserActor actor,
        IZLinkMessageContext context,
        OpenConversationReq message,
        CancellationToken cancellationToken)
    {
        if (!string.Equals(actor.Role, SupportChatRoles.Customer, StringComparison.Ordinal))
            throw new InvalidOperationException("Only customer actors can open a conversation.");

        logger.LogInformation(
            "support entry open: request actor={ActorId} subject={Subject}",
            actor.ActorId,
            message.Subject);

        // The API server allocates the conversation; this handler then joins the
        // customer. Agent assignment happens inside the ConversationSpot when the
        // customer joins, so it can reserve and later release the agent's capacity.
        var opened = await entrySpot.Context.Outbound.RequestToChannel(
                SampleNames.ApiChannel,
                new OpenConversationApiReq(
                    actor.ParticipantId,
                    actor.DisplayName,
                    message.Subject))
            .Async<OpenConversationApiRes>(cancellationToken);

        actor.TrackDeferredJoin(opened.State.ConversationId, notifyBoundSession: false);
        actor.Context.JoinSpot(
                opened.State.ConversationId,
                new JoinConversationReq(actor.ParticipantId, actor.Role, actor.DisplayName))
            .Defer();

        logger.LogInformation(
            "support entry open: completed conversation={ConversationId} status={Status}",
            opened.State.ConversationId,
            opened.State.Status);
        return new OpenConversationRes(opened.State.ConversationId, opened.State);
    }
}
