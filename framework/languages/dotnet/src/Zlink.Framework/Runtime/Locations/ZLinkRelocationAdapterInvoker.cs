using Microsoft.Extensions.DependencyInjection;

namespace Zlink.Framework.Runtime.Locations;

internal interface IZLinkRelocationAdapterInvoker
{
    ValueTask<byte[]> CaptureAsync(
        IServiceProvider services,
        object instance,
        CancellationToken cancellationToken);

    ValueTask RestoreAsync(
        IServiceProvider services,
        object instance,
        ReadOnlyMemory<byte> payload,
        CancellationToken cancellationToken);
}

internal sealed class ZLinkSpotRelocationAdapterInvoker<TSpot>(Type adapterType)
    : IZLinkRelocationAdapterInvoker
    where TSpot : class
{
    public ValueTask<byte[]> CaptureAsync(
        IServiceProvider services,
        object instance,
        CancellationToken cancellationToken)
    {
        return Resolve(services).CaptureAsync(
            (TSpot)instance,
            cancellationToken);
    }

    public ValueTask RestoreAsync(
        IServiceProvider services,
        object instance,
        ReadOnlyMemory<byte> payload,
        CancellationToken cancellationToken)
    {
        return Resolve(services).RestoreAsync(
            (TSpot)instance,
            payload,
            cancellationToken);
    }

    private IZLinkSpotRelocationAdapter<TSpot> Resolve(IServiceProvider services)
    {
        return (IZLinkSpotRelocationAdapter<TSpot>)
            services.GetRequiredService(adapterType);
    }
}

internal sealed class ZLinkActorRelocationAdapterInvoker<TActor>(Type adapterType)
    : IZLinkRelocationAdapterInvoker
    where TActor : class, IZLinkActor
{
    public ValueTask<byte[]> CaptureAsync(
        IServiceProvider services,
        object instance,
        CancellationToken cancellationToken)
    {
        return Resolve(services).CaptureAsync(
            (TActor)instance,
            cancellationToken);
    }

    public ValueTask RestoreAsync(
        IServiceProvider services,
        object instance,
        ReadOnlyMemory<byte> payload,
        CancellationToken cancellationToken)
    {
        return Resolve(services).RestoreAsync(
            (TActor)instance,
            payload,
            cancellationToken);
    }

    private IZLinkActorRelocationAdapter<TActor> Resolve(IServiceProvider services)
    {
        return (IZLinkActorRelocationAdapter<TActor>)
            services.GetRequiredService(adapterType);
    }
}
