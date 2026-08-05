using SupportChat.Server.Configuration;
using SupportChat.Shared.Contracts;
using Microsoft.Extensions.Logging;
using Zlink.Framework.Contracts.Handlers;
using Zlink.Framework.Contracts.Spots;

namespace SupportChat.Server.Api.Handlers;

[ZLinkHandlerGroup("api")]
internal sealed class OpenConversationHandler(
    IZLinkSpotManager spots,
    ILogger<OpenConversationHandler> logger)
    : IZLinkRequestHandler<OpenConversationApiReq, OpenConversationApiRes>
{
    public async ValueTask<OpenConversationApiRes> HandleAsync(
        OpenConversationApiReq request,
        IZLinkMessageContext context,
        CancellationToken cancellationToken)
    {
        var created = await spots
            .Create(SampleNames.ConversationSpotType)
            .InMesh(SampleNames.MeshName)
            .Request(new ConversationCreateReq(
                    request.CustomerActorId,
                    request.CustomerDisplayName,
                    request.Subject,
                    DateTimeOffset.UtcNow.ToUnixTimeMilliseconds()))
            .Async(cancellationToken);
        var state = created.Reply?.Decode<ConversationCreateRes>().State
            ?? throw new InvalidOperationException(
                "Conversation Spot creation returned no state.");
        logger.LogInformation(
            "support api open: conversation Spot ready id={ConversationId} status={Status}",
            state.ConversationId,
            state.Status);

        // Agent assignment is a separate step driven after the customer has joined
        // the conversation (see OpenConversationActorHandler), so the open response
        // carries only the allocation result.
        return new OpenConversationApiRes(state);
    }
}
