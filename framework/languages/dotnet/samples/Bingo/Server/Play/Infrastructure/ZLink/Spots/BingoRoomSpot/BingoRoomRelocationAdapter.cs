using System.Text.Json;
using Bingo.Server.Play.Domain.Bingo;
using Bingo.Shared.Contracts;
using Google.Protobuf;
using Zlink.Framework.Contracts.Spots;

namespace Bingo.Server.Play.Infrastructure.ZLink.Spots.BingoRoomSpot;

internal sealed class BingoRoomRelocationAdapter
    : IZLinkSpotRelocationAdapter<BingoRoom>
{
    public ValueTask<byte[]> CaptureAsync(
        BingoRoom spot,
        CancellationToken cancellationToken)
    {
        cancellationToken.ThrowIfCancellationRequested();
        var state = spot.CaptureRelocationState();
        return ValueTask.FromResult(JsonSerializer.SerializeToUtf8Bytes(
            new Payload(
                state.Settings.RoomName,
                state.Settings.Mode,
                state.Settings.RequiredPlayers,
                state.Settings.MaxDrawNumber,
                state.Settings.Purpose,
                state.Settings.ObservedRoomId,
                Convert.ToBase64String(state.State.ToByteArray()))));
    }

    public ValueTask RestoreAsync(
        BingoRoom spot,
        ReadOnlyMemory<byte> payload,
        CancellationToken cancellationToken)
    {
        cancellationToken.ThrowIfCancellationRequested();
        var state = JsonSerializer.Deserialize<Payload>(payload.Span)
                    ?? throw new InvalidDataException("Bingo room relocation payload is empty.");
        spot.RestoreRelocationState(
            new BingoRoomSettings(
                state.RoomName,
                state.Mode,
                state.RequiredPlayers,
                state.MaxDrawNumber,
                state.Purpose,
                state.ObservedRoomId),
            BingoRoomState.Parser.ParseFrom(Convert.FromBase64String(state.State)));
        return ValueTask.CompletedTask;
    }

    private sealed record Payload(
        string RoomName,
        string Mode,
        int RequiredPlayers,
        int MaxDrawNumber,
        string Purpose,
        string? ObservedRoomId,
        string State);
}
