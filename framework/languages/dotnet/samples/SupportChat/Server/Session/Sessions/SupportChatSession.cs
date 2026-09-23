using Microsoft.Extensions.Logging;
using SupportChat.Server.Configuration;
using SupportChat.Shared.Contracts;
using Systems.Zlink;
using Zlink.Framework.Contracts.Actors;
using Zlink.Framework.Contracts.Channels;
using Zlink.Framework.Contracts.Messaging;
using Zlink.Framework.Contracts.Streams;

namespace SupportChat.Server.Session.Sessions;

// Owns one client connection. It authenticates the client and binds an identity actor
// (customer actor, or agent roster actor). Each packet's Actor slot selects a bound
// conversation actor; packets without a slot go to the identity actor.
internal sealed class SupportChatSession(
    IZLinkSessionContext context,
    IZLinkRouteClient channels,
    IZLinkActorManager actors,
    ILogger<SupportChatSession> logger
) : IZLinkSession
{
    private IZLinkSessionActor? _identityActor;
    private string _identityActorId = string.Empty;
    private string _identityDisplayName = string.Empty;
    private string _identityRole = string.Empty;

    public IZLinkSessionContext Context { get; } = context;

    public ValueTask OnConnectedAsync(CancellationToken cancellationToken) =>
        ValueTask.CompletedTask;

    public ValueTask OnDisconnectedAsync(CancellationToken cancellationToken) =>
        ValueTask.CompletedTask;

    public ValueTask OnErrorAsync(ZLinkStreamError error, CancellationToken cancellationToken) =>
        ValueTask.CompletedTask;

    // --8<-- [start:doc-sc-session-dispatch]
    public async ValueTask OnDispatchAsync(
        ZLinkSessionDispatchContext dispatch,
        ZLinkMessage payload,
        CancellationToken cancellationToken
    )
    {
        switch (dispatch.PacketName)
        {
            case nameof(AuthenticateReq):
                await AuthenticateAsync(payload, cancellationToken);
                return;
            case nameof(JoinConversationReq):
                await JoinConversationAsync(payload, cancellationToken);
                return;
            default:
                await RelayConversationPacketAsync(dispatch, payload, cancellationToken);
                return;
        }
    }

    // --8<-- [end:doc-sc-session-dispatch]

    private async ValueTask AuthenticateAsync(
        ZLinkMessage payload,
        CancellationToken cancellationToken
    )
    {
        var request = payload.Decode<AuthenticateReq>();
        var authenticated = await channels
            .RequestToChannel(SampleNames.ApiChannel, new AuthenticateUserReq(request.AccessToken))
            .Async<AuthenticateUserRes>(cancellationToken);

        if (
            !authenticated.Accepted
            || string.IsNullOrWhiteSpace(authenticated.ActorId)
            || string.IsNullOrWhiteSpace(authenticated.DisplayName)
            || string.IsNullOrWhiteSpace(authenticated.Role)
        )
            throw new InvalidOperationException(
                authenticated.Reason ?? "SupportChat authentication failed."
            );

        // --8<-- [start:doc-sc-session-auth]
        // The identity actor's ParticipantId is its own ActorId (customer id or roster id).
        var actor = await GetOrCreateActorAsync(
            authenticated.ActorId,
            new SupportUserActorCreateReq(
                authenticated.ActorId,
                authenticated.DisplayName,
                authenticated.Role,
                authenticated.ActorId
            ),
            cancellationToken
        );

        _identityActor = await Context.Actors.BindOrGetAsync(actor, cancellationToken);
        _identityActorId = authenticated.ActorId;
        _identityDisplayName = authenticated.DisplayName;
        _identityRole = authenticated.Role;
        // --8<-- [end:doc-sc-session-auth]

        await Context
            .Client.Reply(
                new AuthenticateRes(
                    authenticated.ActorId,
                    authenticated.DisplayName,
                    authenticated.Role
                )
            )
            .Async(cancellationToken);
    }

    private async ValueTask JoinConversationAsync(
        ZLinkMessage payload,
        CancellationToken cancellationToken
    )
    {
        var join = payload.Decode<JoinConversationReq>();
        if (string.IsNullOrWhiteSpace(join.ConversationId))
            throw new InvalidOperationException("Conversation join requires a conversationId.");

        // A customer's identity actor is itself the conversation participant, so a
        // customer join just refreshes state on the bound identity actor.
        if (string.Equals(_identityRole, SupportChatRoles.Customer, StringComparison.Ordinal))
        {
            await RequireIdentityActor().RelayAsync(payload, cancellationToken);
            return;
        }

        // --8<-- [start:doc-sc-agent-join]
        // An agent joins each conversation through its own per-conversation actor. Ask
        // the Support server to create it and join it into the ConversationSpot, then
        // bind it onto this session so the agent client receives that room's pushes.
        var conversationActorId = $"{_identityActorId}@{join.ConversationId}";
        var actor = await GetOrCreateActorAsync(
            conversationActorId,
            new SupportUserActorCreateReq(
                conversationActorId,
                _identityDisplayName,
                SupportChatRoles.Agent,
                _identityActorId
            ),
            cancellationToken
        );

        var boundActor = await Context.Actors.BindOrGetAsync(actor, cancellationToken);
        await boundActor.RelayAsync(payload, cancellationToken);
        // --8<-- [end:doc-sc-agent-join]
        logger.LogInformation(
            "session: agent conversation join submitted. roster={RosterActorId}, conversation={ConversationId}",
            _identityActorId,
            join.ConversationId
        );
    }

    private async ValueTask RelayConversationPacketAsync(
        ZLinkSessionDispatchContext dispatch,
        ZLinkMessage payload,
        CancellationToken cancellationToken
    )
    {
        // --8<-- [start:doc-sc-actor-relay]
        var target = dispatch.Actor ?? RequireIdentityActor();
        // --8<-- [end:doc-sc-actor-relay]
        await target.RelayAsync(payload, cancellationToken);
    }

    private IZLinkSessionActor RequireIdentityActor()
    {
        return _identityActor
            ?? throw new InvalidOperationException(
                "Client must authenticate before sending conversation packets."
            );
    }

    private async ValueTask<ActorRef> GetOrCreateActorAsync(
        string actorId,
        SupportUserActorCreateReq createRequest,
        CancellationToken cancellationToken
    )
    {
        return await actors
            .GetOrCreate(actorId, SampleNames.SupportActorType)
            .InMesh(SampleNames.MeshName)
            .Request(createRequest)
            .Async(cancellationToken) switch
        {
            ZLinkActorCreateResult.Existing value => value.Actor,
            ZLinkActorCreateResult.Created value => value.Actor,
            ZLinkActorCreateResult.Rejected => throw new InvalidOperationException(
                $"Support Actor '{actorId}' creation was rejected."
            ),
            _ => throw new InvalidOperationException(
                $"Support Actor '{actorId}' returned an unknown creation result."
            ),
        };
    }
}
