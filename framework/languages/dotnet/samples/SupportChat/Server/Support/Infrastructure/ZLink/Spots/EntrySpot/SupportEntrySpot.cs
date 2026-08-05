using Microsoft.Extensions.Logging;
using SupportChat.Server.Configuration;
using SupportChat.Server.Support.Application.ConversationAssignment;
using SupportChat.Server.Support.Infrastructure.ZLink.Actors;
using SupportChat.Shared.Contracts;
using Zlink.Framework.Contracts.Actors;
using Zlink.Framework.Contracts.Messaging;
using Zlink.Framework.Contracts.Spots;

namespace SupportChat.Server.Support.Infrastructure.ZLink.Spots.EntrySpot;

internal sealed class SupportEntrySpot(
    IZLinkEntrySpotContext context,
    SupportActorDirectory directory,
    AgentAssignmentService assignment,
    ILogger<SupportEntrySpot> logger) : IZLinkEntrySpot<SupportUserActor>
{
    public IZLinkEntrySpotContext Context { get; } = context;

    public ValueTask<ZLinkActorCreateResponse> OnCreateActorAsync(
        SupportUserActor actor,
        ZLinkMessage createRequest,
        CancellationToken cancellationToken)
    {
        var request = createRequest.Decode<SupportUserActorCreateReq>();
        actor.SetIdentity(request.DisplayName, request.Role, request.ParticipantId);
        directory.AddOrUpdate(actor);
        logger.LogInformation(
            "support entry: actor created. actor={ActorId}, role={Role}",
            actor.ActorId,
            actor.Role);
        return ValueTask.FromResult(ZLinkActorCreateResponse.Accept());
    }

    public ValueTask<ZLinkSpotActorJoinResult> OnActorJoinAsync(
        string actorId,
        ZLinkMessage request,
        CancellationToken cancellationToken)
    {
        return ValueTask.FromResult(ZLinkSpotActorJoinResult.Accept(request));
    }

    public ValueTask OnJoinedActorAsync(
        SupportUserActor actor,
        CancellationToken cancellationToken)
    {
        logger.LogInformation(
            "support entry: actor joined. actor={ActorId}, role={Role}",
            actor.ActorId,
            actor.Role);
        return ValueTask.CompletedTask;
    }

    public ValueTask OnLeaveActorAsync(
        SupportUserActor actor,
        CancellationToken cancellationToken)
    {
        logger.LogInformation(
            "support entry: actor left. actor={ActorId}",
            actor.ActorId);
        return ValueTask.CompletedTask;
    }

    // Fires when a member actor's stream session disconnects. An agent's roster actor
    // leaves the assignable list so it stops receiving new conversations until it
    // reconnects and re-registers availability (§9).
    public ValueTask OnDisconnectActorAsync(
        SupportUserActor actor,
        CancellationToken cancellationToken)
    {
        if (string.Equals(actor.Role, SupportChatRoles.Agent, StringComparison.Ordinal))
        {
            assignment.SetAvailable(actor.ActorId, actor.DisplayName, false);
            logger.LogInformation(
                "support entry: agent disconnected, availability removed. actor={ActorId}",
                actor.ActorId);
        }

        return ValueTask.CompletedTask;
    }
}
