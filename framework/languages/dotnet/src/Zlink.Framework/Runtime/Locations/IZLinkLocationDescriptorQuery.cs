namespace Zlink.Framework.Runtime.Locations;

internal interface IZLinkLocationDescriptorQuery
{
    ValueTask<ZLinkLocationPage<ZLinkMeshNodeDescriptor>> ListMeshNodeDescriptorsAsync(
        string meshName,
        ZLinkPageRequest page = default,
        CancellationToken cancellationToken = default);
}
