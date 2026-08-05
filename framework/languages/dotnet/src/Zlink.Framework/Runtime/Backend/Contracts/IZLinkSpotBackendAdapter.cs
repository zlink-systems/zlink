namespace Zlink.Framework.Runtime.Backend.Contracts;

internal interface IZLinkSpotBackendAdapter
{
    // Core requires a non-empty mesh membership name at MeshNode construction
    // (zlink_mesh_node_new → EINVAL otherwise), so the mesh name is threaded from
    // the RouteMesh registration (AddRouteMesh(meshName)) rather than minted bare.
    IZLinkBackendSpotNode CreateSpotNode(IZLinkBackendContext context, string meshName);
}
