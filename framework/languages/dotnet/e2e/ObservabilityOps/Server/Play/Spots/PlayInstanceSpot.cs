using System.Text;
using ObservabilityOps.Server.Play.Support;
using ObservabilityOps.Shared;
using Zlink.Framework.Contracts.Spots;

namespace ObservabilityOps.Server.Play.Spots;

internal sealed class PlayInstanceSpot(
    IZLinkInstanceSpotContext context,
    EvidenceStore evidence) : IZLinkInstanceSpot
{
    public IZLinkInstanceSpotContext Context { get; } = context;

    public string Marker { get; internal set; } = string.Empty;

    public ValueTask OnInitializeAsync(CancellationToken cancellationToken)
    {
        cancellationToken.ThrowIfCancellationRequested();
        evidence.Add($"instance-initialized|spot={Context.SpotId}|node={Context.NodeRid}");
        return ValueTask.CompletedTask;
    }

    public ValueTask OnClosingAsync(
        ZLinkSpotClosingContext context,
        CancellationToken cleanupCancellationToken)
    {
        cleanupCancellationToken.ThrowIfCancellationRequested();
        evidence.Add(
            $"spot-closing|kind=instance|spot={Context.SpotId}"
            + $"|node={Context.NodeRid}|reason={context.Reason}");
        return ValueTask.CompletedTask;
    }
}

internal sealed class ActivateInstanceSpotHandler
    : IZLinkSpotRequestHandler<
        PlayInstanceSpot,
        ActivateInstanceSpotReq,
        ActivateInstanceSpotRes>
{
    public ValueTask<ActivateInstanceSpotRes> HandleAsync(
        PlayInstanceSpot spot,
        ActivateInstanceSpotReq request,
        CancellationToken cancellationToken)
    {
        cancellationToken.ThrowIfCancellationRequested();
        spot.Marker = request.Marker;
        return ValueTask.FromResult(new ActivateInstanceSpotRes(
            spot.Context.SpotId,
            spot.Context.NodeRid.ToString(),
            spot.Marker));
    }
}

internal sealed class PlayInstanceSpotRelocationAdapter
    : IZLinkSpotRelocationAdapter<PlayInstanceSpot>
{
    public ValueTask<byte[]> CaptureAsync(
        PlayInstanceSpot spot,
        CancellationToken cancellationToken)
    {
        cancellationToken.ThrowIfCancellationRequested();
        return ValueTask.FromResult(Encoding.UTF8.GetBytes(spot.Marker));
    }

    public ValueTask RestoreAsync(
        PlayInstanceSpot spot,
        ReadOnlyMemory<byte> payload,
        CancellationToken cancellationToken)
    {
        cancellationToken.ThrowIfCancellationRequested();
        spot.Marker = Encoding.UTF8.GetString(payload.Span);
        return ValueTask.CompletedTask;
    }
}
