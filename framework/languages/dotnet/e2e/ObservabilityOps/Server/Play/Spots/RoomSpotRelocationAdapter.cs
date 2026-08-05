using System.Text.Json;
using Zlink.Framework.Contracts.Spots;

namespace ObservabilityOps.Server.Play.Spots;

internal sealed record RoomSpotRelocationState(DateTimeOffset? AutoCloseAfter);

internal sealed class RoomSpotRelocationAdapter
    : IZLinkSpotRelocationAdapter<RoomSpot>
{
    public ValueTask<byte[]> CaptureAsync(
        RoomSpot spot,
        CancellationToken cancellationToken)
    {
        cancellationToken.ThrowIfCancellationRequested();
        return ValueTask.FromResult(JsonSerializer.SerializeToUtf8Bytes(
            new RoomSpotRelocationState(spot.AutoCloseAfter)));
    }

    public ValueTask RestoreAsync(
        RoomSpot spot,
        ReadOnlyMemory<byte> payload,
        CancellationToken cancellationToken)
    {
        cancellationToken.ThrowIfCancellationRequested();
        var state = JsonSerializer.Deserialize<RoomSpotRelocationState>(payload.Span)
                    ?? throw new InvalidDataException(
                        "Room Spot relocation state is empty.");
        spot.AutoCloseAfter = state.AutoCloseAfter;
        return ValueTask.CompletedTask;
    }
}
