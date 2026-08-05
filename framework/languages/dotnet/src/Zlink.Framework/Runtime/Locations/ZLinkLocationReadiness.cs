namespace Zlink.Framework.Runtime.Locations;

internal sealed class ZLinkLocationReadiness(IZLinkLocationRuntimeQuery query) : IZLinkLocationReadiness
{
    public async ValueTask<bool> IsPeerReadyAsync(
        string meshName,
        ZLinkLocationRole role,
        RoutingId? nodeRid = null,
        CancellationToken cancellationToken = default)
    {
        try
        {
            var page = await query.ListTopologyAsync(
                    new ZLinkLocationTopologyFilter(
                        MeshName: meshName,
                        NodeRid: nodeRid,
                        State: ZLinkLocationTopologyState.Ready),
                    cancellationToken: cancellationToken)
                .ConfigureAwait(false);
            return page.Items.Count > 0;
        }
        catch
        {
            return false;
        }
    }
}
