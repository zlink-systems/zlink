namespace Zlink.Framework.Runtime.Locations;

internal interface IZLinkMeshNodeLocationResolver
{
    ValueTask<IReadOnlyList<ZLinkMeshNodeDescriptor>> ListLiveMeshNodesAsync(
        string meshName,
        CancellationToken cancellationToken = default);
}
