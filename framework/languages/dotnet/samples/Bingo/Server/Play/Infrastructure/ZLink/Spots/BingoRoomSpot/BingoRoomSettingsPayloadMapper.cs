using Bingo.Server.Play.Domain.Bingo;
using Bingo.Shared.Contracts;
using Zlink.Framework.Contracts.Messaging;

namespace Bingo.Server.Play.Infrastructure.ZLink.Spots.BingoRoomSpot;

internal static class BingoRoomSettingsPayloadMapper
{
    public static BingoRoomSettings FromMessage(ZLinkMessage request, BingoRoomSettings defaultSettings)
    {
        if (request.IsEmpty) return defaultSettings;

        var payload = request.Decode<BingoRoomSettingsPayload>();
        return new BingoRoomSettings(
            payload.RoomName,
            payload.Mode,
            payload.RequiredPlayers,
            payload.MaxDrawNumber,
            string.IsNullOrWhiteSpace(payload.Purpose) ? BingoRoomSettings.GamePurpose : payload.Purpose,
            payload.HasObservedRoomId ? payload.ObservedRoomId : null);
    }

    public static BingoRoomSettingsPayload ToPayload(BingoRoomSettings settings)
    {
        var payload = new BingoRoomSettingsPayload
        {
            RoomName = settings.RoomName,
            Mode = settings.Mode,
            RequiredPlayers = settings.RequiredPlayers,
            MaxDrawNumber = settings.MaxDrawNumber,
            Purpose = settings.Purpose
        };

        if (settings.ObservedRoomId is not null) payload.ObservedRoomId = settings.ObservedRoomId;

        return payload;
    }
}