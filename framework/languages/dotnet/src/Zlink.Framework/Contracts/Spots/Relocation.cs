namespace Zlink.Framework.Contracts.Spots;

public interface IZLinkSpotRelocationAdapter<TSpot>
    where TSpot : class
{
    ValueTask<byte[]> CaptureAsync(
        TSpot spot,
        CancellationToken cancellationToken);

    ValueTask RestoreAsync(
        TSpot spot,
        ReadOnlyMemory<byte> payload,
        CancellationToken cancellationToken);
}
