namespace Zlink.Framework.Runtime.Spots;

internal sealed class ZLinkSpotActorJoinRegistry
{
    private ZLinkSpotActorJoinDescriptor? _descriptor;

    public void Bind(object spot)
    {
        foreach (var descriptor in ZLinkSpotDescriptorFactory.CreateSpotActorJoinDescriptors(spot.GetType()))
            AddDescriptor(spot, descriptor);
    }

    private void AddDescriptor(object spot, ZLinkSpotActorJoinDescriptor descriptor)
    {
        if (_descriptor is not null)
            throw new InvalidOperationException(
                $"SPOT actor join callback is already registered on '{spot.GetType()}'.");

        _descriptor = descriptor;
    }

    public bool TryResolve(out ZLinkSpotActorJoinDescriptor? descriptor)
    {
        descriptor = _descriptor;
        return descriptor is not null;
    }
}
