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
// (customer actor, or agent roster actor), then routes each conversation packet to the
// right bound actor using the ConversationId carried in stream metadata (§9.2). One
// agent session can hold many per-conversation actors at once.
internal sealed class SupportChatSession(
    IZLinkSessionContext context,
    IZLinkRouteClient channels,
    IZLinkActorManager actors,
    ILogger<SupportChatSession> logger) : IZLinkSession
{
    private readonly Dictionary<string, IZLinkSessionActor> _conversationActors = new(StringComparer.Ordinal);
    private IZLinkSessionActor? _identityActor;
    private string _identityActorId = string.Empty;
    private string _identityDisplayName = string.Empty;
    private string _identityRole = string.Empty;

    public IZLinkSessionContext Context { get; } = context;

    public ValueTask OnConnectedAsync(CancellationToken cancellationToken) => ValueTask.CompletedTask;

    public async ValueTask OnDisconnectedAsync(CancellationToken cancellationToken)
    {
        // Propagate the disconnect to every bound actor so each actor's spot can clean up.
        // The agent roster leaves the assignable list via
        // SupportEntrySpot.OnDisconnectActorAsync (§9); customer/per-conversation actors
        // simply detach — their conversations persist for reconnect.
        foreach (var actor in Context.Actors.Bound)
            await actor.NotifyDisconnectedAsync(cancellationToken);
    }

    public ValueTask OnErrorAsync(ZLinkStreamError error, CancellationToken cancellationToken) =>
        ValueTask.CompletedTask;

    public async ValueTask OnDispatchAsync(
        ZLinkSessionDispatchContext dispatch,
        ZLinkMessage payload,
        CancellationToken cancellationToken)
    {
        switch (dispatch.PacketName)
        {
            case nameof(AuthenticateReq):
                await AuthenticateAsync(payload, cancellationToken);
                return;
            case nameof(JoinConversationReq):
                await JoinConversationAsync(dispatch, payload, cancellationToken);
                return;
            default:
                await RelayConversationPacketAsync(dispatch, payload, cancellationToken);
                return;
        }
    }

    private async ValueTask AuthenticateAsync(ZLinkMessage payload, CancellationToken cancellationToken)
    {
        var request = payload.Decode<AuthenticateReq>();
        var authenticated = await channels.RequestToChannel(SampleNames.ApiChannel,
                new AuthenticateUserReq(request.AccessToken))
            .Async<AuthenticateUserRes>(cancellationToken);

        if (!authenticated.Accepted
            || string.IsNullOrWhiteSpace(authenticated.ActorId)
            || string.IsNullOrWhiteSpace(authenticated.DisplayName)
            || string.IsNullOrWhiteSpace(authenticated.Role))
            throw new InvalidOperationException(authenticated.Reason ?? "SupportChat authentication failed.");

        // The identity actor's ParticipantId is its own ActorId (customer id or roster id).
        var actor = await GetOrCreateActorAsync(
            authenticated.ActorId,
            new SupportUserActorCreateReq(
                authenticated.ActorId,
                authenticated.DisplayName,
                authenticated.Role,
                authenticated.ActorId),
            cancellationToken);

        _identityActor = await Context.Actors.BindOrGetAsync(
            actor,
            cancellationToken);
        _identityActorId = authenticated.ActorId;
        _identityDisplayName = authenticated.DisplayName;
        _identityRole = authenticated.Role;

        await Context.Client.Reply(new AuthenticateRes(
                authenticated.ActorId,
                authenticated.DisplayName,
                authenticated.Role))
            .Async(cancellationToken);
    }

    private async ValueTask JoinConversationAsync(
        ZLinkSessionDispatchContext dispatch,
        ZLinkMessage payload,
        CancellationToken cancellationToken)
    {
        // A customer's identity actor is itself the conversation participant, so a
        // customer join just refreshes state on the bound identity actor.
        if (string.Equals(_identityRole, SupportChatRoles.Customer, StringComparison.Ordinal))
        {
            await RequireIdentityActor().RelayAsync(payload, cancellationToken);
            return;
        }

        var conversationId = RequireConversationId(dispatch);
        if (_conversationActors.TryGetValue(conversationId, out var existing))
        {
            await existing.RelayAsync(payload, cancellationToken);
            return;
        }

        // An agent joins each conversation through its own per-conversation actor. Ask
        // the Support server to create it and join it into the ConversationSpot, then
        // bind it onto this session so the agent client receives that room's pushes.
        var conversationActorId = $"{_identityActorId}@{conversationId}";
        var actor = await GetOrCreateActorAsync(
            conversationActorId,
            new SupportUserActorCreateReq(
                conversationActorId,
                _identityDisplayName,
                SupportChatRoles.Agent,
                _identityActorId),
            cancellationToken);

        _conversationActors[conversationId] = await Context.Actors.BindOrGetAsync(
            actor,
            cancellationToken);
        await _conversationActors[conversationId].RelayAsync(
            payload,
            cancellationToken);
        logger.LogInformation(
            "session: agent conversation join submitted. roster={RosterActorId}, conversation={ConversationId}",
            _identityActorId,
            conversationId);
    }

    private async ValueTask RelayConversationPacketAsync(
        ZLinkSessionDispatchContext dispatch,
        ZLinkMessage payload,
        CancellationToken cancellationToken)
    {
        var conversationId = dispatch.Metadata.Find(SampleNames.ConversationIdMetadataKey);
        var target = conversationId is not null && _conversationActors.TryGetValue(conversationId, out var conversationActor)
            ? conversationActor
            : RequireIdentityActor();
        await target.RelayAsync(payload, cancellationToken);
    }

    private IZLinkSessionActor RequireIdentityActor()
    {
        return _identityActor
               ?? throw new InvalidOperationException("Client must authenticate before sending conversation packets.");
    }

    private async ValueTask<ActorRef> GetOrCreateActorAsync(
        string actorId,
        SupportUserActorCreateReq createRequest,
        CancellationToken cancellationToken)
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
                $"Support Actor '{actorId}' creation was rejected."),
            _ => throw new InvalidOperationException(
                $"Support Actor '{actorId}' returned an unknown creation result.")
        };
    }

    private static string RequireConversationId(ZLinkSessionDispatchContext dispatch)
    {
        var conversationId = dispatch.Metadata.Find(SampleNames.ConversationIdMetadataKey);
        if (conversationId is null)
        {
            throw new InvalidOperationException("Conversation packet is missing the ConversationId metadata.");
        }

        return conversationId;
    }

}
