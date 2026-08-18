using Microsoft.Extensions.DependencyInjection;
using Zlink.Framework.Contracts.Actors;
using Zlink.Framework.Contracts.Spots;

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

/// <summary>
/// Optional capability implemented by an <see cref="IZLinkRelocationAdapterInvoker"/>
/// whose registered adapter also implements the base/delta relocation
/// contract (spec 15 §5). Detected once at registration time (compile-time
/// generic constraint), not by runtime reflection on every relocation.
/// </summary>
internal interface IZLinkBaseDeltaRelocationAdapterInvoker
    : IZLinkRelocationAdapterInvoker
{
    ValueTask<byte[]> CaptureBaseAsync(
        IServiceProvider services,
        object instance,
        CancellationToken cancellationToken);

    ValueTask<byte[]> CaptureDeltaAsync(
        IServiceProvider services,
        object instance,
        CancellationToken cancellationToken);

    ValueTask RestoreBaseAsync(
        IServiceProvider services,
        object instance,
        ReadOnlyMemory<byte> basePayload,
        CancellationToken cancellationToken);

    ValueTask ApplyDeltaAsync(
        IServiceProvider services,
        object instance,
        ReadOnlyMemory<byte> deltaPayload,
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

/// <summary>
/// Base/delta-capable Spot relocation adapter invoker (spec 15 §5): the
/// registered adapter implements <see cref="IZLinkSpotBaseDeltaRelocationAdapter{TSpot}"/>.
/// </summary>
internal sealed class ZLinkSpotBaseDeltaRelocationAdapterInvoker<TSpot>(
    Type adapterType)
    : IZLinkBaseDeltaRelocationAdapterInvoker
    where TSpot : class
{
    public ValueTask<byte[]> CaptureAsync(
        IServiceProvider services,
        object instance,
        CancellationToken cancellationToken) =>
        Resolve(services).CaptureAsync((TSpot)instance, cancellationToken);

    public ValueTask RestoreAsync(
        IServiceProvider services,
        object instance,
        ReadOnlyMemory<byte> payload,
        CancellationToken cancellationToken) =>
        Resolve(services).RestoreAsync(
            (TSpot)instance, payload, cancellationToken);

    public ValueTask<byte[]> CaptureBaseAsync(
        IServiceProvider services,
        object instance,
        CancellationToken cancellationToken) =>
        Resolve(services).CaptureBaseAsync(
            (TSpot)instance, cancellationToken);

    public ValueTask<byte[]> CaptureDeltaAsync(
        IServiceProvider services,
        object instance,
        CancellationToken cancellationToken) =>
        Resolve(services).CaptureDeltaAsync(
            (TSpot)instance, cancellationToken);

    public ValueTask RestoreBaseAsync(
        IServiceProvider services,
        object instance,
        ReadOnlyMemory<byte> basePayload,
        CancellationToken cancellationToken) =>
        Resolve(services).RestoreBaseAsync(
            (TSpot)instance, basePayload, cancellationToken);

    public ValueTask ApplyDeltaAsync(
        IServiceProvider services,
        object instance,
        ReadOnlyMemory<byte> deltaPayload,
        CancellationToken cancellationToken) =>
        Resolve(services).ApplyDeltaAsync(
            (TSpot)instance, deltaPayload, cancellationToken);

    private IZLinkSpotBaseDeltaRelocationAdapter<TSpot> Resolve(
        IServiceProvider services) =>
        (IZLinkSpotBaseDeltaRelocationAdapter<TSpot>)
            services.GetRequiredService(adapterType);
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

/// <summary>
/// Base/delta-capable Actor relocation adapter invoker (spec 15 §5): the
/// registered adapter implements <see cref="IZLinkActorBaseDeltaRelocationAdapter{TActor}"/>.
/// </summary>
internal sealed class ZLinkActorBaseDeltaRelocationAdapterInvoker<TActor>(
    Type adapterType)
    : IZLinkBaseDeltaRelocationAdapterInvoker
    where TActor : class, IZLinkActor
{
    public ValueTask<byte[]> CaptureAsync(
        IServiceProvider services,
        object instance,
        CancellationToken cancellationToken) =>
        Resolve(services).CaptureAsync((TActor)instance, cancellationToken);

    public ValueTask RestoreAsync(
        IServiceProvider services,
        object instance,
        ReadOnlyMemory<byte> payload,
        CancellationToken cancellationToken) =>
        Resolve(services).RestoreAsync(
            (TActor)instance, payload, cancellationToken);

    public ValueTask<byte[]> CaptureBaseAsync(
        IServiceProvider services,
        object instance,
        CancellationToken cancellationToken) =>
        Resolve(services).CaptureBaseAsync(
            (TActor)instance, cancellationToken);

    public ValueTask<byte[]> CaptureDeltaAsync(
        IServiceProvider services,
        object instance,
        CancellationToken cancellationToken) =>
        Resolve(services).CaptureDeltaAsync(
            (TActor)instance, cancellationToken);

    public ValueTask RestoreBaseAsync(
        IServiceProvider services,
        object instance,
        ReadOnlyMemory<byte> basePayload,
        CancellationToken cancellationToken) =>
        Resolve(services).RestoreBaseAsync(
            (TActor)instance, basePayload, cancellationToken);

    public ValueTask ApplyDeltaAsync(
        IServiceProvider services,
        object instance,
        ReadOnlyMemory<byte> deltaPayload,
        CancellationToken cancellationToken) =>
        Resolve(services).ApplyDeltaAsync(
            (TActor)instance, deltaPayload, cancellationToken);

    private IZLinkActorBaseDeltaRelocationAdapter<TActor> Resolve(
        IServiceProvider services) =>
        (IZLinkActorBaseDeltaRelocationAdapter<TActor>)
            services.GetRequiredService(adapterType);
}
