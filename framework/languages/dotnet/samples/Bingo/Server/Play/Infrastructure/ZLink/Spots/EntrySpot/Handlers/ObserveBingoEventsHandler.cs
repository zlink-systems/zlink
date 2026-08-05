using Bingo.Server.Configuration;
using Bingo.Server.Play.Domain.Bingo;
using Bingo.Server.Play.Infrastructure.ZLink.Actors;
using Bingo.Server.Play.Infrastructure.ZLink.Spots.BingoRoomSpot;
using Bingo.Shared.Contracts;
using Zlink.Framework.Contracts.Actors;
using Zlink.Framework.Contracts.Handlers;
using Zlink.Framework.Contracts.Spots;

namespace Bingo.Server.Play.Infrastructure.ZLink.Spots.EntrySpot.Handlers;

internal sealed class ObserveBingoEventsHandler(IZLinkSpotManager spots)
    : IZLinkEntrySpotActorSendHandler<BingoEntrySpot, PlayerActor, ObserveBingoEventsReq>
{
    public async ValueTask HandleAsync(
        BingoEntrySpot entrySpot,
        PlayerActor actor,
        IZLinkMessageContext context,
        ObserveBingoEventsReq message,
        CancellationToken cancellationToken)
    {
        var observerSpotId = ObserverSpotId(message.RoomId, actor.ActorId);
        var settings = BingoRoomSettings.CreateObserver(message.RoomId, actor.ActorId);
        _ = await spots
            .GetOrCreate(observerSpotId, SampleNames.RoomSpotType)
            .InMesh(SampleNames.PlayMeshName)
            .Request(BingoRoomSettingsPayloadMapper.ToPayload(settings))
            .Async(cancellationToken);

        actor.TrackDeferredJoin(observerSpotId, observeOnly: true);
        actor.Context.JoinSpot(
                observerSpotId,
                new BingoRoomJoinReq
                {
                    RoomId = message.RoomId,
                    ActorId = actor.ActorId,
                    DisplayName = actor.DisplayName,
                    ObserveOnly = true
                })
            .Defer();
    }

    private static string ObserverSpotId(string roomId, string observerActorId)
    {
        return $"observe:{roomId}:{observerActorId}";
    }
}
