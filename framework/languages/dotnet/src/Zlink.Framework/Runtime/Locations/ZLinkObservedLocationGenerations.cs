namespace Zlink.Framework.Runtime.Locations;

internal sealed class ZLinkObservedLocationGenerations
{
    internal bool AcceptDescriptor(ZLinkMeshNodeDescriptor row) => true;

    internal bool AcceptDescriptor(
        ZLinkMeshNodeDescriptor row,
        out bool rejectedByOlderRevision)
    {
        rejectedByOlderRevision = false;
        return true;
    }

    internal void ObserveDescriptor(ZLinkMeshNodeDescriptor row) { }

    internal bool AcceptSpot(ZLinkResolvedSpotLocation row) => true;

    internal bool AcceptActor(ZLinkResolvedActorLocation row) => true;

    internal void ReconcileDescriptors(
        string meshName,
        IReadOnlyList<ZLinkMeshNodeDescriptor> rows) { }

    internal void ForgetActor(ZLinkActorLocationKey key) { }

    internal void ForgetSpot(ZLinkSpotLocationKey key) { }
}
