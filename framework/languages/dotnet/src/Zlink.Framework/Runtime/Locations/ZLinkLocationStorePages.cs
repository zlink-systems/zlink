namespace Zlink.Framework.Runtime.Locations;

internal static class ZLinkLocationStorePages
{
    internal static async ValueTask<IReadOnlyList<ZLinkMeshNodeDescriptor>>
        ListAllMeshNodesAsync(
            this IZLinkLocationRepository store,
            string meshName,
            CancellationToken cancellationToken = default)
    {
        for (var attempt = 0; attempt < 4; attempt++)
        {
            var rows = new List<ZLinkMeshNodeDescriptor>();
            string? continuationToken = null;
            try
            {
                do
                {
                    var page = await store.ListMeshNodesAsync(
                            meshName,
                            new ZLinkPageRequest(1000, continuationToken),
                            cancellationToken)
                        .ConfigureAwait(false);
                    rows.AddRange(page.Items);
                    continuationToken = page.ContinuationToken;
                } while (continuationToken is not null);

                return rows;
            }
            catch (ZLinkLocationSnapshotExpiredException)
                when (attempt < 3)
            {
                // The discarded rows belong to an expired snapshot. The
                // retry starts from a new provider snapshot.
            }
        }

        throw new ZLinkLocationSnapshotExpiredException();
    }
}
