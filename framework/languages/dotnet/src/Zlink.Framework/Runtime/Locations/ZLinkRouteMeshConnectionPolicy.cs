namespace Zlink.Framework.Runtime.Locations;

internal static class ZLinkRouteMeshConnectionPolicy
{
    internal static bool IsNotRequired(
        ZLinkMeshNodeObjectRole localObjectRole,
        bool localHasServerChannel,
        ZLinkMeshNodeObjectRole remoteObjectRole,
        bool remoteHasServerChannel) =>
        localObjectRole == ZLinkMeshNodeObjectRole.Client
        && !localHasServerChannel
        && remoteObjectRole == ZLinkMeshNodeObjectRole.Client
        && !remoteHasServerChannel;
}
