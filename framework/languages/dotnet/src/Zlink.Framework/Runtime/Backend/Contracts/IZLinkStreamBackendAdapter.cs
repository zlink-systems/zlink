namespace Zlink.Framework.Runtime.Backend.Contracts;

internal interface IZLinkStreamBackendAdapter
{
    // STREAM owns its transport node independently. Actor dispatch resolves each
    // ActorRef through its global authority and uses that ref's MeshName; it does
    // not pin the STREAM socket to one process-local Object MeshNode.
    // standaloneMeshName names the fallback MeshNode minted when actorDispatchNode
    // is null; Core requires a non-empty mesh name at construction (EINVAL otherwise).
    IZLinkBackendStreamSocket CreateStreamSocket(
        IZLinkBackendContext context,
        string standaloneMeshName,
        IZLinkBackendSpotNode? actorDispatchNode = null);
}
