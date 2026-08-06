namespace Zlink.Framework.Runtime.Backend.Contracts;

/// <summary>
/// Owns the binding runtime context and creates the backend resources used by
/// one Framework runtime generation. The semantic runtime sees only backend
/// contracts; binding context options and shutdown ordering stay behind this
/// port.
/// </summary>
internal interface IZLinkBackendRuntimeContext : IAsyncDisposable
{
    void ConfigureAutoHwm(ZLinkApplicationHwmProfile profile);

    IZLinkBackendDealerSocket CreateDealerSocket();

    IZLinkBackendRouterSocket CreateRouterSocket();

    IZLinkBackendPublisherSocket CreatePublisherSocket();

    IZLinkBackendSubscriberSocket CreateSubscriberSocket();

    IZLinkBackendSpotNode CreateSpotNode(string meshName);

    IZLinkBackendStreamSocket CreateStreamSocket(
        string standaloneMeshName,
        IZLinkBackendSpotNode? actorDispatchNode = null);
}
