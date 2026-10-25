using Zlink.Framework.Contracts.Handlers;
using Zlink.Framework.Contracts.Spots;

namespace MyGame.Shared;

public sealed class LobbyEntry(IZLinkEntrySpotContext context, IZLinkSpotManager spots)
    : IZLinkEntrySpot<PlayerActor>
{
    public IZLinkEntrySpotContext Context { get; } = context;

    public void Configure()
    {
        Context.Handlers.AddActorPacket<LobbyLogin, PlayerActor>(nameof(JoinLobbyRequest));
        Context.Handlers.AddActorPacket<CreateParty, PlayerActor>(nameof(CreatePartyRequest));
        Context.Handlers.AddActorPacket<JoinParty, PlayerActor>(nameof(JoinPartyRequest));
    }

    public ValueTask OnJoinedActorAsync(PlayerActor actor, CancellationToken ct) =>
        ValueTask.CompletedTask;

    public ValueTask OnLeaveActorAsync(PlayerActor actor, CancellationToken ct) =>
        ValueTask.CompletedTask;

    public async ValueTask JoinAsync(
        PlayerActor actor,
        string partyId,
        bool create,
        CancellationToken ct
    )
    {
        if (string.IsNullOrWhiteSpace(partyId) || (!create && await spots.FindAsync(partyId, ct) is null))
        {
            await actor
                .Context.BoundSession.Send(new ActionRejected(Rejection.InvalidParty))
                .Async();
            return;
        }
        var id = partyId;
        if (create)
        {
            var result = await spots.GetOrCreate(id, GameNames.PartyType).InMesh(GameNames.WorldMesh).Async(ct);
            if (result.State == ZLinkSpotCreateState.Rejected)
                throw new InvalidOperationException("Party creation rejected.");
        }
        actor.Context.JoinSpot(id, new PartyJoin(partyId, create)).Defer();
    }
}

public sealed class LobbyLogin
    : IZLinkEntrySpotActorSendHandler<LobbyEntry, PlayerActor, JoinLobbyRequest>
{
    public ValueTask HandleAsync(
        LobbyEntry spot,
        PlayerActor actor,
        IZLinkMessageContext context,
        JoinLobbyRequest request,
        CancellationToken ct
    )
    {
        if (request.PlayerId != actor.Context.ActorId)
            throw new InvalidDataException("Player identity mismatch.");
        return actor.Context.BoundSession.Send(new LobbyView()).Async();
    }
}

public sealed class CreateParty
    : IZLinkEntrySpotActorSendHandler<LobbyEntry, PlayerActor, CreatePartyRequest>
{
    public ValueTask HandleAsync(
        LobbyEntry spot,
        PlayerActor actor,
        IZLinkMessageContext context,
        CreatePartyRequest request,
        CancellationToken ct
    ) => spot.JoinAsync(actor, GameNames.NewPartyId(), true, ct);
}

public sealed class JoinParty
    : IZLinkEntrySpotActorSendHandler<LobbyEntry, PlayerActor, JoinPartyRequest>
{
    public ValueTask HandleAsync(
        LobbyEntry spot,
        PlayerActor actor,
        IZLinkMessageContext context,
        JoinPartyRequest request,
        CancellationToken ct
    ) => spot.JoinAsync(actor, request.PartyId, false, ct);
}
